#include "graphics_backend.h"
#include "renderer.h"

int main(void) {
  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0)
    return 1;

  shader_t *shader = renderer_load_shader(&handler, "data/shaders/map.vert.spv", "data/shaders/map.frag.spv");
  if (!shader)
    return 1;

  vertex_t quad_vertices[] = {
      {{-1.f, -1.f}, {1.0f, 1.0f, 1.0f}, {-1.f, 1.0f}}, // Top Left
      {{1.0f, -1.f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}}, // Top Right
      {{1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, -1.f}}, // Bottom Right
      {{-1.f, 1.0f}, {1.0f, 1.0f, 1.0f}, {-1.f, -1.f}}  // Bottom Left
  };
  uint32_t quad_indices[] = {
      0, 1, 2, // First triangle
      2, 3, 0  // Second triangle
  };

  mesh_t *quad_mesh =
      renderer_create_mesh(&handler, quad_vertices, sizeof(quad_vertices) / sizeof(quad_vertices[0]),
                           quad_indices, sizeof(quad_indices) / sizeof(quad_indices[0]));
  texture_t *ddnet_texture = renderer_load_texture(&handler, "data/textures/ddnet.png");
  if (!quad_mesh || !ddnet_texture)
    return 1;

  // Set map renderable with ddnet_texture as entities_texture and default_texture as map_texture
  map_renderable_t *quad_renderable =
      renderer_set_map_renderable(&handler, quad_mesh, shader, ddnet_texture, NULL);
  if (!quad_renderable)
    return 1;

  while (gfx_next_frame(&handler) == 0)
    ;
  gfx_cleanup(&handler);
  return 0;
}
