#include "dd_imgui.h"

// ImGui keeps its context and allocator in globals. A shared library gets its
// own copies of those, so both have to be pointed at the editor's before any
// ig* call, or the module would build widgets into a context nobody draws.
void dd_imgui_attach(const ft_engine_api *engine) {
  static bool attached = false;
  if (attached || !engine || !engine->imgui_context) return;

  ImGuiContext *context = (ImGuiContext *)engine->imgui_context();
  if (!context) return;

  if (engine->imgui_allocators) {
    void *alloc_fn = NULL;
    void *free_fn = NULL;
    void *user_data = NULL;
    engine->imgui_allocators(&alloc_fn, &free_fn, &user_data);
    if (alloc_fn && free_fn) igSetAllocatorFunctions((ImGuiMemAllocFunc)alloc_fn, (ImGuiMemFreeFunc)free_fn, user_data);
  }

  igSetCurrentContext(context);
  attached = true;
}
