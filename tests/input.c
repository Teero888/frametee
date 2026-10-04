// Exercise real input polling with a deterministic clock and system settings.
// No window, display server, or OS keyboard configuration changes are needed.
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <system/input.h>
#include <system/input_repeat.h>

static int keys[GLFW_KEY_LAST + 1];
static int buttons[GLFW_MOUSE_BUTTON_LAST + 1];
static bool focused;
static double now;
static int refresh_count;
static input_repeat_settings_t settings;

int glfwGetKey(GLFWwindow *window, int key) {
  (void)window;
  return keys[key];
}
int glfwGetMouseButton(GLFWwindow *window, int button) {
  (void)window;
  return buttons[button];
}
int glfwGetWindowAttrib(GLFWwindow *window, int attrib) {
  (void)window;
  assert(attrib == GLFW_FOCUSED);
  return focused;
}
double glfwGetTime(void) { return now; }
void glfwSetInputMode(GLFWwindow *window, int mode, int value) {
  (void)window;
  (void)mode;
  (void)value;
}
int glfwRawMouseMotionSupported(void) { return GLFW_FALSE; }
void glfwGetCursorPos(GLFWwindow *window, double *x, double *y) {
  (void)window;
  *x = *y = 0.0;
}
void input_repeat_init(void) { input_repeat_refresh(); }
void input_repeat_shutdown(void) {}
void input_repeat_refresh(void) { ++refresh_count; }
input_repeat_settings_t input_repeat_settings(void) { return settings; }

static void reset(void) {
  memset(keys, 0, sizeof(keys));
  memset(buttons, 0, sizeof(buttons));
  focused = true;
  now = 0.0;
  refresh_count = 0;
  settings = (input_repeat_settings_t){.enabled = true, .delay = 0.5, .interval = 0.05};
  input_init((GLFWwindow *)1);
}

static void frame(double time) {
  now = time;
  input_new_frame();
}

static void mouse_repeat(void) {
  // Any mouse button may be rebound to stepping; repeat is controlled by the caller.
  for (int button = 0; button <= GLFW_MOUSE_BUTTON_LAST; ++button) {
    reset();
    buttons[button] = GLFW_PRESS;
    frame(0.0);
    assert(input_mouse_pressed(button, false));
    assert(input_mouse_pressed(button, true));
    frame(0.499);
    assert(input_mouse_down(button));
    assert(!input_mouse_pressed(button, true));
    frame(0.501);
    assert(input_mouse_pressed(button, true));
    // Read-only queries must not consume a repeat or trigger a single-press action.
    assert(input_mouse_pressed(button, true));
    assert(!input_mouse_pressed(button, false));
    assert(input_capture_pressed_key() == ImGuiKey_None);
    frame(0.549);
    assert(!input_mouse_pressed(button, true));
    frame(0.551);
    assert(input_mouse_pressed(button, true));
    buttons[button] = GLFW_RELEASE;
    frame(0.601);
    assert(!input_mouse_pressed(button, true));
    buttons[button] = GLFW_PRESS;
    frame(0.7);
    assert(input_mouse_pressed(button, false));
    frame(1.199);
    assert(!input_mouse_pressed(button, true));
    frame(1.201);
    assert(input_mouse_pressed(button, true));
  }
}

static void repeat_settings(void) {
  reset();
  settings.delay = 0.8;
  settings.interval = 0.125;
  buttons[GLFW_MOUSE_BUTTON_4] = GLFW_PRESS;
  frame(0.0);
  frame(0.799);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  frame(0.801);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  frame(0.924);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  frame(0.926);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));

  // A changed system setting restarts the delay for held buttons.
  settings.delay = 0.25;
  settings.interval = 0.1;
  frame(1.0);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  frame(1.251);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  settings.enabled = false;
  frame(2.0);
  frame(3.0);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  assert(input_mouse_down(GLFW_MOUSE_BUTTON_4));
  buttons[GLFW_MOUSE_BUTTON_4] = GLFW_RELEASE;
  frame(3.1);
  buttons[GLFW_MOUSE_BUTTON_4] = GLFW_PRESS;
  frame(3.2);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
  frame(4.0);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));

  settings.enabled = true;
  settings.interval = 0.0;
  frame(5.0);
  frame(6.0);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_4, true));
}

static void focus_and_stalls(void) {
  reset();
  buttons[GLFW_MOUSE_BUTTON_5] = GLFW_PRESS;
  frame(0.0);
  // A stall delivers one repeat and retains the original cadence, without a backlog.
  frame(2.021);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
  frame(2.022);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
  frame(2.051);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
  focused = false;
  frame(3.0);
  assert(!input_mouse_down(GLFW_MOUSE_BUTTON_5));
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
  frame(4.0);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
  focused = true;
  frame(5.0);
  assert(refresh_count == 2);
  frame(5.499);
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
  frame(5.501);
  assert(input_mouse_pressed(GLFW_MOUSE_BUTTON_5, true));
}

static void keyboard_and_capture(void) {
  reset();
  buttons[GLFW_MOUSE_BUTTON_4] = GLFW_PRESS;
  frame(0.0);
  assert(input_capture_pressed_key() == ImGuiKey_MouseX1);
  frame(0.501);
  assert(input_capture_pressed_key() == ImGuiKey_None);
  keys[GLFW_KEY_RIGHT] = GLFW_PRESS;
  frame(0.6);
  assert(input_key_pressed(GLFW_KEY_RIGHT, false));
  frame(1.6);
  assert(!input_key_pressed(GLFW_KEY_RIGHT, true));
  input_accumulate_key_repeat(GLFW_KEY_RIGHT);
  frame(1.7);
  assert(input_key_pressed(GLFW_KEY_RIGHT, true));
  assert(!input_key_pressed(GLFW_KEY_RIGHT, false));
  keys[GLFW_KEY_RIGHT] = GLFW_RELEASE;
  input_accumulate_key_repeat(GLFW_KEY_RIGHT);
  frame(1.8);
  assert(!input_key_pressed(GLFW_KEY_RIGHT, true));
  assert(!input_mouse_pressed(-1, true));
  assert(!input_mouse_pressed(GLFW_MOUSE_BUTTON_LAST + 1, true));
  input_shutdown();
  assert(!input_mouse_down(GLFW_MOUSE_BUTTON_4));
  input_new_frame();
}

int main(void) {
  mouse_repeat();
  repeat_settings();
  focus_and_stalls();
  keyboard_and_capture();
  puts("Input repeat tests passed");
  return 0;
}
