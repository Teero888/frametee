#ifndef DD_IMGUI_H
#define DD_IMGUI_H

// The module draws its own panels with the editor's ImGui.
//
// Nothing is compiled in: the ig* symbols resolve against the host executable
// at load time, exactly as the bundled plugins do. The only thing that has to
// be arranged is adopting the editor's context, which dd_imgui_attach does.

#ifndef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#endif
#include <cimgui.h>

#include <frametee/game_abi.h>

// Points this module's ImGui at the editor's context and allocator. Safe to
// call every frame; it only does work the first time.
void dd_imgui_attach(const ft_engine_api *engine);

#endif // DD_IMGUI_H
