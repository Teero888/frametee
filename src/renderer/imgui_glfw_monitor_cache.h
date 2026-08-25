#ifndef IMGUI_GLFW_MONITOR_CACHE_H
#define IMGUI_GLFW_MONITOR_CACHE_H
// ImGui_ImplGlfw_UpdateMonitors() re-queries every monitor each frame, which is
// expensive enough on X11 to show up in profiles. The wrappers below are cached
// stand-ins implemented in graphics_backend.c; this header is force-included
// into imgui_impl_glfw.cpp (see the top-level CMakeLists.txt) so its GLFW
// monitor calls land on them instead.
//
// The redirects deliberately live behind <GLFW/glfw3.h>: renaming the functions
// with -D instead would also rename their declarations in glfw3.h, so on
// Windows the wrappers would inherit GLFW's dllimport attribute and the link
// would go looking for __imp_mock_glfw* in glfw's import library.
#include <GLFW/glfw3.h>

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
#ifndef IMGUI_GLFW_MONITOR_CACHE_NO_REDIRECT
#define glfwGetMonitors mock_glfwGetMonitors
#define glfwGetMonitorPos mock_glfwGetMonitorPos
#define glfwGetVideoMode mock_glfwGetVideoMode
#define glfwGetMonitorWorkarea mock_glfwGetMonitorWorkarea
#define glfwGetMonitorContentScale mock_glfwGetMonitorContentScale
#endif

#endif
