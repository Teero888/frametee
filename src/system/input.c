#include "input.h"
#include "input_repeat.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>

#define KEY_COUNT (GLFW_KEY_LAST + 1)
#define BUTTON_COUNT (GLFW_MOUSE_BUTTON_LAST + 1)
#define WHEEL_QUEUE_SIZE 8

typedef struct {
  GLFWwindow *window;
  bool initialized;
  bool focused;

  unsigned char keys[KEY_COUNT];
  unsigned char keys_prev[KEY_COUNT];
  // Repeats come from GLFW, which emits them at the delay and rate configured in the OS, so held
  // binds behave like every other application on the desktop instead of at a rate invented here.
  unsigned char keys_repeat[KEY_COUNT];
  unsigned char pending_repeat[KEY_COUNT];

  unsigned char buttons[BUTTON_COUNT];
  unsigned char buttons_prev[BUTTON_COUNT];
  unsigned char buttons_repeat[BUTTON_COUNT];
  double button_next_repeat[BUTTON_COUNT];
  input_repeat_settings_t repeat_settings;

  // Written by the GLFW callbacks between frames, drained by input_new_frame().
  double pending_dx, pending_dy;
  double pending_scroll_y;

  double frame_dx, frame_dy;
  double frame_scroll_y;
  double cursor_x, cursor_y;
  double last_event_x, last_event_y;

  // Wheel notches not handed out yet, oldest first: +1 up, -1 down. One goes out per frame, so a
  // quick spin presses a bind once per notch instead of once per frame. A spin that outruns the
  // queue drops the rest rather than keep pressing after the wheel has stopped.
  signed char wheel_queue[WHEEL_QUEUE_SIZE];
  int wheel_queue_start, wheel_queue_count;
  // Scroll short of a whole notch, from touchpads and high resolution wheels.
  double wheel_partial;
  int frame_wheel;

  int imgui_to_glfw[ImGuiKey_NamedKey_END];

  // GLFW's keycodes are sparse: there is nothing below GLFW_KEY_SPACE and several gaps above it.
  // Asking glfwGetKey about a code in a gap raises GLFW_INVALID_ENUM, so only real keys are polled.
  int poll_keys[KEY_COUNT];
  int poll_key_count;
} input_state_t;

static input_state_t g_input;

// Converts a GLFW keycode to a ImGui keycode
static ImGuiKey glfw_key_to_imgui(int keycode) {
  switch (keycode) {
  case GLFW_KEY_TAB:
    return ImGuiKey_Tab;
  case GLFW_KEY_LEFT:
    return ImGuiKey_LeftArrow;
  case GLFW_KEY_RIGHT:
    return ImGuiKey_RightArrow;
  case GLFW_KEY_UP:
    return ImGuiKey_UpArrow;
  case GLFW_KEY_DOWN:
    return ImGuiKey_DownArrow;
  case GLFW_KEY_PAGE_UP:
    return ImGuiKey_PageUp;
  case GLFW_KEY_PAGE_DOWN:
    return ImGuiKey_PageDown;
  case GLFW_KEY_HOME:
    return ImGuiKey_Home;
  case GLFW_KEY_END:
    return ImGuiKey_End;
  case GLFW_KEY_INSERT:
    return ImGuiKey_Insert;
  case GLFW_KEY_DELETE:
    return ImGuiKey_Delete;
  case GLFW_KEY_BACKSPACE:
    return ImGuiKey_Backspace;
  case GLFW_KEY_SPACE:
    return ImGuiKey_Space;
  case GLFW_KEY_ENTER:
    return ImGuiKey_Enter;
  case GLFW_KEY_ESCAPE:
    return ImGuiKey_Escape;
  case GLFW_KEY_APOSTROPHE:
    return ImGuiKey_Apostrophe;
  case GLFW_KEY_COMMA:
    return ImGuiKey_Comma;
  case GLFW_KEY_MINUS:
    return ImGuiKey_Minus;
  case GLFW_KEY_PERIOD:
    return ImGuiKey_Period;
  case GLFW_KEY_SLASH:
    return ImGuiKey_Slash;
  case GLFW_KEY_SEMICOLON:
    return ImGuiKey_Semicolon;
  case GLFW_KEY_EQUAL:
    return ImGuiKey_Equal;
  case GLFW_KEY_LEFT_BRACKET:
    return ImGuiKey_LeftBracket;
  case GLFW_KEY_BACKSLASH:
    return ImGuiKey_Backslash;
  case GLFW_KEY_WORLD_1:
    return ImGuiKey_Oem102;
  case GLFW_KEY_WORLD_2:
    return ImGuiKey_Oem102;
  case GLFW_KEY_RIGHT_BRACKET:
    return ImGuiKey_RightBracket;
  case GLFW_KEY_GRAVE_ACCENT:
    return ImGuiKey_GraveAccent;
  case GLFW_KEY_CAPS_LOCK:
    return ImGuiKey_CapsLock;
  case GLFW_KEY_SCROLL_LOCK:
    return ImGuiKey_ScrollLock;
  case GLFW_KEY_NUM_LOCK:
    return ImGuiKey_NumLock;
  case GLFW_KEY_PRINT_SCREEN:
    return ImGuiKey_PrintScreen;
  case GLFW_KEY_PAUSE:
    return ImGuiKey_Pause;
  case GLFW_KEY_KP_0:
    return ImGuiKey_Keypad0;
  case GLFW_KEY_KP_1:
    return ImGuiKey_Keypad1;
  case GLFW_KEY_KP_2:
    return ImGuiKey_Keypad2;
  case GLFW_KEY_KP_3:
    return ImGuiKey_Keypad3;
  case GLFW_KEY_KP_4:
    return ImGuiKey_Keypad4;
  case GLFW_KEY_KP_5:
    return ImGuiKey_Keypad5;
  case GLFW_KEY_KP_6:
    return ImGuiKey_Keypad6;
  case GLFW_KEY_KP_7:
    return ImGuiKey_Keypad7;
  case GLFW_KEY_KP_8:
    return ImGuiKey_Keypad8;
  case GLFW_KEY_KP_9:
    return ImGuiKey_Keypad9;
  case GLFW_KEY_KP_DECIMAL:
    return ImGuiKey_KeypadDecimal;
  case GLFW_KEY_KP_DIVIDE:
    return ImGuiKey_KeypadDivide;
  case GLFW_KEY_KP_MULTIPLY:
    return ImGuiKey_KeypadMultiply;
  case GLFW_KEY_KP_SUBTRACT:
    return ImGuiKey_KeypadSubtract;
  case GLFW_KEY_KP_ADD:
    return ImGuiKey_KeypadAdd;
  case GLFW_KEY_KP_ENTER:
    return ImGuiKey_KeypadEnter;
  case GLFW_KEY_KP_EQUAL:
    return ImGuiKey_KeypadEqual;
  case GLFW_KEY_LEFT_SHIFT:
    return ImGuiKey_LeftShift;
  case GLFW_KEY_LEFT_CONTROL:
    return ImGuiKey_LeftCtrl;
  case GLFW_KEY_LEFT_ALT:
    return ImGuiKey_LeftAlt;
  case GLFW_KEY_LEFT_SUPER:
    return ImGuiKey_LeftSuper;
  case GLFW_KEY_RIGHT_SHIFT:
    return ImGuiKey_RightShift;
  case GLFW_KEY_RIGHT_CONTROL:
    return ImGuiKey_RightCtrl;
  case GLFW_KEY_RIGHT_ALT:
    return ImGuiKey_RightAlt;
  case GLFW_KEY_RIGHT_SUPER:
    return ImGuiKey_RightSuper;
  case GLFW_KEY_MENU:
    return ImGuiKey_Menu;
  case GLFW_KEY_0:
    return ImGuiKey_0;
  case GLFW_KEY_1:
    return ImGuiKey_1;
  case GLFW_KEY_2:
    return ImGuiKey_2;
  case GLFW_KEY_3:
    return ImGuiKey_3;
  case GLFW_KEY_4:
    return ImGuiKey_4;
  case GLFW_KEY_5:
    return ImGuiKey_5;
  case GLFW_KEY_6:
    return ImGuiKey_6;
  case GLFW_KEY_7:
    return ImGuiKey_7;
  case GLFW_KEY_8:
    return ImGuiKey_8;
  case GLFW_KEY_9:
    return ImGuiKey_9;
  case GLFW_KEY_A:
    return ImGuiKey_A;
  case GLFW_KEY_B:
    return ImGuiKey_B;
  case GLFW_KEY_C:
    return ImGuiKey_C;
  case GLFW_KEY_D:
    return ImGuiKey_D;
  case GLFW_KEY_E:
    return ImGuiKey_E;
  case GLFW_KEY_F:
    return ImGuiKey_F;
  case GLFW_KEY_G:
    return ImGuiKey_G;
  case GLFW_KEY_H:
    return ImGuiKey_H;
  case GLFW_KEY_I:
    return ImGuiKey_I;
  case GLFW_KEY_J:
    return ImGuiKey_J;
  case GLFW_KEY_K:
    return ImGuiKey_K;
  case GLFW_KEY_L:
    return ImGuiKey_L;
  case GLFW_KEY_M:
    return ImGuiKey_M;
  case GLFW_KEY_N:
    return ImGuiKey_N;
  case GLFW_KEY_O:
    return ImGuiKey_O;
  case GLFW_KEY_P:
    return ImGuiKey_P;
  case GLFW_KEY_Q:
    return ImGuiKey_Q;
  case GLFW_KEY_R:
    return ImGuiKey_R;
  case GLFW_KEY_S:
    return ImGuiKey_S;
  case GLFW_KEY_T:
    return ImGuiKey_T;
  case GLFW_KEY_U:
    return ImGuiKey_U;
  case GLFW_KEY_V:
    return ImGuiKey_V;
  case GLFW_KEY_W:
    return ImGuiKey_W;
  case GLFW_KEY_X:
    return ImGuiKey_X;
  case GLFW_KEY_Y:
    return ImGuiKey_Y;
  case GLFW_KEY_Z:
    return ImGuiKey_Z;
  case GLFW_KEY_F1:
    return ImGuiKey_F1;
  case GLFW_KEY_F2:
    return ImGuiKey_F2;
  case GLFW_KEY_F3:
    return ImGuiKey_F3;
  case GLFW_KEY_F4:
    return ImGuiKey_F4;
  case GLFW_KEY_F5:
    return ImGuiKey_F5;
  case GLFW_KEY_F6:
    return ImGuiKey_F6;
  case GLFW_KEY_F7:
    return ImGuiKey_F7;
  case GLFW_KEY_F8:
    return ImGuiKey_F8;
  case GLFW_KEY_F9:
    return ImGuiKey_F9;
  case GLFW_KEY_F10:
    return ImGuiKey_F10;
  case GLFW_KEY_F11:
    return ImGuiKey_F11;
  case GLFW_KEY_F12:
    return ImGuiKey_F12;
  case GLFW_KEY_F13:
    return ImGuiKey_F13;
  case GLFW_KEY_F14:
    return ImGuiKey_F14;
  case GLFW_KEY_F15:
    return ImGuiKey_F15;
  case GLFW_KEY_F16:
    return ImGuiKey_F16;
  case GLFW_KEY_F17:
    return ImGuiKey_F17;
  case GLFW_KEY_F18:
    return ImGuiKey_F18;
  case GLFW_KEY_F19:
    return ImGuiKey_F19;
  case GLFW_KEY_F20:
    return ImGuiKey_F20;
  case GLFW_KEY_F21:
    return ImGuiKey_F21;
  case GLFW_KEY_F22:
    return ImGuiKey_F22;
  case GLFW_KEY_F23:
    return ImGuiKey_F23;
  case GLFW_KEY_F24:
    return ImGuiKey_F24;
  default:
    return ImGuiKey_None;
  }
}

void input_init(GLFWwindow *window) {
  input_shutdown();
  g_input.window = window;
  g_input.focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
  input_repeat_init();
  g_input.repeat_settings = input_repeat_settings();

  for (int i = 0; i < ImGuiKey_NamedKey_END; ++i)
    g_input.imgui_to_glfw[i] = -1;

  // The mapping table doubles as the list of keycodes that actually exist.
  for (int keycode = 0; keycode <= GLFW_KEY_LAST; ++keycode) {
    ImGuiKey key = glfw_key_to_imgui(keycode);
    if (key == ImGuiKey_None) continue;
    g_input.imgui_to_glfw[key] = keycode;
    g_input.poll_keys[g_input.poll_key_count++] = keycode;
  }

  // Polling alone drops a key that is pressed and released between two frames, which is easy to do
  // with a quick tap while the frame time is long. Sticky mode holds the press until it has been
  // read once, so a poll can never miss it.
  glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
  glfwSetInputMode(window, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);

  if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

  // Seeded from the real cursor, or the first event to arrive would report a move all the way from
  // the origin.
  glfwGetCursorPos(window, &g_input.cursor_x, &g_input.cursor_y);
  g_input.last_event_x = g_input.cursor_x;
  g_input.last_event_y = g_input.cursor_y;
  g_input.initialized = true;
}

void input_shutdown(void) {
  input_repeat_shutdown();
  memset(&g_input, 0, sizeof(g_input));
}

void input_accumulate_mouse_pos(double x, double y, double *out_dx, double *out_dy) {
  const double dx = x - g_input.last_event_x;
  const double dy = y - g_input.last_event_y;
  g_input.last_event_x = x;
  g_input.last_event_y = y;

  g_input.pending_dx += dx;
  g_input.pending_dy += dy;

  if (out_dx) *out_dx = dx;
  if (out_dy) *out_dy = dy;
}

void input_accumulate_scroll(double x, double y) {
  (void)x;
  g_input.pending_scroll_y += y;

  // GLFW reports a notch as 1.0 on every platform. Smaller steps add up to notches, and turning
  // the wheel back starts over instead of finishing a notch in the other direction.
  if ((y > 0.0 && g_input.wheel_partial < 0.0) || (y < 0.0 && g_input.wheel_partial > 0.0)) g_input.wheel_partial = 0.0;
  g_input.wheel_partial += y;
  const double notches = trunc(g_input.wheel_partial);
  g_input.wheel_partial -= notches;
  const signed char direction = notches > 0.0 ? 1 : -1;
  for (double left = fabs(notches); left >= 1.0 && g_input.wheel_queue_count < WHEEL_QUEUE_SIZE; left -= 1.0)
    g_input.wheel_queue[(g_input.wheel_queue_start + g_input.wheel_queue_count++) % WHEEL_QUEUE_SIZE] = direction;
}

void input_accumulate_key_repeat(int glfw_key) {
  if (glfw_key < 0 || glfw_key > GLFW_KEY_LAST) return;
  g_input.pending_repeat[glfw_key] = 1;
}

void input_new_frame(void) {
  if (!g_input.initialized) return;

  const bool focused = glfwGetWindowAttrib(g_input.window, GLFW_FOCUSED) != 0;
  if (focused && !g_input.focused) input_repeat_refresh();
  g_input.focused = focused;
  const input_repeat_settings_t settings = input_repeat_settings();
  const bool settings_changed = settings.enabled != g_input.repeat_settings.enabled ||
                                settings.delay != g_input.repeat_settings.delay ||
                                settings.interval != g_input.repeat_settings.interval;
  g_input.repeat_settings = settings;
  const double now = glfwGetTime();

  memcpy(g_input.keys_prev, g_input.keys, sizeof(g_input.keys));
  memcpy(g_input.buttons_prev, g_input.buttons, sizeof(g_input.buttons));

  for (int i = 0; i < g_input.poll_key_count; ++i) {
    int key = g_input.poll_keys[i];
    g_input.keys[key] = glfwGetKey(g_input.window, key) == GLFW_PRESS ? 1 : 0;
  }

  memcpy(g_input.keys_repeat, g_input.pending_repeat, sizeof(g_input.keys_repeat));
  memset(g_input.pending_repeat, 0, sizeof(g_input.pending_repeat));

  for (int button = 0; button <= GLFW_MOUSE_BUTTON_LAST; ++button) {
    // Poll even when unfocused to drain sticky presses, but never repeat them.
    const bool down = glfwGetMouseButton(g_input.window, button) == GLFW_PRESS;
    g_input.buttons[button] = down && focused;
    g_input.buttons_repeat[button] = 0;
    if (!g_input.buttons[button]) {
      g_input.button_next_repeat[button] = 0.0;
    } else if (!g_input.buttons_prev[button] || settings_changed) {
      g_input.button_next_repeat[button] = now + settings.delay;
    } else if (settings.enabled && settings.interval > 0.0 && now >= g_input.button_next_repeat[button]) {
      g_input.buttons_repeat[button] = 1;
      // Preserve the system cadence, dropping missed repeats after a long
      // frame instead of delivering a burst on subsequent frames.
      const double missed = floor((now - g_input.button_next_repeat[button]) / settings.interval);
      g_input.button_next_repeat[button] += (missed + 1.0) * settings.interval;
    }
  }

  // Drain what the callbacks collected since the previous frame; nothing is lost regardless of how
  // many events arrived between frames.
  g_input.frame_dx = g_input.pending_dx;
  g_input.frame_dy = g_input.pending_dy;
  g_input.frame_scroll_y = g_input.pending_scroll_y;
  g_input.pending_dx = g_input.pending_dy = 0.0;
  g_input.pending_scroll_y = 0.0;

  // Like the buttons, the wheel presses nothing while another window has focus.
  g_input.frame_wheel = 0;
  if (!focused) {
    g_input.wheel_queue_count = 0;
    g_input.wheel_partial = 0.0;
  } else if (g_input.wheel_queue_count > 0) {
    g_input.frame_wheel = g_input.wheel_queue[g_input.wheel_queue_start];
    g_input.wheel_queue_start = (g_input.wheel_queue_start + 1) % WHEEL_QUEUE_SIZE;
    g_input.wheel_queue_count--;
  }

  glfwGetCursorPos(g_input.window, &g_input.cursor_x, &g_input.cursor_y);
}

bool input_key_down(int glfw_key) {
  if (glfw_key < 0 || glfw_key > GLFW_KEY_LAST) return false;
  return g_input.keys[glfw_key] != 0;
}

bool input_key_pressed(int glfw_key, bool repeat) {
  if (glfw_key < 0 || glfw_key > GLFW_KEY_LAST) return false;
  if (g_input.keys[glfw_key] && !g_input.keys_prev[glfw_key]) return true;
  // A repeat that arrived just before the key was released must not count.
  return repeat && g_input.keys_repeat[glfw_key] && g_input.keys[glfw_key];
}

bool input_mouse_down(int glfw_button) {
  if (glfw_button < 0 || glfw_button > GLFW_MOUSE_BUTTON_LAST) return false;
  return g_input.buttons[glfw_button] != 0;
}

bool input_mouse_pressed(int glfw_button, bool repeat) {
  if (glfw_button < 0 || glfw_button > GLFW_MOUSE_BUTTON_LAST) return false;
  return g_input.buttons[glfw_button] && (!g_input.buttons_prev[glfw_button] || (repeat && g_input.buttons_repeat[glfw_button]));
}

bool input_ctrl_down(void) { return input_key_down(GLFW_KEY_LEFT_CONTROL) || input_key_down(GLFW_KEY_RIGHT_CONTROL); }
bool input_alt_down(void) { return input_key_down(GLFW_KEY_LEFT_ALT) || input_key_down(GLFW_KEY_RIGHT_ALT); }
bool input_shift_down(void) { return input_key_down(GLFW_KEY_LEFT_SHIFT) || input_key_down(GLFW_KEY_RIGHT_SHIFT); }
bool input_super_down(void) { return input_key_down(GLFW_KEY_LEFT_SUPER) || input_key_down(GLFW_KEY_RIGHT_SUPER); }

void input_mouse_delta(double *out_dx, double *out_dy) {
  if (out_dx) *out_dx = g_input.frame_dx;
  if (out_dy) *out_dy = g_input.frame_dy;
}

double input_scroll_y(void) { return g_input.frame_scroll_y; }

int input_wheel_notch(void) { return g_input.frame_wheel; }

void input_cursor_pos(double *out_x, double *out_y) {
  if (out_x) *out_x = g_input.cursor_x;
  if (out_y) *out_y = g_input.cursor_y;
}

int input_glfw_key_from_imgui(ImGuiKey key) {
  if (key <= ImGuiKey_None || key >= ImGuiKey_NamedKey_END) return -1;
  return g_input.imgui_to_glfw[key];
}

ImGuiKey input_capture_pressed_key(void) {
  // A modifier is normally part of a combo rather than its key, so it only counts on its own once
  // it is let go: binding Ctrl+S or Ctrl+WheelUp starts with holding Ctrl, which must not bind Ctrl
  // right there. On its own it is still a perfectly good bind, a freecam wants Shift to sprint.
  ImGuiKey released_modifier = ImGuiKey_None;

  for (int i = 0; i < g_input.poll_key_count; ++i) {
    int keycode = g_input.poll_keys[i];
    ImGuiKey key = glfw_key_to_imgui(keycode);
    switch (key) {
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
    case ImGuiKey_LeftSuper:
    case ImGuiKey_RightSuper:
      if (released_modifier == ImGuiKey_None && g_input.keys_prev[keycode] && !g_input.keys[keycode]) released_modifier = key;
      continue;
    default:
      if (input_key_pressed(keycode, false)) return key;
    }
  }

  if (input_mouse_pressed(GLFW_MOUSE_BUTTON_LEFT, false)) return ImGuiKey_MouseLeft;
  if (input_mouse_pressed(GLFW_MOUSE_BUTTON_RIGHT, false)) return ImGuiKey_MouseRight;
  if (input_mouse_pressed(GLFW_MOUSE_BUTTON_MIDDLE, false)) return ImGuiKey_MouseMiddle;
  if (input_mouse_pressed(GLFW_MOUSE_BUTTON_4, false)) return ImGuiKey_MouseX1;
  if (input_mouse_pressed(GLFW_MOUSE_BUTTON_5, false)) return ImGuiKey_MouseX2;
  if (g_input.frame_wheel) return g_input.frame_wheel > 0 ? INPUT_KEY_WHEEL_UP : INPUT_KEY_WHEEL_DOWN;

  return released_modifier;
}

int input_glfw_button_from_imgui(ImGuiKey key) {
  switch (key) {
  case ImGuiKey_MouseLeft:
    return GLFW_MOUSE_BUTTON_LEFT;
  case ImGuiKey_MouseRight:
    return GLFW_MOUSE_BUTTON_RIGHT;
  case ImGuiKey_MouseMiddle:
    return GLFW_MOUSE_BUTTON_MIDDLE;
  case ImGuiKey_MouseX1:
    return GLFW_MOUSE_BUTTON_4;
  case ImGuiKey_MouseX2:
    return GLFW_MOUSE_BUTTON_5;
  default:
    return -1;
  }
}
