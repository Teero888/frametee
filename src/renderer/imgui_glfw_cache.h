#ifndef IMGUI_GLFW_CACHE_H
#define IMGUI_GLFW_CACHE_H
// The ImGui GLFW backend asks GLFW about the monitors and the window every
// frame: ImGui_ImplGlfw_UpdateMonitors() re-queries every monitor, and
// ImGui_ImplGlfw_NewFrame() the window's sizes, focus, hover and cursor. On X11
// each of those is a round trip to the server, enough to show up in profiles.
// The stand-ins below answer from caches instead: the monitor wrappers in
// graphics_backend.c, the window's state in system/window_state.c. This header
// is force-included into imgui_impl_glfw.cpp (see the top-level CMakeLists.txt)
// so its GLFW calls land on them.
//
// The redirects deliberately live behind <GLFW/glfw3.h>: renaming the functions
// with -D instead would also rename their declarations in glfw3.h, so on
// Windows the wrappers would inherit GLFW's dllimport attribute and the link
// would go looking for __imp_mock_glfw* in glfw's import library.
#include <GLFW/glfw3.h>
#include <system/window_state.h>

#ifdef __cplusplus
extern "C" {
#endif

GLFWmonitor **mock_glfwGetMonitors(int *count);
void mock_glfwGetMonitorPos(GLFWmonitor *monitor, int *xpos, int *ypos);
const GLFWvidmode *mock_glfwGetVideoMode(GLFWmonitor *monitor);
void mock_glfwGetMonitorWorkarea(GLFWmonitor *monitor, int *xpos, int *ypos, int *width, int *height);
void mock_glfwGetMonitorContentScale(GLFWmonitor *monitor, float *xscale, float *yscale);

#ifdef __cplusplus
}
#endif

// graphics_backend.c defines this before including the header: it implements the
// wrappers and calls the real GLFW functions, so it must not redirect them.
#ifndef IMGUI_GLFW_CACHE_NO_REDIRECT
#define glfwGetMonitors mock_glfwGetMonitors
#define glfwGetMonitorPos mock_glfwGetMonitorPos
#define glfwGetVideoMode mock_glfwGetVideoMode
#define glfwGetMonitorWorkarea mock_glfwGetMonitorWorkarea
#define glfwGetMonitorContentScale mock_glfwGetMonitorContentScale
#define glfwGetWindowSize window_state_get_size
#define glfwGetFramebufferSize window_state_get_framebuffer_size
#define glfwGetWindowAttrib window_state_get_attrib
#define glfwSetWindowAttrib window_state_set_attrib
#define glfwGetCursorPos window_state_get_cursor_pos
#endif

#endif
