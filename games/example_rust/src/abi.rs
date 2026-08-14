//! Rust view of `include/frametee/game_abi.h`.
//!
//! Hand-written rather than bindgen-generated so the mapping stays readable.
//! Every type here is `#[repr(C)]` and must stay byte-identical to the header;
//! the layout, not this file, is the contract.
//!
//! Only the parts the Bevy example needs are declared. A larger module would
//! mirror the rest of the header the same way, or run bindgen over it.

#![allow(dead_code, non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

pub const FT_GAME_ABI_VERSION: u32 = 10;
pub const FT_GAME_ABI_REVISION: u32 = 0;

pub const FT_CAP_DYNAMIC_PLAYERS: u32 = 1 << 0;
pub const FT_CAP_LINKED_INPUTS: u32 = 1 << 1;
pub const FT_CAP_WORLD_SERIALIZE: u32 = 1 << 3;
pub const FT_CAP_LEVEL_FROM_MEMORY: u32 = 1 << 4;
pub const FT_CAP_TIMELINE_EVENTS: u32 = 1 << 5;
pub const FT_CAP_EXPORTERS: u32 = 1 << 6;
pub const FT_CAP_RENDERS_LEVEL: u32 = 1 << 7;
pub const FT_CAP_HEADLESS: u32 = 1 << 8;

pub const FT_INPUT_FLAG_TIMELINE_LANE: u32 = 1 << 0;
pub const FT_INPUT_FLAG_MIRROR_X: u32 = 1 << 2;
pub const FT_CONTROL_ADD: u32 = 1 << 1;

pub const FT_PLAYER_ALIVE: u32 = 1 << 0;

pub const FT_PASS_LEVEL_BACKGROUND: u32 = 0;
pub const FT_PASS_ENTITIES: u32 = 1;
pub const FT_UI_SPLASH: u32 = 5;

pub const FT_LOG_INFO: c_int = 1;

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct ft_vec2 {
    pub x: f32,
    pub y: f32,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct ft_vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct ft_rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

#[repr(C)]
#[derive(Copy, Clone, Default)]
pub struct ft_color {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct ft_input_field {
    pub id: *const c_char,
    pub display_name: *const c_char,
    pub description: *const c_char,
    pub kind: u32,
    pub flags: u32,
    pub min_value: i32,
    pub max_value: i32,
    pub default_value: i32,
    pub min_float: f32,
    pub max_float: f32,
    pub default_float: f32,
    pub enum_labels: *const *const c_char,
    pub enum_count: u32,
    pub color: ft_color,
}
unsafe impl Sync for ft_input_field {}

#[repr(C)]
pub struct ft_input_control {
    pub id: *const c_char,
    pub display_name: *const c_char,
    pub description: *const c_char,
    pub category: *const c_char,
    pub default_binding: *const c_char,
    pub field: u32,
    pub value: i32,
    pub flags: u32,
    pub linked_default_binding: *const c_char,
}
unsafe impl Sync for ft_input_control {}

#[repr(C)]
pub struct ft_input_schema {
    pub struct_size: u32,
    pub record_size: u32,
    pub record_align: u32,
    pub fields: *const ft_input_field,
    pub field_count: u32,
    pub controls: *const ft_input_control,
    pub control_count: u32,
}
unsafe impl Sync for ft_input_schema {}

#[repr(C)]
pub struct ft_game_info {
    pub struct_size: u32,
    pub id: *const c_char,
    pub display_name: *const c_char,
    pub version: *const c_char,
    pub author: *const c_char,
    pub description: *const c_char,
    pub url: *const c_char,
    pub thumbnail: *const c_char,
}

#[repr(C)]
pub struct ft_game_variant {
    pub id: *const c_char,
    pub display_name: *const c_char,
    pub description: *const c_char,
}

#[repr(C)]
pub struct ft_game_constraints {
    pub struct_size: u32,
    pub caps: u32,
    /// FT_DIMENSIONS_2D / _3D. Zero means a plane.
    pub dimensions: u32,
    pub min_players: i32,
    pub max_players: i32,
    pub ticks_per_second: i32,
    pub units_per_tile: f32,
    pub default_camera_height: f32,
    pub variants: *const ft_game_variant,
    pub variant_count: u32,
    pub camera_modes: *const c_void,
    pub camera_mode_count: u32,
    pub level_extension: *const c_char,
    pub level_filter_name: *const c_char,
}

#[repr(C)]
pub struct ft_level_info {
    pub struct_size: u32,
    pub name: *const c_char,
    pub bounds: ft_rect,
    pub width_tiles: i32,
    pub height_tiles: i32,
    pub default_spawn: ft_vec2,
}

#[repr(C)]
pub struct ft_world_desc {
    pub struct_size: u32,
    pub level: *const c_void,
    pub variant_id: *const c_char,
    pub player_count: i32,
    pub world_index: i32,
}

#[repr(C)]
pub struct ft_player_view {
    pub struct_size: u32,
    pub position: ft_vec2,
    pub velocity: ft_vec2,
    pub aim: ft_vec2,
    pub flags: u32,
    pub run_start_tick: i32,
}

#[repr(C)]
pub struct ft_player_setup {
    pub struct_size: u32,
    pub name: *const c_char,
    pub tag: *const c_char,
    pub appearance_id: *const c_char,
    pub primary_color: ft_color,
    pub secondary_color: ft_color,
    pub use_custom_color: bool,
    pub linked_player: i32,
}

#[repr(C)]
pub struct ft_camera {
    pub struct_size: u32,
    pub position: ft_vec2,
    pub zoom: f32,
    pub aspect: f32,
    pub mode: u32,
    pub viewport: ft_vec2,
    pub visible: ft_rect,
    // Filled only for a game that declared three dimensions.
    pub eye: ft_vec3,
    pub target: ft_vec3,
    pub up: ft_vec3,
    pub fov_y: f32,
    pub near_z: f32,
    pub far_z: f32,
    pub view_proj: [f32; 16],
}

#[repr(C)]
pub struct ft_engine_state {
    pub struct_size: u32,
    pub current_tick: i32,
    pub playing: bool,
    pub recording: bool,
    pub headless: bool,
    pub selected_player: i32,
    pub camera: ft_camera,
}

#[repr(C)]
pub struct ft_render_frame {
    pub struct_size: u32,
    pub pass: u32,
    pub level: *const c_void,
    pub world: *const c_void,
    pub previous_world: *const c_void,
    pub alpha: f32,
    pub tick: i32,
    pub player_setups: *const ft_player_setup,
    pub player_setup_count: u32,
    pub state: ft_engine_state,
    pub opacity: f32,
    pub accent: ft_color,
    pub selected_player: i32,
    pub world_index: i32,
    pub world_count: i32,
    pub active: bool,
}

#[repr(C)]
pub struct ft_ui_frame {
    pub struct_size: u32,
    pub slot: u32,
    pub world: *const c_void,
    pub tick: i32,
    pub player: i32,
    pub state: ft_engine_state,
}

#[repr(C)]
pub struct ft_sprite_draw {
    pub pos: ft_vec2,
    pub size: ft_vec2,
    pub rotation: f32,
    pub sprite_index: u32,
    pub color: ft_color,
    pub tiling: ft_vec2,
}

/// The engine's Vulkan device, handed over so a renderer that can adopt an
/// existing device renders into engine memory instead of copying pixels.
#[repr(C)]
pub struct ft_gpu_device {
    pub struct_size: u32,
    pub api: u32,
    pub instance: *mut c_void,
    pub physical_device: *mut c_void,
    pub device: *mut c_void,
    pub queue: *mut c_void,
    pub queue_family_index: u32,
    pub api_version: u32,
}

#[repr(C)]
pub struct ft_texture_desc {
    pub struct_size: u32,
    pub pixels: *const c_void,
    pub width: u32,
    pub height: u32,
    pub layers: u32,
    pub format: u32,
    pub mipmaps: bool,
    pub linear_filter: bool,
}

#[repr(C)]
pub struct ft_sprite_rect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

#[repr(C)]
pub struct ft_atlas_desc {
    pub struct_size: u32,
    pub texture: *mut c_void,
    pub sprites: *const ft_sprite_rect,
    pub sprite_count: u32,
    pub max_instances_per_frame: u32,
}

/// The image behind an `ft_texture`.
#[repr(C)]
pub struct ft_gpu_image {
    pub struct_size: u32,
    pub image: *mut c_void,
    pub format: u64,
    pub width: u32,
    pub height: u32,
    pub layers: u32,
    pub layout: u32,
}

pub const FT_GPU_API_VULKAN: u32 = 1;

/// The engine's service table. Only the entries this example calls are typed
/// precisely; the rest are opaque pointers, which is safe because the module
/// never invokes them and the layout is preserved either way.
#[repr(C)]
pub struct ft_engine_api {
    pub struct_size: u32,
    pub log:
        Option<unsafe extern "C" fn(level: c_int, category: *const c_char, message: *const c_char)>,

    pub resolve_data_path: *const c_void,
    pub read_file: *const c_void,
    pub free_file_data: *const c_void,
    pub resolve_cache_path: *const c_void,

    pub texture_create: Option<unsafe extern "C" fn(desc: *const ft_texture_desc) -> *mut c_void>,
    pub texture_destroy: *const c_void,
    pub texture_update_layer: *const c_void,
    pub atlas_create: Option<unsafe extern "C" fn(desc: *const ft_atlas_desc) -> *mut c_void>,
    pub atlas_destroy: *const c_void,
    pub pipeline_create: *const c_void,
    pub pipeline_destroy: *const c_void,
    pub mesh_create: *const c_void,
    pub mesh_destroy: *const c_void,

    pub draw_sprites: Option<
        unsafe extern "C" fn(atlas: *mut c_void, z: f32, draws: *const ft_sprite_draw, count: u32),
    >,
    pub draw_line: Option<
        unsafe extern "C" fn(z: f32, a: ft_vec2, b: ft_vec2, color: ft_color, thickness: f32),
    >,
    pub draw_rect:
        Option<unsafe extern "C" fn(z: f32, pos: ft_vec2, size: ft_vec2, color: ft_color)>,
    pub draw_circle: Option<
        unsafe extern "C" fn(z: f32, center: ft_vec2, radius: f32, color: ft_color, segments: u32),
    >,
    pub draw_triangle: *const c_void,
    pub draw_text: *const c_void,

    // The header declares the 3D primitives here, between draw_text and
    // draw_instances. Position is the contract: appending them at the end
    // instead shifts every later entry and the module calls the wrong function.
    pub draw_line3:
        Option<unsafe extern "C" fn(a: ft_vec3, b: ft_vec3, color: ft_color, thickness: f32)>,
    pub draw_triangle3:
        Option<unsafe extern "C" fn(a: ft_vec3, b: ft_vec3, c: ft_vec3, color: ft_color)>,
    pub draw_box3:
        Option<unsafe extern "C" fn(center: ft_vec3, size: ft_vec3, color: ft_color, wire: bool)>,
    pub draw_instances: *const c_void,
    pub draw_mesh: *const c_void,

    pub camera_get: *const c_void,
    pub camera_set: *const c_void,
    pub screen_to_world: *const c_void,
    pub world_to_screen: *const c_void,

    pub imgui_context: *const c_void,
    pub imgui_allocators: *const c_void,

    pub request_level: Option<unsafe extern "C" fn(path: *const c_char) -> bool>,
    pub imgui_texture_id: *const c_void,
    pub imgui_texture_release: *const c_void,
    pub mark_dirty: *const c_void,
    pub invalidate_simulation: *const c_void,
    pub get_state: *const c_void,
    pub get_player_input: *const c_void,
    pub save_file_dialog: *const c_void,
    pub open_file_dialog: *const c_void,
    pub visit_directory: *const c_void,
    pub get_player_setup: *const c_void,
    pub set_player_appearance: *const c_void,
    pub timeline_world_count: *const c_void,
    pub timeline_world_info: *const c_void,
    pub timeline_world_pair: *const c_void,
    pub timeline_player_track: *const c_void,
    pub render_instances_preview: *const c_void,

    pub gpu_device: Option<unsafe extern "C" fn() -> *const ft_gpu_device>,
    pub texture_gpu_image:
        Option<unsafe extern "C" fn(texture: *mut c_void, out: *mut ft_gpu_image) -> bool>,
    pub draw_texture:
        Option<unsafe extern "C" fn(z: f32, texture: *mut c_void, dst: ft_rect, tint: ft_color)>,
}

/// The module vtable. Field order and count must match `ft_game_module`
/// exactly; the engine also checks `struct_size` against its own definition.
#[repr(C)]
pub struct ft_game_module {
    pub struct_size: u32,
    pub abi_version: u32,
    pub abi_revision: u32,

    pub info: ft_game_info,
    pub constraints: ft_game_constraints,

    pub input_schema: *const ft_input_schema,
    pub entity_classes: *const c_void,
    pub entity_class_count: u32,

    pub create: Option<unsafe extern "C" fn(engine: *const ft_engine_api) -> *mut c_void>,
    pub destroy: Option<unsafe extern "C" fn(game: *mut c_void)>,
    pub update: *const c_void,

    pub level_load_path:
        Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> *mut c_void>,
    pub level_load_memory: Option<
        unsafe extern "C" fn(*mut c_void, *const c_void, usize, *const c_char) -> *mut c_void,
    >,
    pub level_destroy: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub level_info:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void, *mut ft_level_info) -> bool>,
    pub level_serialize: *const c_void,

    pub world_create:
        Option<unsafe extern "C" fn(*mut c_void, *const ft_world_desc) -> *mut c_void>,
    pub world_destroy: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub world_copy: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_void)>,
    pub world_step: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_void, u32)>,
    pub world_tick: Option<unsafe extern "C" fn(*mut c_void, *const c_void) -> i32>,
    pub world_player_count: Option<unsafe extern "C" fn(*mut c_void, *const c_void) -> i32>,
    pub world_player_view:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void, i32, *mut ft_player_view) -> bool>,
    pub world_add_player: *const c_void,
    pub world_remove_player: *const c_void,
    pub world_serialize:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void, *mut c_void, usize) -> usize>,
    pub world_deserialize:
        Option<unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_void, usize) -> bool>,

    pub input_default: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub input_get: Option<unsafe extern "C" fn(*mut c_void, *const c_void, u32) -> i64>,
    pub input_set: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, u32, i64)>,
    pub input_get_vec2: Option<unsafe extern "C" fn(*mut c_void, *const c_void, u32) -> ft_vec2>,
    pub input_set_vec2: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, u32, ft_vec2)>,
    pub input_describe: *const c_void,

    pub entity_prop_get: *const c_void,
    pub entity_prop_set: *const c_void,
    pub entity_count: *const c_void,

    pub render: Option<unsafe extern "C" fn(*mut c_void, *const ft_render_frame)>,
    pub resources_create: *const c_void,
    pub resources_destroy: *const c_void,

    pub ui: Option<unsafe extern "C" fn(*mut c_void, *const ft_ui_frame)>,
    pub collect_events: *const c_void,

    pub exporter_count: *const c_void,
    pub exporter_desc: *const c_void,
    pub export_run: *const c_void,

    pub status_lines: *const c_void,
    pub player_label: *const c_void,
    pub camera_update: *const c_void,

    pub setting_count: *const c_void,
    pub setting_desc: *const c_void,
    pub setting_get: *const c_void,
    pub setting_set: *const c_void,

    pub project_save: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, usize) -> usize>,
    pub project_load: Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> bool>,
    pub input_get_float: *const c_void,
    pub input_set_float: *const c_void,
    pub linked_actions: *const c_void,
    pub linked_action_count: u32,
    pub linked_input_update: *const c_void,
}

// The vtable is immutable static data shared with the engine thread.
unsafe impl Sync for ft_game_module {}
