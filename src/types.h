#ifndef TYPES_H
#define TYPES_H

// Miscellaneous types
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// System
typedef struct tas_project_header_t tas_project_header_t;
typedef struct skin_file_header_t skin_file_header_t;

// Physics
typedef struct physics_handler_t physics_handler_t;
typedef struct physics_v_t physics_v_t;

// Plugins
typedef struct plugin_manager_t plugin_manager_t;
typedef struct loaded_plugin_t loaded_plugin_t;
typedef struct tas_context_t tas_context_t;
typedef struct plugin_info_t plugin_info_t;
typedef struct tas_api_t tas_api_t;

typedef void *(*plugin_init_func)(tas_context_t *context, const tas_api_t *api);
typedef void (*plugin_shutdown_func)(void *plugin_data);
typedef void (*plugin_update_func)(void *plugin_data);
typedef plugin_info_t (*get_plugin_info_func)(void);

// Renderer
typedef struct pipeline_cache_entry_t pipeline_cache_entry_t;
typedef struct skin_atlas_manager_t skin_atlas_manager_t;
typedef struct sprite_definition_t sprite_definition_t;
typedef struct map_buffer_object_t map_buffer_object_t;
typedef struct primitive_vertex_t primitive_vertex_t;
typedef struct render_command_t render_command_t;
typedef struct render_queue_t render_queue_t;
typedef struct renderer_state_t renderer_state_t;
typedef struct atlas_renderer_t atlas_renderer_t;
typedef struct atlas_instance_t atlas_instance_t;
typedef struct skin_renderer_t skin_renderer_t;
typedef struct skin_instance_t skin_instance_t;
typedef struct primitive_ubo_t primitive_ubo_t;
typedef struct gfx_handler_t gfx_handler_t;
typedef struct raw_mouse_t raw_mouse_t;
typedef struct texture_t texture_t;
typedef struct vertex_t vertex_t;
typedef struct shader_t shader_t;
typedef struct camera_t camera_t;
typedef struct buffer_t buffer_t;
typedef struct mesh_t mesh_t;

// Animation
typedef struct data_container_t data_container_t;
typedef struct anim_sequence_t anim_sequence_t;
typedef struct anim_keyframe_t anim_keyframe_t;
typedef struct weapon_specs_t weapon_specs_t;
typedef struct weapon_spec_t weapon_spec_t;
typedef struct anim_state_t anim_state_t;
typedef struct animation_t animation_t;

// User Interface
typedef struct demo_exporter_t demo_exporter_t;
typedef struct ui_handler_t ui_handler_t;

// Keybinds
typedef struct keybind_manager_t keybind_manager_t;
typedef struct keybind_entry_t keybind_entry_t;
typedef struct action_info_t action_info_t;
typedef struct key_combo_t key_combo_t;

// Player Info & Skins
typedef struct skin_manager_t skin_manager_t;
typedef struct player_info_t player_info_t;
typedef struct skin_info_t skin_info_t;

// Undo/Redo
typedef struct undo_command_t undo_command_t;
typedef struct undo_manager_t undo_manager_t;

// Timeline
typedef struct recording_snippet_vector_t recording_snippet_vector_t;
typedef struct timeline_drag_state_t timeline_drag_state_t;
typedef struct snippet_id_vector_t snippet_id_vector_t;
typedef struct dragged_snippet_info_t dragged_snippet_info_t;
typedef struct starting_config_t starting_config_t;
typedef struct timeline_state timeline_state_t;
typedef struct input_snippet_t input_snippet_t;
typedef struct player_track_t player_track_t;
typedef struct net_event_t net_event_t;

#endif // TYPES_H
