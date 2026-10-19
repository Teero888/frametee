#include "window_state.h"
#include <GLFW/glfw3.h>

static struct {
  GLFWwindow *window;
  int width, height;
  int framebuffer_width, framebuffer_height;
  bool focused, iconified, hovered;
  double cursor_x, cursor_y;
  // The cursor mode the position was last read in. Switching it moves the
  // pointer (GLFW restores it, or centres it to hide it) without a callback.
  int cursor_mode;
  int mouse_passthrough; // -1 before the first set
} g_state;

static void read_cursor(GLFWwindow *window) {
  glfwGetCursorPos(window, &g_state.cursor_x, &g_state.cursor_y);
  g_state.cursor_mode = glfwGetInputMode(window, GLFW_CURSOR);
}

void window_state_init(GLFWwindow *window) {
  g_state.window = window;
  glfwGetWindowSize(window, &g_state.width, &g_state.height);
  glfwGetFramebufferSize(window, &g_state.framebuffer_width, &g_state.framebuffer_height);
  g_state.focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
  g_state.iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;
  g_state.hovered = glfwGetWindowAttrib(window, GLFW_HOVERED) != 0;
  g_state.mouse_passthrough = -1;
  read_cursor(window);
}

static bool followed(GLFWwindow *window) { return window && window == g_state.window; }

void window_state_on_size(GLFWwindow *window, int width, int height) {
  if (!followed(window)) return;
  g_state.width = width;
  g_state.height = height;
}

void window_state_on_framebuffer_size(GLFWwindow *window, int width, int height) {
  if (!followed(window)) return;
  g_state.framebuffer_width = width;
  g_state.framebuffer_height = height;
}

void window_state_on_focus(GLFWwindow *window, int focused) {
  if (followed(window)) g_state.focused = focused != 0;
}

void window_state_on_iconify(GLFWwindow *window, int iconified) {
  if (followed(window)) g_state.iconified = iconified != 0;
}

void window_state_on_enter(GLFWwindow *window, int entered) {
  if (followed(window)) g_state.hovered = entered != 0;
}

void window_state_on_cursor(GLFWwindow *window, double x, double y) {
  if (!followed(window)) return;
  g_state.cursor_x = x;
  g_state.cursor_y = y;
}

void window_state_get_size(GLFWwindow *window, int *width, int *height) {
  if (!followed(window)) {
    glfwGetWindowSize(window, width, height);
    return;
  }
  if (width) *width = g_state.width;
  if (height) *height = g_state.height;
}

void window_state_get_framebuffer_size(GLFWwindow *window, int *width, int *height) {
  if (!followed(window)) {
    glfwGetFramebufferSize(window, width, height);
    return;
  }
  if (width) *width = g_state.framebuffer_width;
  if (height) *height = g_state.framebuffer_height;
}

int window_state_get_attrib(GLFWwindow *window, int attrib) {
  if (followed(window)) {
    if (attrib == GLFW_FOCUSED) return g_state.focused;
    if (attrib == GLFW_ICONIFIED) return g_state.iconified;
    if (attrib == GLFW_HOVERED) return g_state.hovered;
  }
  return glfwGetWindowAttrib(window, attrib);
}

void window_state_set_attrib(GLFWwindow *window, int attrib, int value) {
  if (followed(window) && attrib == GLFW_MOUSE_PASSTHROUGH) {
    if (g_state.mouse_passthrough == (value != 0)) return;
    g_state.mouse_passthrough = value != 0;
  }
  glfwSetWindowAttrib(window, attrib, value);
}

void window_state_get_cursor_pos(GLFWwindow *window, double *x, double *y) {
  if (!followed(window)) {
    glfwGetCursorPos(window, x, y);
    return;
  }
  if (glfwGetInputMode(window, GLFW_CURSOR) != g_state.cursor_mode) read_cursor(window);
  if (x) *x = g_state.cursor_x;
  if (y) *y = g_state.cursor_y;
}
