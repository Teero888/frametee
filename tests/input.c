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

static void wheel_notches(void) {
  reset();
  // Each notch is pressed once, on a frame of its own, in the order the wheel turned.
  input_accumulate_scroll(0.0, 1.0);
  input_accumulate_scroll(0.0, 1.0);
  input_accumulate_scroll(0.0, -1.0);
  frame(0.0);
  assert(input_wheel_notch() == 1);
  assert(input_scroll_y() == 1.0);
  frame(0.01);
  assert(input_wheel_notch() == 1);
  assert(input_scroll_y() == 0.0);
  frame(0.02);
  assert(input_wheel_notch() == -1);
  frame(0.03);
  assert(input_wheel_notch() == 0);

  // Touchpads and high resolution wheels report fractions, which add up to notches.
  input_accumulate_scroll(0.0, 0.5);
  frame(0.04);
  assert(input_wheel_notch() == 0);
  input_accumulate_scroll(0.0, 0.25);
  input_accumulate_scroll(0.0, 0.25);
  frame(0.05);
  assert(input_wheel_notch() == 1);
  // Turning back drops the part notch instead of finishing it the other way.
  input_accumulate_scroll(0.0, 0.75);
  input_accumulate_scroll(0.0, -0.5);
  frame(0.06);
  assert(input_wheel_notch() == 0);
  input_accumulate_scroll(0.0, -0.5);
  frame(0.07);
  assert(input_wheel_notch() == -1);

  // Notches the platform merged into one event still count one by one.
  input_accumulate_scroll(0.0, -2.0);
  frame(0.08);
  assert(input_wheel_notch() == -1);
  frame(0.09);
  assert(input_wheel_notch() == -1);
  frame(0.1);
  assert(input_wheel_notch() == 0);

  // A spin that outruns the queue drops the rest instead of pressing on after it stopped.
  for (int i = 0; i < 100; ++i)
    input_accumulate_scroll(0.0, 1.0);
  int pressed = 0;
  for (int i = 0; i < 100; ++i) {
    frame(0.2 + i * 0.01);
    pressed += input_wheel_notch();
  }
  assert(pressed > 1 && pressed < 100);

  // The rebinding UI takes a notch as a key.
  input_accumulate_scroll(0.0, -1.0);
  frame(2.0);
  assert(input_capture_pressed_key() == INPUT_KEY_WHEEL_DOWN);
  frame(2.01);
  assert(input_capture_pressed_key() == ImGuiKey_None);

  // Like the buttons, the wheel presses nothing while unfocused, then or later.
  focused = false;
  input_accumulate_scroll(0.0, 1.0);
  frame(3.0);
  assert(input_wheel_notch() == 0);
  focused = true;
  frame(3.01);
  assert(input_wheel_notch() == 0);
}

static void modifier_capture(void) {
  reset();
  // Holding Ctrl starts a combo, so it is not bound on its own while it goes down.
  keys[GLFW_KEY_LEFT_CONTROL] = GLFW_PRESS;
  frame(0.0);
  assert(input_capture_pressed_key() == ImGuiKey_None);
  input_accumulate_scroll(0.0, 1.0);
  frame(0.1);
  assert(input_capture_pressed_key() == INPUT_KEY_WHEEL_UP);
  assert(input_ctrl_down());
  keys[GLFW_KEY_S] = GLFW_PRESS;
  frame(0.2);
  assert(input_capture_pressed_key() == ImGuiKey_S);

  // A modifier on its own is bound when it is let go.
  reset();
  keys[GLFW_KEY_LEFT_SHIFT] = GLFW_PRESS;
  frame(0.0);
  assert(input_capture_pressed_key() == ImGuiKey_None);
  frame(0.1);
  assert(input_capture_pressed_key() == ImGuiKey_None);
  keys[GLFW_KEY_LEFT_SHIFT] = GLFW_RELEASE;
  frame(0.2);
  assert(input_capture_pressed_key() == ImGuiKey_LeftShift);
  frame(0.3);
  assert(input_capture_pressed_key() == ImGuiKey_None);
}

static void blocked_while_dialog(void) {
  reset();
  // While a file dialog is up nothing reads as pressed or moved, and nothing is left over after.
  input_set_blocked(true);
  keys[GLFW_KEY_D] = GLFW_PRESS;
  buttons[GLFW_MOUSE_BUTTON_LEFT] = GLFW_PRESS;
  input_accumulate_scroll(0.0, 1.0);
  input_accumulate_mouse_pos(5.0, 3.0, NULL, NULL);
  frame(0.0);
  assert(!input_key_down(GLFW_KEY_D) && !input_mouse_down(GLFW_MOUSE_BUTTON_LEFT));
  assert(input_wheel_notch() == 0 && input_scroll_y() == 0.0);
  double dx, dy;
  input_mouse_delta(&dx, &dy);
  assert(dx == 0.0 && dy == 0.0);
  input_set_blocked(false);
  frame(0.1);
  assert(input_wheel_notch() == 0);
  // Keys still down when it closes count from then on, as a fresh press.
  assert(input_key_pressed(GLFW_KEY_D, false) && input_mouse_pressed(GLFW_MOUSE_BUTTON_LEFT, false));
}

int main(void) {
  mouse_repeat();
  repeat_settings();
  focus_and_stalls();
  keyboard_and_capture();
  wheel_notches();
  modifier_capture();
  blocked_while_dialog();
  puts("Input repeat tests passed");
  return 0;
}
