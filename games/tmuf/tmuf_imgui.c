// The module draws its panels with the editor's ImGui. The ig* symbols resolve
// against the host at load time; the context and allocators have to be
// adopted, since a shared library gets its own copies of ImGui's globals.

#include "tmuf_internal.h"

#include <include_cimgui.h>

void tm_imgui_attach(const ft_engine_api *engine) {
  static bool attached = false;
  if (attached || !engine || !engine->imgui_context) return;
  ImGuiContext *context = (ImGuiContext *)engine->imgui_context();
  if (!context) return;
  if (engine->imgui_allocators) {
    void *alloc_fn = NULL, *free_fn = NULL, *user_data = NULL;
    engine->imgui_allocators(&alloc_fn, &free_fn, &user_data);
    if (alloc_fn && free_fn) igSetAllocatorFunctions((ImGuiMemAllocFunc)alloc_fn, (ImGuiMemFreeFunc)free_fn, user_data);
  }
  igSetCurrentContext(context);
  attached = true;
}
