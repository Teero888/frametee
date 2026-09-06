//! A complete Bevy ECS-backed game module written in Rust: a ball bouncing in
//! a box under editable per-tick impulses.
//!
//! FrameTee loads this crate as a shared library and drives its snapshots and
//! ticks through a C ABI. Inside that boundary, simulation is ordinary Bevy:
//! an ECS `World`, components, resources, and an explicitly ordered schedule.
//! Bevy's renderer is intentionally not initialized because FrameTee owns the
//! Vulkan window; the module emits FrameTee draw commands from its ECS state.
//!
//! What a module in any language has to provide is exactly this: one exported
//! `ft_game_module_entry`, a static vtable, and functions that respect the
//! ownership rules in `game_abi.h`.

mod abi;
mod gpu;
mod render;

use abi::*;
use bevy_ecs::prelude::{Component, Entity, Query, Res, ResMut, Resource, With, World as EcsWorld};
use bevy_ecs::schedule::{IntoScheduleConfigs, Schedule, SingleThreadedExecutor};
use bevy_math::Vec2;
use std::os::raw::{c_char, c_void};

// Static, NUL-terminated strings, since everything crossing the boundary is a
// C string that must outlive the call.
const ID: &[u8] = b"example-bouncer\0";
const NAME: &[u8] = b"Bevy Bouncer (Rust)\0";
const VERSION: &[u8] = b"2.0.0\0";
const AUTHOR: &[u8] = b"FrameTee\0";
const THUMBNAIL: &[u8] = b"thumbnail.png\0";

const FIELD_PUSH_X: &[u8] = b"push_x\0";
const FIELD_PUSH_X_NAME: &[u8] = b"Push X\0";
const FIELD_PUSH_Y: &[u8] = b"push_y\0";
const FIELD_PUSH_Y_NAME: &[u8] = b"Push Y\0";
const CONTROL_LEFT: &[u8] = b"left\0";
const CONTROL_LEFT_NAME: &[u8] = b"Push Left\0";
const CONTROL_RIGHT: &[u8] = b"right\0";
const CONTROL_RIGHT_NAME: &[u8] = b"Push Right\0";
const CONTROL_UP: &[u8] = b"up\0";
const CONTROL_UP_NAME: &[u8] = b"Push Up\0";
const CONTROL_DOWN: &[u8] = b"down\0";
const CONTROL_DOWN_NAME: &[u8] = b"Push Down\0";
const CONTROL_CATEGORY: &[u8] = b"Bevy Bouncer\0";
const KEY_A: &[u8] = b"A\0";
const KEY_D: &[u8] = b"D\0";
const KEY_W: &[u8] = b"W\0";
const KEY_S: &[u8] = b"S\0";
const SPLASH_TITLE: &[u8] = b"Bevy Bouncer\0";
const SPLASH_DESCRIPTION: &[u8] =
    b"A deterministic Bevy ECS world driven one schedule at a time by FrameTee.\0";
const START_BUTTON: &[u8] = b"Start Bevy arena\0";
const BUILTIN_ARENA: &[u8] = b"builtin:bevy-arena\0";

const fn cstr(bytes: &'static [u8]) -> *const c_char {
    bytes.as_ptr() as *const c_char
}

const BALL_RADIUS: f32 = 0.78;
const WIDTH: f32 = 48.0;
const HEIGHT: f32 = 27.0;

#[repr(C)]
struct ImVec2 {
    x: f32,
    y: f32,
}

// The host exports cimgui's C entry points. Keeping this declaration tiny
// avoids pulling a second ImGui implementation into the Rust module while
// still giving its one built-in level a usable start screen.
extern "C" {
    fn igTextUnformatted(text: *const c_char, text_end: *const c_char);
    fn igSpacing();
    fn igSeparator();
    fn igButton(label: *const c_char, size: ImVec2) -> bool;
}

/// This game's input record. Two signed nudges per tick; the engine stores and
/// edits these through the schema below without knowing what they mean.
#[repr(C)]
#[derive(Copy, Clone, Default)]
struct BouncerInput {
    push_x: i8,
    push_y: i8,
}

struct Level;

#[derive(Component, Copy, Clone)]
struct Ball;

#[derive(Component, Copy, Clone)]
struct Position(Vec2);

#[derive(Component, Copy, Clone)]
struct Velocity(Vec2);

#[derive(Resource, Copy, Clone, Default)]
struct StepInput(Vec2);

#[derive(Resource, Copy, Clone)]
struct SimulationTick(i32);

#[derive(Resource, Copy, Clone)]
struct Arena {
    width: f32,
    height: f32,
    ball_radius: f32,
}

struct World {
    magic: u64,
    ecs: EcsWorld,
    step_schedule: Schedule,
    ball: Entity,
}

const WORLD_MAGIC: u64 = 0x4654_4256_574F_524C; // "FTBVWORL"
const SNAPSHOT_MAGIC: [u8; 4] = *b"FTBV";
const SNAPSHOT_VERSION: u32 = 1;

#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq)]
struct SimulationSnapshot {
    magic: [u8; 4],
    version: u32,
    tick: i32,
    position: [f32; 2],
    velocity: [f32; 2],
}

impl SimulationSnapshot {
    fn initial() -> Self {
        Self {
            magic: SNAPSHOT_MAGIC,
            version: SNAPSHOT_VERSION,
            tick: 0,
            position: [WIDTH * 0.5, HEIGHT * 0.5],
            velocity: [0.21, 0.13],
        }
    }

    fn is_valid(self) -> bool {
        self.magic == SNAPSHOT_MAGIC
            && self.version == SNAPSHOT_VERSION
            && self
                .position
                .iter()
                .chain(self.velocity.iter())
                .all(|value| value.is_finite())
    }
}

fn apply_impulse(input: Res<StepInput>, mut balls: Query<&mut Velocity, With<Ball>>) {
    for mut velocity in &mut balls {
        velocity.0 += input.0 * 0.02;
    }
}

fn integrate(mut balls: Query<(&mut Position, &Velocity), With<Ball>>) {
    for (mut position, velocity) in &mut balls {
        position.0 += velocity.0;
    }
}

fn bounce(mut balls: Query<(&mut Position, &mut Velocity), With<Ball>>, arena: Res<Arena>) {
    for (mut position, mut velocity) in &mut balls {
        if position.0.x < arena.ball_radius {
            position.0.x = arena.ball_radius;
            velocity.0.x = velocity.0.x.abs();
        } else if position.0.x > arena.width - arena.ball_radius {
            position.0.x = arena.width - arena.ball_radius;
            velocity.0.x = -velocity.0.x.abs();
        }

        if position.0.y < arena.ball_radius {
            position.0.y = arena.ball_radius;
            velocity.0.y = velocity.0.y.abs();
        } else if position.0.y > arena.height - arena.ball_radius {
            position.0.y = arena.height - arena.ball_radius;
            velocity.0.y = -velocity.0.y.abs();
        }
    }
}

fn advance_tick(mut tick: ResMut<SimulationTick>) {
    tick.0 += 1;
}

impl World {
    fn new(snapshot: SimulationSnapshot) -> Self {
        let mut ecs = EcsWorld::new();
        ecs.insert_resource(StepInput::default());
        ecs.insert_resource(SimulationTick(snapshot.tick));
        ecs.insert_resource(Arena {
            width: WIDTH,
            height: HEIGHT,
            ball_radius: BALL_RADIUS,
        });
        let ball = ecs
            .spawn((
                Ball,
                Position(Vec2::from_array(snapshot.position)),
                Velocity(Vec2::from_array(snapshot.velocity)),
            ))
            .id();

        // FrameTee can replay the same tick thousands of times while seeking.
        // A single-threaded, explicitly chained schedule makes execution order
        // part of the example's deterministic simulation contract.
        let mut step_schedule = Schedule::default();
        step_schedule.set_executor(SingleThreadedExecutor::new());
        step_schedule.add_systems((apply_impulse, integrate, bounce, advance_tick).chain());

        Self {
            magic: WORLD_MAGIC,
            ecs,
            step_schedule,
            ball,
        }
    }

    fn is_valid(&self) -> bool {
        self.magic == WORLD_MAGIC
    }

    fn snapshot(&self) -> Option<SimulationSnapshot> {
        if !self.is_valid() {
            return None;
        }
        let position = self.ecs.get::<Position>(self.ball)?.0;
        let velocity = self.ecs.get::<Velocity>(self.ball)?.0;
        let tick = self.ecs.get_resource::<SimulationTick>()?.0;
        Some(SimulationSnapshot {
            magic: SNAPSHOT_MAGIC,
            version: SNAPSHOT_VERSION,
            tick,
            position: position.to_array(),
            velocity: velocity.to_array(),
        })
    }

    fn apply_snapshot(&mut self, snapshot: SimulationSnapshot) -> bool {
        if !self.is_valid() || !snapshot.is_valid() {
            return false;
        }
        let Some(mut tick) = self.ecs.get_resource_mut::<SimulationTick>() else {
            return false;
        };
        tick.0 = snapshot.tick;
        let Some(mut position) = self.ecs.get_mut::<Position>(self.ball) else {
            return false;
        };
        position.0 = Vec2::from_array(snapshot.position);
        let Some(mut velocity) = self.ecs.get_mut::<Velocity>(self.ball) else {
            return false;
        };
        velocity.0 = Vec2::from_array(snapshot.velocity);
        true
    }

    fn step(&mut self, input: BouncerInput) {
        if !self.is_valid() {
            return;
        }
        let Some(mut step_input) = self.ecs.get_resource_mut::<StepInput>() else {
            return;
        };
        step_input.0 = Vec2::new(input.push_x as f32, input.push_y as f32);
        self.step_schedule.run(&mut self.ecs);
    }
}

struct Game {
    engine: *const ft_engine_api,
    // Built on first use: the engine has no graphics in headless runs, and the
    // viewport size is not known until a frame arrives.
    renderer: Option<render::BevyRenderer>,
    // Set once the renderer has failed, so it is not rebuilt every frame.
    renderer_failed: bool,
}

// --- input schema ------------------------------------------------------------

static INPUT_FIELDS: [ft_input_field; 2] = [
    ft_input_field {
        id: cstr(FIELD_PUSH_X),
        display_name: cstr(FIELD_PUSH_X_NAME),
        description: std::ptr::null(),
        kind: 1, // FT_INPUT_INT
        flags: FT_INPUT_FLAG_TIMELINE_LANE | FT_INPUT_FLAG_MIRROR_X,
        min_value: -1,
        max_value: 1,
        default_value: 0,
        min_float: 0.0,
        max_float: 0.0,
        default_float: 0.0,
        enum_labels: std::ptr::null(),
        enum_count: 0,
        color: ft_color {
            r: 0.4,
            g: 0.8,
            b: 1.0,
            a: 1.0,
        },
    },
    ft_input_field {
        id: cstr(FIELD_PUSH_Y),
        display_name: cstr(FIELD_PUSH_Y_NAME),
        description: std::ptr::null(),
        kind: 1,
        flags: FT_INPUT_FLAG_TIMELINE_LANE,
        min_value: -1,
        max_value: 1,
        default_value: 0,
        min_float: 0.0,
        max_float: 0.0,
        default_float: 0.0,
        enum_labels: std::ptr::null(),
        enum_count: 0,
        color: ft_color {
            r: 1.0,
            g: 0.7,
            b: 0.4,
            a: 1.0,
        },
    },
];

static INPUT_CONTROLS: [ft_input_control; 4] = [
    ft_input_control {
        id: cstr(CONTROL_LEFT),
        display_name: cstr(CONTROL_LEFT_NAME),
        description: std::ptr::null(),
        category: cstr(CONTROL_CATEGORY),
        default_binding: cstr(KEY_A),
        field: 0,
        value: -1,
        flags: FT_CONTROL_ADD,
        linked_default_binding: std::ptr::null(),
    },
    ft_input_control {
        id: cstr(CONTROL_RIGHT),
        display_name: cstr(CONTROL_RIGHT_NAME),
        description: std::ptr::null(),
        category: cstr(CONTROL_CATEGORY),
        default_binding: cstr(KEY_D),
        field: 0,
        value: 1,
        flags: FT_CONTROL_ADD,
        linked_default_binding: std::ptr::null(),
    },
    ft_input_control {
        id: cstr(CONTROL_UP),
        display_name: cstr(CONTROL_UP_NAME),
        description: std::ptr::null(),
        category: cstr(CONTROL_CATEGORY),
        default_binding: cstr(KEY_W),
        field: 1,
        value: -1,
        flags: FT_CONTROL_ADD,
        linked_default_binding: std::ptr::null(),
    },
    ft_input_control {
        id: cstr(CONTROL_DOWN),
        display_name: cstr(CONTROL_DOWN_NAME),
        description: std::ptr::null(),
        category: cstr(CONTROL_CATEGORY),
        default_binding: cstr(KEY_S),
        field: 1,
        value: 1,
        flags: FT_CONTROL_ADD,
        linked_default_binding: std::ptr::null(),
    },
];

static INPUT_SCHEMA: ft_input_schema = ft_input_schema {
    struct_size: std::mem::size_of::<ft_input_schema>() as u32,
    record_size: std::mem::size_of::<BouncerInput>() as u32,
    record_align: std::mem::align_of::<BouncerInput>() as u32,
    fields: INPUT_FIELDS.as_ptr(),
    field_count: 2,
    controls: INPUT_CONTROLS.as_ptr(),
    control_count: INPUT_CONTROLS.len() as u32,
};

// --- entry points ------------------------------------------------------------
//
// Each of these turns the engine's raw pointers back into Rust references. The
// unsafety is confined to the boundary: everything past the first line of each
// function is ordinary safe Rust.

unsafe extern "C" fn create(engine: *const ft_engine_api) -> *mut c_void {
    Box::into_raw(Box::new(Game {
        engine,
        renderer: None,
        renderer_failed: false,
    })) as *mut c_void
}

unsafe extern "C" fn destroy(game: *mut c_void) {
    if !game.is_null() {
        drop(Box::from_raw(game as *mut Game));
    }
}

unsafe extern "C" fn level_load_path(
    _game: *mut c_void,
    _path: *const c_char,
    _variant: *const c_char,
) -> *mut c_void {
    Box::into_raw(Box::new(Level)) as *mut c_void
}

unsafe extern "C" fn level_load_memory(
    _game: *mut c_void,
    _data: *const c_void,
    _size: usize,
    _variant: *const c_char,
) -> *mut c_void {
    Box::into_raw(Box::new(Level)) as *mut c_void
}

unsafe extern "C" fn level_destroy(_game: *mut c_void, level: *mut c_void) {
    if !level.is_null() {
        drop(Box::from_raw(level as *mut Level));
    }
}

unsafe extern "C" fn level_info(
    _game: *mut c_void,
    _level: *const c_void,
    out: *mut ft_level_info,
) -> bool {
    if out.is_null() {
        return false;
    }
    let info = &mut *out;
    info.name = cstr(NAME);
    info.width_tiles = WIDTH as i32;
    info.height_tiles = HEIGHT as i32;
    info.bounds = ft_rect {
        x: 0.0,
        y: 0.0,
        w: WIDTH,
        h: HEIGHT,
    };
    info.default_spawn = ft_vec2 {
        x: WIDTH * 0.5,
        y: HEIGHT * 0.5,
    };
    true
}

unsafe extern "C" fn world_create(_game: *mut c_void, _desc: *const ft_world_desc) -> *mut c_void {
    Box::into_raw(Box::new(World::new(SimulationSnapshot::initial()))) as *mut c_void
}

unsafe extern "C" fn world_destroy(_game: *mut c_void, world: *mut c_void) {
    if !world.is_null() {
        let world = world as *mut World;
        if (*world).magic == WORLD_MAGIC {
            (*world).magic = 0;
            drop(Box::from_raw(world));
        }
    }
}

unsafe extern "C" fn world_copy(_game: *mut c_void, dst: *mut c_void, src: *const c_void) {
    if dst.is_null() || src.is_null() || std::ptr::eq(dst, src as *mut c_void) {
        return;
    }
    let Some(snapshot) = (*(src as *const World)).snapshot() else {
        return;
    };
    (*(dst as *mut World)).apply_snapshot(snapshot);
}

unsafe extern "C" fn world_step(
    _game: *mut c_void,
    world: *mut c_void,
    inputs: *const c_void,
    player_count: u32,
) {
    if world.is_null() {
        return;
    }
    let mut input = BouncerInput::default();
    if !inputs.is_null() && player_count > 0 {
        input = *(inputs as *const BouncerInput);
    }

    (*(world as *mut World)).step(input);
}

unsafe extern "C" fn world_tick(_game: *mut c_void, world: *const c_void) -> i32 {
    if world.is_null() {
        0
    } else {
        (*(world as *const World))
            .snapshot()
            .map_or(0, |snapshot| snapshot.tick)
    }
}

unsafe extern "C" fn world_player_count(_game: *mut c_void, _world: *const c_void) -> i32 {
    if _world.is_null() || (*(_world as *const World)).magic != WORLD_MAGIC {
        0
    } else {
        1
    }
}

unsafe extern "C" fn world_player_view(
    _game: *mut c_void,
    world: *const c_void,
    player: i32,
    out: *mut ft_player_view,
) -> bool {
    if world.is_null() || player != 0 || out.is_null() {
        return false;
    }
    let Some(snapshot) = (*(world as *const World)).snapshot() else {
        return false;
    };
    let view = &mut *out;
    view.position = ft_vec2 {
        x: snapshot.position[0],
        y: snapshot.position[1],
    };
    view.velocity = ft_vec2 {
        x: snapshot.velocity[0],
        y: snapshot.velocity[1],
    };
    view.aim = ft_vec2::default();
    view.flags = FT_PLAYER_ALIVE;
    view.run_start_tick = -1;
    true
}

unsafe extern "C" fn world_serialize(
    _game: *mut c_void,
    world: *const c_void,
    out: *mut c_void,
    out_size: usize,
) -> usize {
    let needed = std::mem::size_of::<SimulationSnapshot>();
    if world.is_null() {
        return 0;
    }
    if out.is_null() {
        return needed;
    }
    if out_size < needed {
        return 0;
    }
    let Some(snapshot) = (*(world as *const World)).snapshot() else {
        return 0;
    };
    std::ptr::copy_nonoverlapping(
        &snapshot as *const SimulationSnapshot as *const u8,
        out as *mut u8,
        needed,
    );
    needed
}

unsafe extern "C" fn world_deserialize(
    _game: *mut c_void,
    world: *mut c_void,
    data: *const c_void,
    size: usize,
) -> bool {
    let needed = std::mem::size_of::<SimulationSnapshot>();
    if world.is_null() || data.is_null() || size < needed {
        return false;
    }
    let mut snapshot = SimulationSnapshot::initial();
    std::ptr::copy_nonoverlapping(
        data as *const u8,
        &mut snapshot as *mut SimulationSnapshot as *mut u8,
        needed,
    );
    if !snapshot.is_valid() {
        return false;
    }
    (*(world as *mut World)).apply_snapshot(snapshot)
}

// Opaque, explicitly versioned game-owned project metadata. It is deliberately
// tiny, but makes the complete Rust example exercise both project callbacks.
const PROJECT_DATA: &[u8; 8] = b"BEVYPRJ\x01";

unsafe extern "C" fn project_save(_game: *mut c_void, out: *mut c_void, out_size: usize) -> usize {
    if out.is_null() {
        return PROJECT_DATA.len();
    }
    if out_size < PROJECT_DATA.len() {
        return 0;
    }
    std::ptr::copy_nonoverlapping(PROJECT_DATA.as_ptr(), out as *mut u8, PROJECT_DATA.len());
    PROJECT_DATA.len()
}

unsafe extern "C" fn project_load(_game: *mut c_void, data: *const c_void, size: usize) -> bool {
    if data.is_null() || size != PROJECT_DATA.len() {
        return false;
    }
    std::slice::from_raw_parts(data as *const u8, size) == PROJECT_DATA
}

unsafe extern "C" fn input_default(_game: *mut c_void, record: *mut c_void) {
    if !record.is_null() {
        *(record as *mut BouncerInput) = BouncerInput::default();
    }
}

unsafe extern "C" fn input_get(_game: *mut c_void, record: *const c_void, field: u32) -> i64 {
    if record.is_null() {
        return 0;
    }
    let input = &*(record as *const BouncerInput);
    match field {
        0 => input.push_x as i64,
        1 => input.push_y as i64,
        _ => 0,
    }
}

unsafe extern "C" fn input_set(_game: *mut c_void, record: *mut c_void, field: u32, value: i64) {
    if record.is_null() {
        return;
    }
    let input = &mut *(record as *mut BouncerInput);
    let clamped = value.clamp(-1, 1) as i8;
    match field {
        0 => input.push_x = clamped,
        1 => input.push_y = clamped,
        _ => {}
    }
}

unsafe extern "C" fn input_get_vec2(
    _game: *mut c_void,
    _record: *const c_void,
    _field: u32,
) -> ft_vec2 {
    ft_vec2::default()
}

unsafe extern "C" fn input_set_vec2(
    _game: *mut c_void,
    _record: *mut c_void,
    _field: u32,
    _value: ft_vec2,
) {
}

unsafe extern "C" fn render(game: *mut c_void, frame: *const ft_render_frame) {
    if game.is_null() || frame.is_null() {
        return;
    }
    let engine = (*(game as *mut Game)).engine;
    if engine.is_null() {
        return;
    }
    let api = &*engine;
    let frame = &*frame;

    // The level passes draw nothing: Bevy renders the arena, its walls and the
    // ball itself, into the image the engine composites during the entity pass.
    if frame.pass == FT_PASS_LEVEL_BACKGROUND {
        return;
    }

    if frame.pass != FT_PASS_ENTITIES || frame.world.is_null() {
        return;
    }

    // Interpolate Bevy component snapshots between the two simulated ticks the
    // engine handed over.
    let now = &*(frame.world as *const World);
    let before = if frame.previous_world.is_null() {
        now
    } else {
        &*(frame.previous_world as *const World)
    };
    let Some(now_snapshot) = now.snapshot() else {
        return;
    };
    let before_snapshot = before.snapshot().unwrap_or(now_snapshot);
    let alpha = if frame.alpha.is_finite() {
        frame.alpha.clamp(0.0, 1.0)
    } else {
        1.0
    };
    // Never blend across an unrelated cache entry or seek. The engine normally
    // supplies adjacent ticks; falling back to the exact current snapshot is
    // both clearer while scrubbing and safer for arbitrary host scheduling.
    let pos = if now_snapshot.tick == before_snapshot.tick + 1 {
        Vec2::from_array(before_snapshot.position)
            .lerp(Vec2::from_array(now_snapshot.position), alpha)
    } else {
        Vec2::from_array(now_snapshot.position)
    };
    let velocity = Vec2::from_array(now_snapshot.velocity);

    // Bevy draws the scene itself, on the engine's device and into the engine's
    // image; the engine only composites the result. Everything drawn after this
    // point is editor overlay rather than the game.
    if !frame.state.headless {
        let this = &mut *(game as *mut Game);
        if this.renderer.is_none() && !this.renderer_failed {
            let viewport = frame.state.camera.viewport;
            let (w, h) = (viewport.x as u32, viewport.y as u32);
            if w > 0 && h > 0 {
                this.renderer = render::BevyRenderer::new(
                    api,
                    w,
                    h,
                    Vec2::new(WIDTH, HEIGHT),
                    BALL_RADIUS,
                );
                if let Some(log) = api.log {
                    let message = if this.renderer.is_some() {
                        c"Bevy renderer active: drawing on the engine's device"
                    } else {
                        c"Bevy renderer unavailable; drawing with engine primitives"
                    };
                    log(FT_LOG_INFO, c"Bouncer".as_ptr(), message.as_ptr());
                }
            }
        }
        if let Some(renderer) = this.renderer.as_mut() {
            // This function is `extern "C"`, so a panic escaping it aborts the
            // whole editor rather than unwinding. A module must not be able to
            // take the host down, so a failing renderer is caught and switched
            // off; the engine-drawn fallback below keeps the game visible.
            let drawn = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                renderer.render(api, pos, Vec2::new(WIDTH, HEIGHT));
            }));
            if drawn.is_err() {
                if let Some(log) = api.log {
                    log(
                        FT_LOG_INFO,
                        c"Bouncer".as_ptr(),
                        c"Bevy renderer panicked; falling back to engine drawing".as_ptr(),
                    );
                }
                this.renderer = None;
                this.renderer_failed = true;
            }
        }
    }

    if let Some(draw_line) = api.draw_line {
        draw_line(
            5.5,
            ft_vec2 { x: pos.x, y: pos.y },
            ft_vec2 {
                x: pos.x + velocity.x * 5.0,
                y: pos.y + velocity.y * 5.0,
            },
            ft_color {
                r: 0.45,
                g: 0.82,
                b: 1.0,
                a: 0.75,
            },
            0.10,
        );
    }

}

unsafe extern "C" fn splash(engine: *const ft_engine_api, _context: *mut *mut c_void, frame: *const ft_ui_frame) {
    if engine.is_null() || frame.is_null() || (*frame).slot != FT_UI_SPLASH {
        return;
    }

    igTextUnformatted(cstr(SPLASH_TITLE), std::ptr::null());
    igSpacing();
    igTextUnformatted(cstr(SPLASH_DESCRIPTION), std::ptr::null());
    igSpacing();
    igSeparator();
    igSpacing();

    if igButton(cstr(START_BUTTON), ImVec2 { x: 220.0, y: 48.0 }) {
        if !engine.is_null() {
            if let Some(request_level) = (*engine).request_level {
                request_level(cstr(BUILTIN_ARENA));
            }
        }
    }
}

// --- the vtable --------------------------------------------------------------

static MODULE: ft_game_module = ft_game_module {
    struct_size: std::mem::size_of::<ft_game_module>() as u32,
    abi_version: FT_GAME_ABI_VERSION,
    abi_revision: FT_GAME_ABI_REVISION,

    info: ft_game_info {
        struct_size: std::mem::size_of::<ft_game_info>() as u32,
        id: cstr(ID),
        display_name: cstr(NAME),
        version: cstr(VERSION),
        author: cstr(AUTHOR),
        url: std::ptr::null(),
        thumbnail: cstr(THUMBNAIL),
    },

    constraints: ft_game_constraints {
        struct_size: std::mem::size_of::<ft_game_constraints>() as u32,
        // One Bevy-controlled ball, no dynamic cast. The editor adapts.
        caps: FT_CAP_WORLD_SERIALIZE | FT_CAP_RENDERS_LEVEL | FT_CAP_HEADLESS,
        // A plane: this example's arena is flat, and Bevy renders it with a 2D camera.
        dimensions: 0,
        min_players: 1,
        max_players: 1,
        ticks_per_second: 50,
        units_per_tile: 1.0,
        default_camera_height: 20.0,
        variants: std::ptr::null(),
        variant_count: 0,
        camera_modes: std::ptr::null(),
        camera_mode_count: 0,
        level_extension: std::ptr::null(),
        level_filter_name: std::ptr::null(),
    },

    input_schema: &INPUT_SCHEMA,
    entity_classes: std::ptr::null(),
    entity_class_count: 0,

    create: Some(create),
    destroy: Some(destroy),
    update: std::ptr::null(),

    level_load_path: Some(level_load_path),
    level_load_memory: Some(level_load_memory),
    level_destroy: Some(level_destroy),
    level_info: Some(level_info),
    level_serialize: std::ptr::null(),

    world_create: Some(world_create),
    world_destroy: Some(world_destroy),
    world_copy: Some(world_copy),
    world_step: Some(world_step),
    world_tick: Some(world_tick),
    world_player_count: Some(world_player_count),
    world_player_view: Some(world_player_view),
    world_add_player: std::ptr::null(),
    world_remove_player: std::ptr::null(),
    world_serialize: Some(world_serialize),
    world_deserialize: Some(world_deserialize),

    input_default: Some(input_default),
    input_get: Some(input_get),
    input_set: Some(input_set),
    input_get_vec2: Some(input_get_vec2),
    input_set_vec2: Some(input_set_vec2),
    input_describe: std::ptr::null(),

    entity_prop_get: std::ptr::null(),
    entity_prop_set: std::ptr::null(),
    entity_count: std::ptr::null(),

    render: Some(render),
    resources_create: std::ptr::null(),
    resources_destroy: std::ptr::null(),

    ui: None,
    panels: std::ptr::null(),
    panel_count: 0,
    collect_events: std::ptr::null(),

    exporter_count: std::ptr::null(),
    exporter_desc: std::ptr::null(),
    export_run: std::ptr::null(),

    status_lines: std::ptr::null(),
    player_label: std::ptr::null(),
    camera_update: std::ptr::null(),

    setting_count: std::ptr::null(),
    setting_desc: std::ptr::null(),
    setting_get: std::ptr::null(),
    setting_set: std::ptr::null(),

    project_save: Some(project_save),
    project_load: Some(project_load),
    input_get_float: std::ptr::null(),
    input_set_float: std::ptr::null(),
    linked_actions: std::ptr::null(),
    linked_action_count: 0,
    linked_input_update: std::ptr::null(),
    input_effect_count: std::ptr::null(),
    input_effect_desc: std::ptr::null(),
    input_effect_default: std::ptr::null(),
    input_effect_apply: std::ptr::null(),
    input_effect_ui: std::ptr::null(),
    splash: Some(splash),
    splash_destroy: None,
};

/// The one symbol the engine looks for.
#[no_mangle]
pub extern "C" fn ft_game_module_entry(engine_abi_version: u32) -> *const ft_game_module {
    if engine_abi_version != FT_GAME_ABI_VERSION {
        return std::ptr::null();
    }
    &MODULE
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bevy_schedule_replays_identically_from_a_snapshot() {
        let mut first = World::new(SimulationSnapshot::initial());
        for tick in 0..240 {
            first.step(BouncerInput {
                push_x: if tick % 7 == 0 { 1 } else { 0 },
                push_y: if tick % 11 == 0 { -1 } else { 0 },
            });
        }

        let checkpoint = first.snapshot().unwrap();
        let mut replay = World::new(checkpoint);
        for tick in 0..180 {
            let input = BouncerInput {
                push_x: if tick % 5 == 0 { -1 } else { 0 },
                push_y: if tick % 13 == 0 { 1 } else { 0 },
            };
            first.step(input);
            replay.step(input);
        }

        assert_eq!(first.snapshot(), replay.snapshot());
    }
}
