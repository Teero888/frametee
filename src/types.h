#ifndef TYPES_H
#define TYPES_H

// Miscellaneous types
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Physics
typedef struct physics_v_t physics_v_t;

// Plugins
typedef struct plugin_manager_t plugin_manager_t;
typedef struct loaded_plugin_t loaded_plugin_t;

// Renderer
typedef struct custom_pipeline_t custom_pipeline_t;
typedef struct vertex_layout_t vertex_layout_t;
typedef struct pipeline_cache_entry_t pipeline_cache_entry_t;
typedef struct sprite_definition_t sprite_definition_t;
typedef struct map_buffer_object_t map_buffer_object_t;
typedef struct primitive_vertex_t primitive_vertex_t;
typedef struct primitive3d_vertex_t primitive3d_vertex_t;
typedef struct render_command_t render_command_t;
typedef struct render_queue_t render_queue_t;
typedef struct renderer_state_t renderer_state_t;
typedef struct atlas_renderer_t atlas_renderer_t;
typedef struct atlas_instance_t atlas_instance_t;
typedef struct primitive_ubo_t primitive_ubo_t;
typedef struct gfx_handler_t gfx_handler_t;
typedef struct texture_t texture_t;
typedef struct vertex_t vertex_t;
typedef struct shader_t shader_t;
typedef struct camera_t camera_t;
typedef struct camera3_t camera3_t;
typedef struct buffer_t buffer_t;
typedef struct mesh_t mesh_t;

// User Interface
typedef struct ui_handler_t ui_handler_t;

// Keybinds
typedef struct keybind_manager_t keybind_manager_t;
typedef struct keybind_entry_t keybind_entry_t;
typedef struct action_info_t action_info_t;
typedef struct key_combo_t key_combo_t;

// Player Info
typedef struct player_info_t player_info_t;

// Undo/Redo
#ifndef FRAMETEE_UNDO_COMMAND_T_DEFINED
#define FRAMETEE_UNDO_COMMAND_T_DEFINED
typedef struct undo_command_t undo_command_t;
#endif
typedef struct undo_manager_t undo_manager_t;

// Timeline
typedef struct recording_snippet_vector_t recording_snippet_vector_t;
typedef struct timeline_drag_state_t timeline_drag_state_t;
typedef struct timeline_trim_state_t timeline_trim_state_t;
typedef struct snippet_id_vector_t snippet_id_vector_t;
typedef struct dragged_snippet_info_t dragged_snippet_info_t;
typedef struct starting_config_t starting_config_t;
typedef struct timeline_state timeline_state_t;
typedef struct timeline_group_t timeline_group_t;
typedef struct input_snippet_t input_snippet_t;
typedef struct player_track_t player_track_t;
typedef struct timeline_event_t timeline_event_t;

#endif // TYPES_H
