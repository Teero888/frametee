#include "graphics_backend.h"
#include "renderer.h"

int main(void) {
  gfx_handler_t handler;
  if (init_gfx_handler(&handler) != 0)
    return 1;

  shader_t *shader =
      renderer_load_shader(&handler, "data/shaders/simple.vert.spv", "data/shaders/simple.frag.spv");
  if (!shader)
    return 1;

  vertex_t quad_vertices[] = {
      {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}}, // Bottom Left - Red
      {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},  // Bottom Right - Green
      {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},   // Top Right - Blue
      {{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}   // Top Left - White
  };
  uint32_t quad_indices[] = {
      0, 1, 2, // First triangle
      2, 3, 0  // Second triangle
  };

  mesh_t *quad_mesh =
      renderer_create_mesh(&handler, quad_vertices, sizeof(quad_vertices) / sizeof(quad_vertices[0]),
                           quad_indices, sizeof(quad_indices) / sizeof(quad_indices[0]));
  if (!quad_mesh)
    return 1;
  renderable_t *quad_renderable = renderer_add_renderable(&handler, quad_mesh, shader, NULL);
  if (!quad_renderable)
    return 1;

  while (gfx_next_frame(&handler) == 0)
    ;
  gfx_cleanup(&handler);
  return 0;
}
