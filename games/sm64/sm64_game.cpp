#include <frametee/game_abi.h>
#include <cimgui.h>
#include "sm64_bridge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

// Serialize the C boundary, including lifecycle calls. The per-level native session
// also locks activation, stepping and rendering around its loaded game image.
static std::recursive_mutex bridge_mutex;
struct ft_game {
  const ft_engine_api *engine;
  char error[2048]{};
  ft_texture *texture = nullptr;
  ft_pipeline *pipeline = nullptr;
  ft_mesh *quad = nullptr;
  uint32_t width = 0, height = 0;
  bool collision = false;
  bool gpu_failure_reported = false;
  uint64_t rendered_revision = 0;
  sm64_render_config rendered_config{};
  std::vector<uint64_t> rendered_groups;
  std::vector<uint8_t> pixels;
};
static void report(ft_game *g) { if (g->engine->log) g->engine->log(FT_LOG_ERROR, "sm64", g->error); }
static ft_game *create(const ft_engine_api *e) { auto *g = new(std::nothrow) ft_game; if(g) g->engine=e; return g; }
static void resources_destroy(ft_game *g) {
  std::lock_guard lock(bridge_mutex);
  sm64_ft_release_graphics();
  if(g->texture && g->engine->texture_destroy) g->engine->texture_destroy(g->texture);
  if(g->pipeline && g->engine->pipeline_destroy) g->engine->pipeline_destroy(g->pipeline);
  if(g->quad && g->engine->mesh_destroy) g->engine->mesh_destroy(g->quad);
  g->texture=nullptr;g->pipeline=nullptr;g->quad=nullptr;g->width=g->height=0;
  g->rendered_revision=0;
}
static void destroy(ft_game *g) { resources_destroy(g); delete g; }
static ft_level *level_load(ft_game *g,const char *path,const char *) {
  std::lock_guard lock(bridge_mutex);char cache[4096]{};
  if(!path||!g->engine->resolve_cache_path)return nullptr;
  g->engine->resolve_cache_path("backend",cache,sizeof(cache));
  auto *handle=sm64_ft_open(path,cache,g->error,sizeof(g->error));
  if(!handle){report(g);return nullptr;}
  return new ft_level{handle};
}
static void level_destroy(ft_game *,ft_level *l){std::lock_guard lock(bridge_mutex);sm64_ft_level_free(l->handle);delete l;}
static bool level_info(ft_game *,const ft_level *l,ft_level_info *out){
  *out={};out->struct_size=sizeof(*out);out->name=sm64_ft_level_name(l->handle);out->bounds={-8192,-8192,16384,16384};out->width_tiles=out->height_tiles=16384;return true;
}
static ft_world *world_create(ft_game *g,const ft_world_desc *d){
  std::lock_guard lock(bridge_mutex);if(!d||!d->level)return nullptr;
  auto *h=sm64_ft_world_new(d->level->handle,g->error,sizeof(g->error));if(!h){report(g);return nullptr;}
  const bool is_scratch = (d->world_index == -1);
  sm64_ft_world_set_scratch(h, is_scratch);
  auto *w = new ft_world{h, false, d->world_index, {}, sm64_ft_physics_create};
  sm64_ft_view(h, &w->view);
  return w;
}
static void world_destroy(ft_game *,ft_world *w){std::lock_guard lock(bridge_mutex);sm64_ft_world_free(w->handle);delete w;}
static void world_copy(ft_game *,ft_world *d,const ft_world *s){
  std::lock_guard lock(bridge_mutex);
  sm64_ft_copy(d->handle,s->handle);
  d->failed=s->failed;
  d->view=s->view;
}
static void step(ft_game *g,ft_world *w,const void *inputs,uint32_t count){
  std::lock_guard lock(bridge_mutex);if(w->failed||count!=1||!inputs)return;
  if(!sm64_ft_step(w->handle,*static_cast<const sm64_input *>(inputs),g->error,sizeof(g->error))){w->failed=true;report(g);}
  else sm64_ft_view(w->handle,&w->view);
}
static int32_t tick(ft_game *,const ft_world *w){std::lock_guard lock(bridge_mutex);sm64_view v{};return sm64_ft_view(w->handle,&v);}
static int32_t player_count(ft_game *,const ft_world *){return 1;}
static bool player_view(ft_game *,const ft_world *w,int32_t player,ft_player_view *out){
  if(player!=0||!w)return false;
  const sm64_view &v = w->view;
  *out={};out->struct_size=sizeof(*out);out->position={v.pos[0],v.pos[2]};out->velocity={v.vel[0],v.vel[2]};out->run_start_tick=0;
  out->flags=v.valid&&v.health>0x100?FT_PLAYER_ALIVE:FT_PLAYER_DISABLED;return true;
}
static size_t save(ft_game *g,const ft_world *w,void *out,size_t size){
  std::lock_guard lock(bridge_mutex);return sm64_ft_save(w->handle,static_cast<uint8_t *>(out),size,g->error,sizeof(g->error));
}
static bool load(ft_game *g,ft_world *w,const void *data,size_t size){
  std::lock_guard lock(bridge_mutex);bool ok=sm64_ft_load(w->handle,static_cast<const uint8_t *>(data),size,g->error,sizeof(g->error));
  if(!ok)report(g);else{w->failed=false;sm64_ft_view(w->handle,&w->view);}return ok;
}

static const uint16_t buttons[]={0x8000,0x4000,0x2000,0x1000,0x0020,0x0010,0x0008,0x0004,0x0002,0x0001,0x0800,0x0400,0x0200,0x0100};
#define AXIS(id,name) {id,name,"Raw N64 stick axis",FT_INPUT_INT,FT_INPUT_FLAG_TIMELINE_LANE,-128,127,0,0,0,0,nullptr,0,{0.3f,0.7f,1,1}}
#define BUTTON(id,name) {id,name,"N64 controller button",FT_INPUT_BOOL,FT_INPUT_FLAG_TIMELINE_LANE,0,1,0,0,0,0,nullptr,0,{1,0.7f,0.3f,1}}
static const ft_input_field fields[]={AXIS("stick_x","Stick X"),AXIS("stick_y","Stick Y"),BUTTON("a","A"),BUTTON("b","B"),BUTTON("z","Z"),BUTTON("start","Start"),BUTTON("l","L"),BUTTON("r","R"),BUTTON("c_up","C Up"),BUTTON("c_down","C Down"),BUTTON("c_left","C Left"),BUTTON("c_right","C Right"),BUTTON("d_up","D Up"),BUTTON("d_down","D Down"),BUTTON("d_left","D Left"),BUTTON("d_right","D Right")};
#undef AXIS
#undef BUTTON
static const ft_input_control controls[]={
 {"left","Stick left",nullptr,"Movement","A",0,-80,FT_CONTROL_ADD,nullptr},
 {"right","Stick right",nullptr,"Movement","D",0,80,FT_CONTROL_ADD,nullptr},
 {"forward","Stick forward",nullptr,"Movement","W",1,80,FT_CONTROL_ADD,nullptr},
 {"back","Stick back",nullptr,"Movement","S",1,-80,FT_CONTROL_ADD,nullptr},
 {"a","A / Jump",nullptr,"Actions","Space",2,1,0,nullptr},
 {"b","B / Attack",nullptr,"Actions","F",3,1,0,nullptr},
 {"z","Z / Crouch",nullptr,"Actions","LeftShift",4,1,0,nullptr},
 {"r","R / Camera",nullptr,"Camera","E",7,1,0,nullptr},
 {"l","L Trigger",nullptr,"Actions","Q",6,1,0,nullptr},
 {"c_up","C Up",nullptr,"Camera","I",8,1,0,nullptr},
 {"c_down","C Down",nullptr,"Camera","K",9,1,0,nullptr},
 {"c_left","C Left",nullptr,"Camera","J",10,1,0,nullptr},
 {"c_right","C Right",nullptr,"Camera","L",11,1,0,nullptr},
 {"start","Start / Pause",nullptr,"Menu","Enter",5,1,0,nullptr},
 {"d_up","D-Pad Up",nullptr,"D-Pad",nullptr,12,1,0,nullptr},
 {"d_down","D-Pad Down",nullptr,"D-Pad",nullptr,13,1,0,nullptr},
 {"d_left","D-Pad Left",nullptr,"D-Pad",nullptr,14,1,0,nullptr},
 {"d_right","D-Pad Right",nullptr,"D-Pad",nullptr,15,1,0,nullptr},
};
static const ft_input_schema schema={sizeof(ft_input_schema),sizeof(sm64_input),alignof(sm64_input),fields,16,controls,uint32_t(std::size(controls))};
static void input_default(ft_game *,void *p){std::memset(p,0,sizeof(sm64_input));}
static int64_t input_get(ft_game *,const void *p,uint32_t field){const auto&i=*static_cast<const sm64_input *>(p);if(field==0)return i.stick_x;if(field==1)return i.stick_y;return field<16?bool(i.buttons&buttons[field-2]):0;}
static void input_set(ft_game *,void *p,uint32_t field,int64_t value){auto&i=*static_cast<sm64_input *>(p);if(field==0)i.stick_x=std::clamp<int64_t>(value,-128,127);else if(field==1)i.stick_y=std::clamp<int64_t>(value,-128,127);else if(field<16){if(value)i.buttons|=buttons[field-2];else i.buttons&=~buttons[field-2];}}
static const ft_prop_desc props[]={
 {"position","Position","Mario","units",FT_VALUE_VEC3,FT_PROP_SUMMARY|FT_PROP_WRITABLE|FT_PROP_STARTING,-1000000,1000000},
 {"velocity","Velocity","Mario","units/frame",FT_VALUE_VEC3,FT_PROP_SUMMARY|FT_PROP_WRITABLE|FT_PROP_STARTING,-1000000,1000000},
 {"action","Action","Mario",nullptr,FT_VALUE_INT,FT_PROP_SUMMARY,0,0},
 {"health","Health","Mario",nullptr,FT_VALUE_INT,FT_PROP_SUMMARY|FT_PROP_WRITABLE|FT_PROP_STARTING,0,0x880},
};
static const ft_entity_class classes[]={{"mario","Mario",props,4}};
static int32_t entity_count(ft_game *,const ft_world *,uint32_t c){return c==0?1:0;}
static bool prop_get(ft_game *,const ft_world *w,uint32_t c,int32_t entity,uint32_t prop,ft_value *out){
  if(c||entity||prop>3||!w)return false;
  const sm64_view &v = w->view;
  if(prop<2){const float*p=prop==0?v.pos:v.vel;out->kind=FT_VALUE_VEC3;out->as.v3={p[0],p[1],p[2]};}
  else{out->kind=FT_VALUE_INT;out->as.i=prop==2?v.action:v.health;}
  return v.valid;
}
static bool prop_set(ft_game *g,ft_world *w,uint32_t c,int32_t entity,uint32_t prop,const ft_value *value){
  std::lock_guard lock(bridge_mutex);
  if(c||entity||!value||prop==2||prop>3||!w)return false;
  sm64_view edit{};
  if(prop<2){
    if(value->kind!=FT_VALUE_VEC3)return false;
    float *v=prop==0?edit.pos:edit.vel;
    v[0]=value->as.v3.x;v[1]=value->as.v3.y;v[2]=value->as.v3.z;
  }else{
    if(value->kind!=FT_VALUE_INT||value->as.i<0||value->as.i>0x880)return false;
    edit.health=static_cast<int32_t>(value->as.i);
  }
  const bool ok=sm64_ft_set(w->handle,prop,&edit,g->error,sizeof(g->error));
  if(!ok)report(g);
  else sm64_ft_view(w->handle,&w->view);
  return ok;
}
static uint32_t status(ft_game *g,const ft_world *w,int32_t,float,char *out,uint32_t max,uint32_t line_size){
  if(!max||!line_size||!w)return 0;const sm64_view &v = w->view;
  std::snprintf(out,line_size,"SM64 frame %u | action %08X | health %d",v.frame,v.action,v.health);
  if(max>1&&g->error[0]){std::snprintf(out+line_size,line_size,"%s",g->error);return 2;}return 1;
}
static const ft_camera_mode camera_modes[]={
  {"game","Game camera","Original SM64 camera",FT_CAMERA_MODE_DIRECTED},
  {"orbit","Orbit camera","Drag to turn around Mario, scroll to pull back",FT_CAMERA_MODE_FREE}
};
static bool camera_update(ft_game *,const ft_camera_frame *f,ft_camera *out){
  std::lock_guard lock(bridge_mutex);
  if(!f->world)return false;
  sm64_camera camera{};
  if(!sm64_ft_camera(f->world->handle,out->aspect,&camera))return false;
  out->eye={camera.eye[0],camera.eye[1],camera.eye[2]};
  out->target={camera.target[0],camera.target[1],camera.target[2]};
  out->up={camera.up[0],camera.up[1],camera.up[2]};
  out->fov_y=camera.fov_y;out->near_z=camera.near_z;out->far_z=camera.far_z;
  out->orthographic=false;out->use_view_proj=(f->mode == 0);
  std::memcpy(out->view_proj,camera.view_proj,sizeof(out->view_proj));
  return true;
}
static bool resources_create(ft_game *g){
  if(!g->engine || (g->engine->gpu_device && !g->engine->gpu_device())) return true;
  // A clip-space quad composites the full-game Fast3D frame, without transforming it
  // a second time through the editor camera.
  char path[4096];void *vs=nullptr,*fs=nullptr;size_t vs_size=0,fs_size=0;
  const auto*e=g->engine;
  e->resolve_data_path("shaders/present.vert.spv",path,sizeof(path));if(!e->read_file(path,&vs,&vs_size))return false;
  e->resolve_data_path("shaders/present.frag.spv",path,sizeof(path));if(!e->read_file(path,&fs,&fs_size)){e->free_file_data(vs);return false;}
  ft_pipeline_desc d{};d.struct_size=sizeof(d);d.vertex_spirv=vs;d.vertex_spirv_size=vs_size;d.fragment_spirv=fs;d.fragment_spirv_size=fs_size;d.texture_count=1;
  g->pipeline=e->pipeline_create(&d);e->free_file_data(vs);e->free_file_data(fs);
  const ft_vertex vertices[]={{{-1,-1},{1,1,1},{0,0}},{{1,-1},{1,1,1},{1,0}},{{1,1},{1,1,1},{1,1}},{{-1,1},{1,1,1},{0,1}}};
  const uint32_t indices[]={0,1,2,0,2,3};g->quad=e->mesh_create(vertices,4,sizeof(ft_vertex),indices,6);
  return g->pipeline&&g->quad;
}
static bool same_render_config(const sm64_render_config &a, const sm64_render_config &b) {
  if(a.width!=b.width||a.height!=b.height||a.mode!=b.mode||a.collision!=b.collision||a.scene_only!=b.scene_only)return false;
  if(!a.mode)return true;
  return std::memcmp(a.eye,b.eye,sizeof(a.eye))==0 && std::memcmp(a.target,b.target,sizeof(a.target))==0 &&
         std::memcmp(a.up,b.up,sizeof(a.up))==0 && std::memcmp(a.view_proj,b.view_proj,sizeof(a.view_proj))==0;
}
static void render(ft_game *g,const ft_render_frame *f){
  if(f->pass!=FT_PASS_ENTITIES||!f->active||!f->world||!g->pipeline||!g->quad)return;
  std::lock_guard lock(bridge_mutex);ft_camera camera{};g->engine->camera_get(&camera);
  uint32_t width=std::clamp(int(camera.viewport.x),1,4096),height=std::clamp(int(camera.viewport.y),1,4096);
  if(!g->texture||g->width!=width||g->height!=height){
    g->rendered_revision=0;
    if(g->texture)g->engine->texture_destroy(g->texture);
    ft_texture_desc d{};d.struct_size=sizeof(d);d.pixels=nullptr;d.width=width;d.height=height;d.layers=1;d.format=FT_TEXTURE_RGBA8;
    g->texture=g->engine->texture_create(&d);g->width=width;g->height=height;
  }
  if(!g->texture)return;
  sm64_render_config config{};config.width=width;config.height=height;config.collision=g->collision;
  config.mode=camera.mode==0?0:camera.orthographic?2:1;
  config.eye[0]=camera.eye.x;config.eye[1]=camera.eye.y;config.eye[2]=camera.eye.z;
  config.target[0]=camera.target.x;config.target[1]=camera.target.y;config.target[2]=camera.target.z;
  config.up[0]=camera.up.x;config.up[1]=camera.up.y;config.up[2]=camera.up.z;
  config.span=camera.view_proj[5]!=0?2.f/std::abs(camera.view_proj[5]):2000.f;
  std::memcpy(config.view_proj,camera.view_proj,sizeof(config.view_proj));
  std::vector<sm64_scene_mario> ghosts;
  std::vector<uint64_t> group_revisions;
  if(g->engine->timeline_world_count && g->engine->timeline_world_pair) {
    const auto count=g->engine->timeline_world_count();
    for(uint32_t i=0;i<count;++i) {
      if(int32_t(i)==f->world_index) continue;
      const ft_world *previous=nullptr,*current=nullptr;
      if(g->engine->timeline_world_pair(i,f->state.current_tick,&previous,&current) && current) {
        sm64_scene_mario pose{};
        if(sm64_ft_pose(current->handle,&pose)) {
          ghosts.push_back(pose);
          group_revisions.push_back(i);
          group_revisions.push_back(sm64_ft_revision(current->handle));
        }
      }
    }
  }
  config.ghosts=ghosts.data();config.ghost_count=uint32_t(ghosts.size());
  const uint64_t revision=sm64_ft_revision(f->world->handle);
  if(revision==g->rendered_revision && group_revisions==g->rendered_groups && same_render_config(config,g->rendered_config)){
    g->engine->draw_mesh(g->pipeline,0,g->quad,&g->texture,1,nullptr,0);
    return;
  }

  ft_gpu_image gpu_image{};
  gpu_image.struct_size = sizeof(ft_gpu_image);
  const ft_gpu_device *gpu = g->engine->gpu_device ? g->engine->gpu_device() : nullptr;
  bool rendered = false;
  if(gpu && gpu->api==FT_GPU_API_VULKAN && g->engine->texture_gpu_image && g->engine->texture_gpu_image(g->texture, &gpu_image)){
    rendered = sm64_ft_render_gpu(f->world->handle, &config, gpu, &gpu_image, g->error, sizeof(g->error));
    if (!rendered && !g->gpu_failure_reported) {
      report(g);
      if(g->engine->log) g->engine->log(FT_LOG_WARN, "sm64", "Vulkan rendering failed; using software rendering for this frame");
    }
    g->gpu_failure_reported = !rendered;
  }
  if(!rendered){
    g->pixels.resize(size_t(width)*height*4);
    if(!sm64_ft_render(f->world->handle,&config,g->pixels.data(),g->pixels.size(),g->error,sizeof(g->error)))return;
    if(!g->engine->texture_update_layer(g->texture,0,g->pixels.data(),width,height))return;
  }
  g->rendered_revision=revision;
  g->rendered_config=config;
  g->rendered_config.ghosts=nullptr;
  g->rendered_groups=std::move(group_revisions);
  if(g->texture)g->engine->draw_mesh(g->pipeline,0,g->quad,&g->texture,1,nullptr,0);
}
static const ft_setting_desc settings[]={{"collision","Collision surfaces","Show physical surfaces instead of level visuals","SM64",FT_VALUE_BOOL,0,1,FT_SETTING_RENDER}};
static uint32_t setting_count(ft_game *){return 1;}
static const ft_setting_desc *setting_desc(ft_game *,uint32_t i){return i==0?settings:nullptr;}
static bool setting_get(ft_game *g,uint32_t i,ft_value *v){if(i)return false;v->kind=FT_VALUE_BOOL;v->as.b=g->collision;return true;}
static bool setting_set(ft_game *g,uint32_t i,const ft_value *v){if(i||v->kind!=FT_VALUE_BOOL)return false;g->collision=v->as.b;return true;}
static const ft_exporter_desc exporter={"m64","M64 movie from power-on","m64","SM64 M64 movie"};
static uint32_t exporter_count(ft_game *){return 1;}
static const ft_exporter_desc *exporter_desc(ft_game *,uint32_t i){return i==0?&exporter:nullptr;}
static bool export_run(ft_game *g,uint32_t index,const ft_export_request *r){
  if(index||!r||r->end_tick==INT_MAX)return false;
  if(g->engine->timeline_world_count()!=1){std::snprintf(g->error,sizeof(g->error),"M64 export currently requires one timeline world");report(g);return false;}
  const ft_world *previous=nullptr,*current=nullptr;
  if(!g->engine->timeline_world_pair(0,r->end_tick+1,&previous,&current)||!current)return false;
  std::lock_guard lock(bridge_mutex);bool ok=sm64_ft_export(current->handle,r->path,g->error,sizeof(g->error));if(!ok)report(g);return ok;
}
struct Setup {
  char error[2048]{};
  bool launched = false;
  char filter[128]{};
};

struct CourseCard {
  uint32_t level_id;
  const char *tag;
  const char *title;
  const char *desc;
};

static const CourseCard kMainCourses[] = {
  {9, "COURSE 1", "Bob-omb Battlefield", "King Bob-omb on the summit, floating island, and rolling chain chomps."},
  {24, "COURSE 2", "Whomp's Fortress", "Towering stone fortress, Whomp King, and sleeping Piranha Plants."},
  {12, "COURSE 3", "Jolly Roger Bay", "Sunken galleon, treasure chests, eel in the wall, and deep ocean jet."},
  {5, "COURSE 4", "Cool, Cool Mountain", "Snowy peak, slippery mountain ice slide, and lost baby penguin."},
  {4, "COURSE 5", "Big Boo's Haunt", "Spooky haunted mansion, mad piano, merry-go-round, and Big Boo."},
  {7, "COURSE 6", "Hazy Maze Cave", "Underground caverns, toxic haze maze, rolling rocks, and Dorrie."},
  {22, "COURSE 7", "Lethal Lava Land", "Boiling magma sea, sinking platforms, rolling log, and Big Bully."},
  {8, "COURSE 8", "Shifting Sand Land", "Desert dunes, treacherous quicksand, ancient pyramid, and Eyerok."},
  {23, "COURSE 9", "Dire, Dire Docks", "Bowser's submarine docking bay, swirling whirlpool, and manta ray."},
  {10, "COURSE 10", "Snowman's Land", "Giant frozen snowman, freezing pond, ice maze, and shell surfing."},
  {11, "COURSE 11", "Wet-Dry World", "Water level switches, submerged platforms, Heave-Hos, downtown secret."},
  {36, "COURSE 12", "Tall, Tall Mountain", "Sheer mountain cliffs, rolling mushrooms, waterfall, Ukiki monkey."},
  {13, "COURSE 13", "Tiny-Huge Island", "Shrink and grow pipes, giant Koopa, and Wiggler inside the mountain."},
  {14, "COURSE 14", "Tick Tock Clock", "Grandfather clock mechanisms, turning gears, pendulums, conveyor belts."},
  {15, "COURSE 15", "Rainbow Ride", "Magic carpets flying along rainbow rails, floating cruiser, house in sky."},
};

static const CourseCard kCastleStages[] = {
  {16, "OVERWORLD", "Castle Grounds", "Princess Peach's castle exterior, waterfall, moat, bridge, and cannon."},
  {6, "CASTLE", "Inside Castle", "Castle foyer, mezzanine, stained glass windows, and course doors."},
  {26, "GARDEN", "Castle Courtyard", "Courtyard garden behind the castle, central fountain, and Boos."},
};

static const CourseCard kBowserStages[] = {
  {17, "BOWSER 1", "Bowser in the Dark World", "Suspended obstacle course with crystal switches and tilting ramps."},
  {30, "BOSS 1", "Bowser in the Dark World - Boss", "First boss encounter: swing Bowser by his tail into spiked bombs."},
  {19, "BOWSER 2", "Bowser in the Fire Sea", "Magma-filled gauntlet with sinking ledges and fire-breathing traps."},
  {33, "BOSS 2", "Bowser in the Fire Sea - Boss", "Second boss encounter: Bowser tilts the arena with fiery shockwaves."},
  {21, "BOWSER 3", "Bowser in the Sky", "The final high-altitude obstacle course leading to Bowser's domain."},
  {34, "FINAL BOSS", "Bowser in the Sky - Boss", "The final showdown: three bomb hits to defeat Bowser and rescue Peach."},
};

static const CourseCard kSecretStages[] = {
  {27, "SECRET", "The Princess's Secret Slide", "Hidden high-speed slide through Princess Peach's stained glass window."},
  {28, "CAP COURSE", "Cavern of the Metal Cap", "Emerald cavern with rushing currents unlocking the heavy metal cap."},
  {29, "CAP COURSE", "Tower of the Wing Cap", "Soaring rainbow tower unlocking red wing cap flight over the castle."},
  {18, "CAP COURSE", "Vanish Cap Under the Moat", "Castle moat drainage course unlocking the ethereal vanish cap."},
  {31, "SECRET", "Wing Mario Over the Rainbow", "High floating cloud islands with cannons and eight red coins."},
  {20, "SECRET", "The Secret Aquarium", "Hidden underwater aquarium behind the high castle window."},
};

static bool card_matches_filter(const CourseCard &card, const char *filter) {
  if (!filter || !filter[0]) return true;
  auto contains_ci = [](const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    std::string h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    return h.find(n) != std::string::npos;
  };
  return contains_ci(card.title, filter) || contains_ci(card.tag, filter) || contains_ci(card.desc, filter);
}

static void draw_course_grid(const ft_engine_api *e, Setup &s, const std::shared_ptr<struct RuntimePreparation> &task,
                             const CourseCard *cards, size_t count);
struct RuntimePaths {
  std::filesystem::path directory,cache_directory;
  std::string extension;
};
struct SelectedRuntime {
  std::filesystem::path rom,locked,cache_rom,cache_library;
  std::string version;
};
struct RuntimePreparation {
  enum class State : uint8_t {Idle,Unlocking,Ready,Failed};
  std::atomic<State> state{State::Idle};
  std::mutex mutex;
  std::string error;
  SelectedRuntime runtime;
};
static std::mutex preparation_mutex;
static std::shared_ptr<RuntimePreparation> preparation;
[[maybe_unused]] static bool regular_file(const std::filesystem::path &path){std::error_code error;return std::filesystem::is_regular_file(path,error);}
static bool runtime_paths(const ft_engine_api *e,RuntimePaths *out){
  char directory[4096]{},cache[4096]{};
  if(!e||!e->resolve_data_path||!e->resolve_cache_path||!e->resolve_data_path("",directory,sizeof(directory))||!e->resolve_cache_path("runtime",cache,sizeof(cache)))return false;
  out->directory=directory;out->cache_directory=cache;
#ifdef _WIN32
  out->extension=".dll";
#else
  out->extension=".so";
#endif
  return true;
}
static std::string identify_rom(const std::filesystem::path &path){
  std::error_code size_error;if(std::filesystem::file_size(path,size_error)!=8u*1024u*1024u||size_error)return {};
  std::array<uint8_t,0x40> header{};std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char *>(header.data()),header.size());if(file.gcount()!=std::streamsize(header.size()))return {};
  if(std::equal(header.begin(),header.begin()+4,std::array<uint8_t,4>{0x37,0x80,0x40,0x12}.begin()))for(size_t i=0;i<header.size();i+=2)std::swap(header[i],header[i+1]);
  else if(std::equal(header.begin(),header.begin()+4,std::array<uint8_t,4>{0x40,0x12,0x37,0x80}.begin()))for(size_t i=0;i<header.size();i+=4)std::reverse(header.begin()+i,header.begin()+i+4);
  else if(!std::equal(header.begin(),header.begin()+4,std::array<uint8_t,4>{0x80,0x37,0x12,0x40}.begin()))return {};
  if(header[0x10]==0x63&&header[0x11]==0x5a&&header[0x12]==0x2b&&header[0x13]==0xff&&header[0x3e]=='E')return "us";
  if(header[0x10]==0x4e&&header[0x11]==0xaa&&header[0x12]==0x3d&&header[0x13]==0x0e&&header[0x3e]=='J')return "jp";
  if(header[0x10]==0xa0&&header[0x11]==0x3c&&header[0x12]==0xf0&&header[0x13]==0x36&&header[0x3e]=='P')return "eu";
  return {};
}
static int version_priority(const std::string &version){
  if(version=="us")return 0;if(version=="jp")return 1;if(version=="eu")return 2;return 3;
}
static bool find_runtime(const RuntimePaths &paths,SelectedRuntime *out,std::string *error){
  struct Candidate {std::filesystem::path path;std::string version;};std::error_code iterator_error;std::vector<Candidate> candidates;bool found_rom_file=false;
  for(std::filesystem::directory_iterator it(paths.directory,iterator_error),end;!iterator_error&&it!=end;it.increment(iterator_error)){
    std::error_code file_error;if(!it->is_regular_file(file_error)||file_error)continue;std::string extension=it->path().extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(extension!=".z64"&&extension!=".n64"&&extension!=".v64")continue;found_rom_file=true;const std::string version=identify_rom(it->path());if(!version.empty())candidates.push_back({it->path(),version});
  }
  if(iterator_error){*error="Cannot inspect data/games/sm64: "+iterator_error.message();return false;}
  if(candidates.empty()){*error=found_rom_file?"The SM64 ROM is not a supported unmodified US, JP, or EU ROM.":"Place a legally obtained, unmodified SM64 ROM (.z64, .n64, or .v64) in data/games/sm64 next to FrameTee.";return false;}
  std::sort(candidates.begin(),candidates.end(),[](const Candidate &a,const Candidate &b){const int ap=version_priority(a.version),bp=version_priority(b.version);return ap==bp?a.path.generic_string()<b.path.generic_string():ap<bp;});
  out->rom=candidates.front().path;
  out->version=candidates.front().version;
  out->cache_rom=candidates.front().path;
  out->cache_library="";
  return true;
}
static std::shared_ptr<RuntimePreparation> prepare_runtime(const RuntimePaths &,const SelectedRuntime &runtime){
  std::lock_guard lock(preparation_mutex);if(!preparation)preparation=std::make_shared<RuntimePreparation>();auto task=preparation;if(task->state.load()==RuntimePreparation::State::Idle){task->runtime=runtime;task->state.store(RuntimePreparation::State::Ready);}return task;
}
static void retry_preparation(){std::lock_guard lock(preparation_mutex);preparation.reset();}

static void draw_course_grid(const ft_engine_api *e, Setup &s, const std::shared_ptr<RuntimePreparation> &task,
                             const CourseCard *cards, size_t count) {
  const ImVec2 avail = igGetContentRegionAvail();
  const float scrollbar_padding = 8.f;
  const float effective_avail_x = std::max(100.f, avail.x - scrollbar_padding);
  const float card_width = effective_avail_x > 840.f ? (effective_avail_x - 24.f) / 3.f : (effective_avail_x > 500.f ? (effective_avail_x - 12.f) / 2.f : effective_avail_x);
  const float padding = 12.f;
  const float text_width = std::max(1.f, card_width - padding * 2.f);
  const float line_height = igGetTextLineHeight();
  float card_height = 110.f;
  for (size_t i = 0; i < count; ++i) {
    if (!card_matches_filter(cards[i], s.filter)) continue;
    const float title_height = igCalcTextSize(cards[i].title, nullptr, false, text_width).y;
    const float desc_height = igCalcTextSize(cards[i].desc, nullptr, false, text_width).y;
    card_height = std::max(card_height, padding * 2.f + line_height + title_height + desc_height + 16.f);
  }
  int drawn_in_row = 0;
  int total_drawn = 0;

  for (size_t i = 0; i < count; ++i) {
    const auto &card = cards[i];
    if (!card_matches_filter(card, s.filter)) continue;

    igPushID_Int(static_cast<int>(card.level_id));
    igBeginGroup();

    const bool clicked = igInvisibleButton("##card", ImVec2{std::max(1.f, card_width), card_height}, 0);
    const bool hovered = igIsItemHovered(0);
    const ImVec2 min = igGetItemRectMin();
    const ImVec2 max = igGetItemRectMax();
    ImDrawList *draw = igGetWindowDrawList();

    const ImU32 bg = hovered ? igGetColorU32_Vec4(ImVec4{42.f / 255.f, 52.f / 255.f, 72.f / 255.f, 1.f})
                             : igGetColorU32_Vec4(ImVec4{25.f / 255.f, 30.f / 255.f, 42.f / 255.f, 1.f});
    const ImU32 border = hovered ? igGetColorU32_Vec4(ImVec4{100.f / 255.f, 180.f / 255.f, 255.f / 255.f, 1.f})
                                 : igGetColorU32_Vec4(ImVec4{45.f / 255.f, 55.f / 255.f, 75.f / 255.f, 140.f / 255.f});
    const ImU32 tag_col = igGetColorU32_Vec4(ImVec4{255.f / 255.f, 200.f / 255.f, 80.f / 255.f, 1.f});
    const ImU32 title_col = igGetColorU32_Vec4(ImVec4{240.f / 255.f, 245.f / 255.f, 255.f / 255.f, 1.f});
    const ImU32 desc_col = igGetColorU32_Vec4(ImVec4{150.f / 255.f, 160.f / 255.f, 180.f / 255.f, 1.f});

    ImDrawList_AddRectFilled(draw, min, max, bg, 6.f, 0);
    ImDrawList_AddRect(draw, min, max, border, 6.f, 0, 1.f);
    ImDrawList_PushClipRect(draw, min, max, true);
    const float title_y = min.y + padding + line_height + 4.f;
    const float title_h = igCalcTextSize(card.title, nullptr, false, text_width).y;
    const float desc_y = title_y + title_h + 6.f;
    ImDrawList_AddText_FontPtr(draw, igGetFont(), igGetFontSize(), ImVec2{min.x + padding, min.y + padding}, tag_col, card.tag, nullptr, text_width, nullptr);
    ImDrawList_AddText_FontPtr(draw, igGetFont(), igGetFontSize(), ImVec2{min.x + padding, title_y}, title_col, card.title, nullptr, text_width, nullptr);
    ImDrawList_AddText_FontPtr(draw, igGetFont(), igGetFontSize(), ImVec2{min.x + padding, desc_y}, desc_col, card.desc, nullptr, text_width, nullptr);
    ImDrawList_PopClipRect(draw);

    if (clicked) {
      std::lock_guard lock(task->mutex);
      char config_name[128];
      std::snprintf(config_name, sizeof(config_name), "course_%u.sm64", card.level_id);
      char config[4096]{};
      if (!e->request_level || !e->resolve_cache_path(config_name, config, sizeof(config)) ||
          !sm64_ft_write_config(config, task->runtime.rom.string().c_str(),
                                "", "", 0, card.level_id,
                                s.error, sizeof(s.error))) {
        if (!s.error[0]) std::snprintf(s.error, sizeof(s.error), "FrameTee could not create the SM64 project.");
      } else {
        s.launched = true;
        e->request_level(config);
      }
    }

    igEndGroup();
    igPopID();

    ++total_drawn;
    ++drawn_in_row;
    const int max_per_row = effective_avail_x > 840.f ? 3 : (effective_avail_x > 500.f ? 2 : 1);
    if (drawn_in_row < max_per_row) {
      igSameLine(0.f, 12.f);
    } else {
      drawn_in_row = 0;
      igSpacing();
    }
  }

  if (total_drawn == 0) {
    igTextDisabled("No courses match your search in this category.");
  }
}

static constexpr ft_panel_desc kPanels[] = {
  {"Player Info", FT_DOCK_LEFT},
};

static void ui_attach(const ft_engine_api *engine) {
  static bool attached = false;
  if (attached || !engine || !engine->imgui_context) return;
  auto *context = static_cast<ImGuiContext *>(engine->imgui_context());
  if (!context) return;
  if (engine->imgui_allocators) {
    void *alloc_fn = nullptr;
    void *free_fn = nullptr;
    void *user_data = nullptr;
    engine->imgui_allocators(&alloc_fn, &free_fn, &user_data);
    if (alloc_fn && free_fn)
      igSetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(alloc_fn), reinterpret_cast<ImGuiMemFreeFunc>(free_fn), user_data);
  }
  igSetCurrentContext(context);
  attached = true;
}

static void player_panel(ft_game *g, const ft_ui_frame *frame) {
  if (!igBegin("Player Info", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    igEnd();
    return;
  }

  const int32_t track = frame->state.selected_player;
  if (track < 0) {
    igTextDisabled("No player track selected.");
    igEnd();
    return;
  }

  if (frame->world) {
    const sm64_view *v = sm64_world_view(frame->world);
    if (v && v->valid) {
      igSeparatorText("Mario");
      igText("Position: (%.1f, %.1f, %.1f)", v->pos[0], v->pos[1], v->pos[2]);
      igText("Velocity: (%.1f, %.1f, %.1f)", v->vel[0], v->vel[1], v->vel[2]);
      igText("Action:   0x%08X", v->action);
      const int wedges = (v->health >> 8);
      igText("Health:   %d / 8 (0x%X)", wedges, v->health);
      igSpacing();
    }
  }

  if (igCollapsingHeader_TreeNodeFlags("Starting state", ImGuiTreeNodeFlags_DefaultOpen) && g->engine->starting_state_editor) {
    g->engine->starting_state_editor(track);
  }

  igEnd();
}

static void ui(ft_game *g, const ft_ui_frame *frame) {
  if (!g || !frame || frame->state.headless) return;
  ui_attach(g->engine);
  if (frame->slot == FT_UI_PANELS) {
    player_panel(g, frame);
  }
}

static void splash(const ft_engine_api *e,void **context,const ft_ui_frame *f){
  if(f->slot!=FT_UI_SPLASH)return;
  ui_attach(e);
  if(!*context)*context=new Setup;
  auto&s=*static_cast<Setup *>(*context);
  s.error[0]=0;
  RuntimePaths paths{};

  if(!runtime_paths(e,&paths)){
    std::snprintf(s.error,sizeof(s.error),"FrameTee could not resolve its SM64 data or cache directory.");
    igTextUnformatted("Super Mario 64",nullptr);
    igTextWrapped("%s",s.error);
    return;
  }

  SelectedRuntime runtime{};
  std::string preparation_error;
  if(!find_runtime(paths,&runtime,&preparation_error)){
    std::snprintf(s.error,sizeof(s.error),"%s",preparation_error.c_str());
    igTextUnformatted("Super Mario 64",nullptr);
    igTextWrapped("%s",s.error);
    return;
  }

  const auto task=prepare_runtime(paths,runtime);
  const auto state=task->state.load();
  if(state==RuntimePreparation::State::Unlocking){
    igPushFont(nullptr, 22.f);
    igTextUnformatted("Super Mario 64",nullptr);
    igPopFont();
    igTextWrapped("Unlocking the native SM64 runtime with your ROM. This happens automatically and needs no compiler.");
    return;
  }
  if(state==RuntimePreparation::State::Failed){
    igPushFont(nullptr, 22.f);
    igTextUnformatted("Super Mario 64",nullptr);
    igPopFont();
    std::lock_guard lock(task->mutex);
    std::snprintf(s.error,sizeof(s.error),"%s",task->error.c_str());
    igTextWrapped("%s",s.error);
    if(igButton("Retry unlock",ImVec2{0,0}))retry_preparation();
    return;
  }

  if(state==RuntimePreparation::State::Ready){
    if(s.launched){
      igPushFont(nullptr, 22.f);
      igTextUnformatted("Super Mario 64",nullptr);
      igPopFont();
      igTextWrapped("Starting the native SM64 runtime unlocked with data/games/sm64.");
      if(s.error[0])igTextWrapped("%s",s.error);
      return;
    }

    igPushFont(nullptr, 22.f);
    igTextUnformatted("Super Mario 64",nullptr);
    igPopFont();
    igPushStyleColor_Vec4(ImGuiCol_Text, igGetStyle()->Colors[ImGuiCol_TextDisabled]);
    igTextWrapped("Choose a course or stage to start practicing or recording.");
    igPopStyleColor(1);
    igSpacing();

    igAlignTextToFramePadding();
    igTextUnformatted("Select Stage",nullptr);
    igSameLine(0.f,20.f);
    const float search_width=std::min(260.f,igGetContentRegionAvail().x);
    const float search_x=igGetCursorPosX()+igGetContentRegionAvail().x-search_width;
    igSetCursorPosX(search_x);
    igSetNextItemWidth(search_width);
    igInputTextWithHint("##filter","Search courses...",s.filter,sizeof(s.filter),0,nullptr,nullptr);
    igSeparator();
    igSpacing();

    if(igBeginTabBar("##sm64_categories",0)){
      auto render_tab = [&](const char *name, const char *child_id, const CourseCard *cards, size_t count) {
        if(igBeginTabItem(name,nullptr,0)){
          igSpacing();
          if(igBeginChild_Str(child_id,ImVec2{0,0},false,0)){
            draw_course_grid(e,s,task,cards,count);
          }
          igEndChild();
          igEndTabItem();
        }
      };
      render_tab("Main Courses", "##scroll_main", kMainCourses, std::size(kMainCourses));
      render_tab("Castle & Grounds", "##scroll_castle", kCastleStages, std::size(kCastleStages));
      render_tab("Bowser Stages", "##scroll_bowser", kBowserStages, std::size(kBowserStages));
      render_tab("Secret Courses", "##scroll_secret", kSecretStages, std::size(kSecretStages));
      igEndTabBar();
    }
    if(s.error[0]){
      igSpacing();
      igTextColored(ImVec4{1.f,0.3f,0.3f,1.f},"%s",s.error);
    }
  }
}
static void splash_destroy(void *context){delete static_cast<Setup *>(context);}
static ft_game_module make_module(){
  ft_game_module m{};m.struct_size=sizeof(m);m.abi_version=FT_GAME_ABI_VERSION;m.abi_revision=FT_GAME_ABI_REVISION;
  m.info={sizeof(ft_game_info),"sm64","Super Mario 64","0.2.0","FrameTee", "https://github.com/sm64pc/sm64ex","thumbnail.png"};
  m.constraints.struct_size=sizeof(ft_game_constraints);m.constraints.caps=FT_CAP_HEADLESS|FT_CAP_WORLD_SERIALIZE|FT_CAP_RENDERS_LEVEL|FT_CAP_EXPORTERS|FT_CAP_HOSTS_STARTING_STATE;
  m.constraints.dimensions=FT_DIMENSIONS_3D;m.constraints.min_players=m.constraints.max_players=1;m.constraints.ticks_per_second=30;m.constraints.units_per_tile=1;m.constraints.default_camera_height=2000;
  m.constraints.camera_modes=camera_modes;m.constraints.camera_mode_count=uint32_t(std::size(camera_modes));m.constraints.level_extension="sm64";m.constraints.level_filter_name="SM64 setup";
  m.input_schema=&schema;m.entity_classes=classes;m.entity_class_count=1;
  m.create=create;m.destroy=destroy;m.level_load_path=level_load;m.level_destroy=level_destroy;m.level_info=level_info;
  m.world_create=world_create;m.world_destroy=world_destroy;m.world_copy=world_copy;m.world_step=step;m.world_tick=tick;m.world_player_count=player_count;m.world_player_view=player_view;
  m.world_serialize=save;m.world_deserialize=load;m.input_default=input_default;m.input_get=input_get;m.input_set=input_set;
  m.entity_count=entity_count;m.entity_prop_get=prop_get;m.entity_prop_set=prop_set;m.status_lines=status;m.camera_update=camera_update;
  m.resources_create=resources_create;m.resources_destroy=resources_destroy;m.render=render;m.ui=ui;m.panels=kPanels;m.panel_count=uint32_t(std::size(kPanels));
  m.splash=splash;m.splash_destroy=splash_destroy;
  m.setting_count=setting_count;m.setting_desc=setting_desc;m.setting_get=setting_get;m.setting_set=setting_set;
  m.exporter_count=exporter_count;m.exporter_desc=exporter_desc;m.export_run=export_run;return m;
}
extern "C" FT_GAME_EXPORT const ft_game_module *ft_game_module_entry(uint32_t version){static const ft_game_module m=make_module();return version==FT_GAME_ABI_VERSION?&m:nullptr;}
