/*
 * FrameTee's small platform layer for the full SM64 PC port.  It deliberately
 * owns no window, controller or audio device: FrameTee supplies controllers,
 * snapshots the library's mutable sections, and implements GfxRenderingAPI.
 */
#include "sm64.h"
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "scene.h"
#include "sm64_physics.h"
#include "buffers/buffers.h"

// The bridge supports the native 64-bit runtime layout, not N64 RAM offsets.
// Fail a future runtime build if a source/layout change needs a bridge update.
_Static_assert(offsetof(struct MarioState, action) == 0x0c, "Mario action ABI");
_Static_assert(offsetof(struct MarioState, pos) == 0x3c, "Mario position ABI");
_Static_assert(offsetof(struct MarioState, vel) == 0x48, "Mario velocity ABI");
_Static_assert(offsetof(struct MarioState, health) == 0xea, "Mario health ABI");

#include "audio/external.h"
#include "game/game_init.h"
#include "game/main.h"
#include "game/memory.h"
#include "game/area.h"
#include "game/camera.h"
#include "game/mario.h"
#include "game/level_update.h"
#include "game/rendering_graph_node.h"
#include "engine/math_util.h"
#include "engine/graph_node.h"
#include "pc/gfx/gfx_pc.h"
#include "pc/gfx/gfx_rendering_api.h"
#include "pc/gfx/gfx_window_manager_api.h"

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;
s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;
s32 gRumblePakPfs;
struct RumbleData gRumbleDataQueue[3];
struct StructSH8031D9B0 gCurrRumbleSettings;

int sm64_simulating;
// Presentation-facing worlds keep native object animation in lockstep with
// their physics tick. Isolated physics workers disable this switch so direct
// simulation does not spend time walking render-only animation state.
int sm64_process_animations = 1;
// Automatic course setup needs SM64's menu/transition state machine to run
// while the regular physics path remains presentation-free.
int sm64_starting;
static Gfx *display_list;
static uint32_t render_width = 320;
static uint32_t render_height = 240;
static sm64_scene_camera scene_camera;
static sm64_scene_config scene_config;
static bool scene_needs_refresh;
static float display_list_aspect = 4.f / 3.f;
static int display_list_mode = -1;

static void matrix_multiply(float out[16], const float a[16], const float b[16]) {
    float result[16] = {0};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    memcpy(out, result, sizeof(result));
}

// Called by the scene walker after it constructs the normal projection. The
// normal camera is recorded during simulation; custom rendering never replaces
// that simulation-owned record.
void sm64_scene_projection(Mtx *matrix, u16 *persp_norm) {
    if (scene_config.mode) {
        guMtxF2L((float (*)[4])scene_config.projection, matrix);
        *persp_norm = 0xffff;
    } else {
        // The PC port's Mtx is already floating point (as is guMtxF2L).
        memcpy(scene_camera.projection, matrix->m, sizeof(scene_camera.projection));
    }
}

void sm64_scene_view(struct GraphNodeCamera *node, Mat4 view, Mtx *roll) {
    if (scene_config.mode) {
        memcpy(view, scene_config.view, sizeof(Mat4));
        memcpy(node->pos, scene_config.eye, sizeof(node->pos));
        memcpy(node->focus, scene_config.target, sizeof(node->focus));
        node->roll = node->rollScreen = 0;
        mtxf_rotate_xy(roll, 0);
    } else {
        float rotation[16];
        memcpy(rotation, roll->m, sizeof(rotation));
        matrix_multiply(scene_camera.projection, scene_camera.projection, rotation);
        memcpy(scene_camera.view, view, sizeof(Mat4));
        memcpy(scene_camera.eye, node->pos, sizeof(node->pos));
        memcpy(scene_camera.target, node->focus, sizeof(node->focus));
        scene_camera.valid = true;
    }
}

// The original object culler assumes a 45-degree perspective lens. Use the
// actual six projection planes for editor cameras, including orthographic ones.
int sm64_scene_object_visible(Mat4 matrix, float radius) {
    if (!scene_config.mode) return -1;
    const float *p = scene_config.projection;
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign = -1; sign <= 1; sign += 2) {
            float plane[4];
            const float aspect_scale = axis == 0 ? (4.f/3.f) * scene_config.height / scene_config.width : 1.f;
            for (int col = 0; col < 4; ++col) plane[col] = p[col * 4 + 3] + sign * p[col * 4 + axis] * aspect_scale;
            const float distance = plane[0] * matrix[3][0] + plane[1] * matrix[3][1] + plane[2] * matrix[3][2] + plane[3];
            const float extent = radius * sqrtf(plane[0]*plane[0] + plane[1]*plane[1] + plane[2]*plane[2]);
            if (distance < -extent) return 0;
        }
    }
    return 1;
}

bool sm64_get_scene_camera(sm64_scene_camera *out) {
    if (!out) return false;
    *out = scene_camera;
    return out->valid;
}

void game_loop_one_iteration(void);

void dispatch_audio_sptask(struct SPTask *task) { (void)task; }
void set_vblank_handler(s32 index, struct VblankHandler *handler, OSMesgQueue *queue, OSMesg *message) {
    (void)index;
    (void)handler;
    (void)queue;
    (void)message;
}

// The standard PC controller entry point polls SDL/keyboard devices and clears
// gControllerPads before every game tick.  FrameTee writes that exported array
// immediately before sm64_update(), so the headless platform preserves it.
s32 osContInit(UNUSED OSMesgQueue *queue, u8 *controller_bits, UNUSED OSContStatus *status) {
    (void)queue;
    *controller_bits = 1;
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue *message) {
    (void)message;
    return 0;
}

void osContGetReadData(OSContPad *pad) { (void)pad; }
s32 osMotorStart(UNUSED void *pfs) { (void)pfs; return 0; }
s32 osMotorStop(UNUSED void *pfs) { (void)pfs; return 0; }
u32 osMotorInit(UNUSED OSMesgQueue *queue, UNUSED void *pfs, UNUSED s32 port) {
    (void)queue;
    (void)pfs;
    (void)port;
    return 0;
}
u32 controller_get_raw_key(void) { return 0xffffffffu; }
void controller_reconfigure(void) {}

void send_display_list(struct SPTask *task) {
    display_list = (Gfx *)task->task.t.data_ptr;
}

static void window_init(const char *title) { (void)title; }
static void window_keyboard(kb_callback_t down, kb_callback_t up, void (*all_up)(void)) {
    (void)down;
    (void)up;
    (void)all_up;
}
static void window_loop(void (*iterate)(void)) { (void)iterate; }
static void window_dimensions(uint32_t *width, uint32_t *height) {
    *width = render_width;
    *height = render_height;
}
static void window_events(void) {}
static bool window_start(void) { return true; }
static void window_void(void) {}
static double window_time(void) { return 0.0; }

static struct GfxWindowManagerAPI window_api = {
    window_init,
    window_keyboard,
    window_loop,
    window_dimensions,
    window_events,
    window_start,
    window_void,
    window_void,
    window_time,
    window_void,
};

void sm64_init(void) {
    gfx_current_dimensions.width = 320;
    gfx_current_dimensions.height = 240;
    gfx_current_dimensions.aspect_ratio = 4.f / 3.f;
    display_list_aspect = 4.f / 3.f;
    display_list_mode = 0;
    static u64 pool[365568];
    main_pool_init(pool, pool + sizeof(pool) / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);
    audio_init();
    sound_init();
    thread5_game_loop(NULL);
}

void sm64_update(void) {
    scene_config.mode = 0;
    if (render_width > 0 && render_height > 0) {
        gfx_current_dimensions.width = render_width;
        gfx_current_dimensions.height = render_height;
        gfx_current_dimensions.aspect_ratio = (float)render_width / (float)render_height;
    } else {
        gfx_current_dimensions.width = 320;
        gfx_current_dimensions.height = 240;
        gfx_current_dimensions.aspect_ratio = 4.f / 3.f;
    }
    sm64_simulating = 1;
    game_loop_one_iteration();
    sm64_simulating = 0;
    display_list_aspect = gfx_current_dimensions.aspect_ratio;
    display_list_mode = 0;
    scene_needs_refresh = true;
}

// Startup uses the original full game-loop presentation cadence so the intro
// script can consume its menu and transition state. FrameTee never exposes
// this path to normal physics ticks.
void sm64_update_startup(void) {
    sm64_simulating = 0;
    game_loop_one_iteration();
    sm64_simulating = 0;
}

// Replayable state edits update the simulation's coupled fields, not only the
// reflected vector. A subsequent render rebuilds geometry without taking a tick.
bool sm64_edit_mario(uint32_t property, const uint32_t value[3]) {
    struct MarioState *mario = gMarioState;
    if (!mario || !mario->action || !mario->marioObj) return false;
    float vector[3];
    memcpy(vector, value, sizeof(vector));
    switch (property) {
        case 0:
            vec3f_copy(mario->pos, vector);
            vec3f_copy(mario->marioObj->header.gfx.pos, vector);
            mario->marioObj->oPosX = vector[0];
            mario->marioObj->oPosY = vector[1];
            mario->marioObj->oPosZ = vector[2];
            break;
        case 1:
            vec3f_copy(mario->vel, vector);
            mario->forwardVel = sqrtf(vector[0]*vector[0] + vector[2]*vector[2]);
            mario->slideVelX = vector[0];
            mario->slideVelZ = vector[2];
            if (mario->forwardVel > 0.f) {
                mario->faceAngle[1] = mario->slideYaw = atan2s(vector[2], vector[0]);
                mario->marioObj->header.gfx.angle[1] = mario->faceAngle[1];
            }
            break;
        case 3:
            mario->health = (s16)value[0];
            gHudDisplay.wedges = mario->health >> 8;
            break;
        default: return false;
    }
    scene_needs_refresh = true;
    return true;
}

void game_exit(void) {}

void sm64_set_render_api(struct GfxRenderingAPI *api) {
    gfx_init(&window_api, api, "FrameTee Super Mario 64");
}

void sm64_render_display_list(uint32_t width, uint32_t height) {
    render_width = width;
    render_height = height;
    gfx_start_frame();
    if (display_list) gfx_run(display_list);
    gfx_end_frame();
}

void sm64_prepare_scene(const sm64_scene_config *config) {
    scene_config = *config;
    if (config->width > 0 && config->height > 0) {
        render_width = config->width;
        render_height = config->height;
    }
    float target_aspect = (config->width > 0 && config->height > 0)
        ? (float)config->width / (float)config->height
        : (4.f / 3.f);

    if (!gCurrentArea) return;

    // Display lists are transient, so always rebuild for the requested view.

    if (!config->mode && scene_camera.valid) {
        // Keep the simulation's exact lens/roll even if a render callback
        // temporarily advances a zoom or camera animation during regeneration.
        scene_config.mode = 3;
        scene_config.width = config->width > 0 ? config->width : 320;
        scene_config.height = config->height > 0 ? config->height : 240;
        memcpy(scene_config.view, scene_camera.view, sizeof(scene_camera.view));
        memcpy(scene_config.projection, scene_camera.projection, sizeof(scene_camera.projection));
        memcpy(scene_config.eye, scene_camera.eye, sizeof(scene_camera.eye));
        memcpy(scene_config.target, scene_camera.target, sizeof(scene_camera.target));
    }
    // Rebuild geometry for the editor camera, without input, object updates,
    // HUD/dialog processing or advancing time. The host restores all temporary
    // scene traversal writes afterwards.
    gfx_current_dimensions.width = scene_config.width > 0 ? scene_config.width : 320;
    gfx_current_dimensions.height = scene_config.height > 0 ? scene_config.height : 240;
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
    memcpy(gLakituState.pos, scene_config.eye, sizeof(gLakituState.pos));
    memcpy(gLakituState.focus, scene_config.target, sizeof(gLakituState.focus));
    config_gfx_pool();
    init_render_image();
    if (config->mode || config->scene_only) geo_process_root(gCurrentArea->unk04, NULL, NULL, 0);
    else render_game();
    end_master_display_list();
    send_display_list(&gGfxPool->spTask);
    display_list_aspect = gfx_current_dimensions.aspect_ratio;
    display_list_mode = config->mode;
    scene_needs_refresh = false;
}

// The worker calls this symbol directly in its own loaded native image.
static sm64_view physics_view;
static void physics_inspect(void) {
    struct MarioState *m = gMarioState;
    physics_view.valid = m && m->action && m->marioObj;
    if (m) {
        memcpy(physics_view.pos, m->pos, sizeof(m->pos));
        memcpy(physics_view.vel, m->vel, sizeof(m->vel));
        physics_view.action = m->action;
        physics_view.health = m->health;
    }
}
const sm64_view *sm64_physics_bind(uint32_t frame) {
    physics_view.frame = frame;
    physics_inspect();
    return &physics_view;
}
void sm64_physics_step(sm64_input input) {
    gControllerPads[0].button = input.buttons;
    gControllerPads[0].stick_x = input.stick_x;
    gControllerPads[0].stick_y = input.stick_y;
    const int previous_process_animations = sm64_process_animations;
    sm64_process_animations = 0;
    sm64_update();
    sm64_process_animations = previous_process_animations;
    ++physics_view.frame;
    physics_inspect();
}
// Display-list storage is regenerated before presentation and never saved.
void sm64_render_memory(void **address, size_t *size) {
    *address = gGfxPools;
    *size = sizeof(gGfxPools);
}


bool sm64_get_mario_pose(sm64_scene_mario *out) {
    memset(out, 0, sizeof(*out));
    if (!gMarioState || !gMarioState->action || !gMarioState->marioObj) return false;
    struct GraphNodeObject *gfx = &gMarioState->marioObj->header.gfx;
    memcpy(out->pos, gMarioState->pos, sizeof(out->pos));
    memcpy(out->scale, gfx->scale, sizeof(out->scale));
    memcpy(out->angle, gfx->angle, sizeof(out->angle));
    out->anim_id = gfx->unk38.animID;
    out->anim_frame = gfx->unk38.animFrame;
    out->anim_accel = gfx->unk38.animAccel;
    out->area = gfx->unk18;
    out->valid = true;
    return true;
}

// Called inside the scene's object parent, with its camera and depth intact.
void sm64_draw_ghosts(void (*draw)(struct Object *)) {
    if (!gMarioState || !gMarioState->marioObj) return;
    for (uint32_t i = 0; i < scene_config.ghost_count; ++i) {
        const sm64_scene_mario *pose = &scene_config.ghosts[i];
        if (!pose->valid) continue;
        struct Object ghost = *gMarioState->marioObj;
        ghost.header.gfx.unk38 = gMarioState->marioObj->header.gfx.unk38;
        // Keep the active animation resource pointer, but use the captured
        // animation identity/frame. This avoids mutating gMarioState while a
        // ghost is being emitted.
        ghost.header.gfx.unk38.animID = pose->anim_id;
        ghost.header.gfx.unk38.animFrame = pose->anim_frame;
        ghost.header.gfx.unk38.animAccel = pose->anim_accel;
        ghost.header.gfx.node.flags &= ~GRAPH_RENDER_HAS_ANIMATION;
        ghost.header.gfx.node.children = NULL;
        ghost.header.gfx.throwMatrix = NULL;
        ghost.header.gfx.unk18 = pose->area;
        memcpy(ghost.header.gfx.pos, pose->pos, sizeof(pose->pos));
        memcpy(ghost.header.gfx.scale, pose->scale, sizeof(pose->scale));
        memcpy(ghost.header.gfx.angle, pose->angle, sizeof(pose->angle));
        draw(&ghost);
    }
}
