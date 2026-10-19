#ifndef WINDOW_STATE_H
#define WINDOW_STATE_H
// The main window's state as GLFW's callbacks last reported it.
//
// Under X11 every GLFW getter for it is a round trip to the X server
// (XGetWindowAttributes for the sizes, XGetInputFocus for the focus,
// XQueryPointer for the cursor and the hover), and the editor and its ImGui
// backend between them asked about ten times a frame. Through Xwayland that
// was a large share of a frame. The callbacks report every change, so the
// answers here are the same ones, without asking.
//
// The getters take GLFW's own signatures: imgui_glfw_cache.h redirects the ImGui
// backend's calls to them. Any window but the followed one is asked for real.
#include <stdbool.h>

typedef struct GLFWwindow GLFWwindow;

#ifdef __cplusplus
extern "C" {
#endif

// Starts following `window`, from what GLFW says about it now.
void window_state_init(GLFWwindow *window);

// What the window's callbacks report (graphics_backend.c forwards them here).
void window_state_on_size(GLFWwindow *window, int width, int height);
void window_state_on_framebuffer_size(GLFWwindow *window, int width, int height);
void window_state_on_focus(GLFWwindow *window, int focused);
void window_state_on_iconify(GLFWwindow *window, int iconified);
void window_state_on_enter(GLFWwindow *window, int entered);
void window_state_on_cursor(GLFWwindow *window, double x, double y);

void window_state_get_size(GLFWwindow *window, int *width, int *height);
void window_state_get_framebuffer_size(GLFWwindow *window, int *width, int *height);
// GLFW_FOCUSED, GLFW_ICONIFIED and GLFW_HOVERED come from the callbacks; anything else from GLFW.
int window_state_get_attrib(GLFWwindow *window, int attrib);
// Setting an attribute to what it already is does nothing (ImGui sets GLFW_MOUSE_PASSTHROUGH
// every frame, and X11 sends a request for it each time).
void window_state_set_attrib(GLFWwindow *window, int attrib, int value);
void window_state_get_cursor_pos(GLFWwindow *window, double *x, double *y);

#ifdef __cplusplus
}
#endif

#endif // WINDOW_STATE_H
