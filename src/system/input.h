#ifndef INPUT_H
#define INPUT_H

/* TODO: Improve this, it's a bit of a mess, and might become more fragile in the future as the program expands
 * How inputs in FrameTee work:
 *
 * The GLFW callbacks in graphics_backend.c are the only ones installed; the imgui backend is started
 * with install_callbacks=false. Every event goes to this module first and is then passed on to
 * ImGui_ImplGlfw_*Callback, so there is one path from the window to both consumers.
 *
 * Key and button state is polled from GLFW once per frame, mouse motion and scroll are summed by the
 * callbacks as they arrive and drained in input_new_frame(). Binds and viewport pan/zoom read from
 * here, so nothing they do can fall behind the hardware. The one queue is a few wheel notches deep:
 * a bind on the wheel is pressed once per notch, and a spin can bring several notches in one frame.
 *
 * Imgui does keep a queue, and trickles it: events of the same kind arriving in one frame are spread
 * over several, so that e.g. a click that is pressed and released inside one frame is not flattened
 * into nothing. It gets past at most one blocking event per frame though, and a mouse reporting at
 * 8000Hz can queue events faster than that. Two things keep it level:
 *
 *   - Mouse motion is held in graphics_backend.c and given to imgui once per frame, plus once ahead
 *     of any event that is ordered against the cursor (a click, a wheel, the cursor leaving).
 *   - imgui_queue_needs_trickling() reads the queue before each igNewFrame() and turns trickling off
 *     for frames where flattening would lose nothing, which empties the queue in one frame.
 */

#include <stdbool.h>
#include <system/include_cimgui.h>

struct GLFWwindow;

void input_init(struct GLFWwindow *window);
void input_shutdown(void);
// Samples the keyboard and mouse for this frame. Must run after glfwPollEvents().
void input_new_frame(void);
// While blocked (a file dialog is up), from the next input_new_frame() on, every key, button, wheel
// notch and motion reads as idle, as if another window had focus.
void input_set_blocked(bool blocked);

// Fed from the GLFW callbacks, which see every event even between frames.
void input_accumulate_mouse_pos(double x, double y, double *out_dx, double *out_dy);
void input_accumulate_scroll(double x, double y);
// Called for GLFW_REPEAT, so held binds repeat at the delay and rate configured in the OS.
void input_accumulate_key_repeat(int glfw_key);

// GLFW keycodes / GLFW mouse buttons.
bool input_key_down(int glfw_key);
bool input_key_pressed(int glfw_key, bool repeat);
bool input_mouse_down(int glfw_button);
// Mouse repeat is synthesized using system keyboard repeat settings, only
// for callers that opt in. Rebinding and single-press actions pass false.
bool input_mouse_pressed(int glfw_button, bool repeat);

bool input_ctrl_down(void);
bool input_alt_down(void);
bool input_shift_down(void);
bool input_super_down(void);

// Motion summed over every event since the previous frame, in pixels.
void input_mouse_delta(double *out_dx, double *out_dy);
double input_scroll_y(void);
void input_cursor_pos(double *out_x, double *out_y);
// The wheel notch this frame hands to binds: 1 up, -1 down, 0 none. At most one per frame; the
// rest of a quick spin follows on the next frames.
int input_wheel_notch(void);

// Keybinds are stored as ImGuiKey so that saved configs and the rebinding UI keep working.
// Returns -1 when the key has no GLFW equivalent.
int input_glfw_key_from_imgui(ImGuiKey key);
// Returns -1 when the key is not one of ImGuiKey_MouseLeft..ImGuiKey_MouseX2.
int input_glfw_button_from_imgui(ImGuiKey key);

// Imgui only knows the wheel as an axis (ImGuiKey_MouseWheelY), so a bind stores each direction
// as a key past imgui's named keys. These must never reach an imgui function such as GetKeyName.
#define INPUT_KEY_WHEEL_UP ((ImGuiKey)ImGuiKey_NamedKey_END)
#define INPUT_KEY_WHEEL_DOWN ((ImGuiKey)(ImGuiKey_NamedKey_END + 1))

// For the rebinding UI: the first key, mouse button or wheel notch pressed this frame, as the
// ImGuiKey the binding is stored as. A modifier pressed with it is recorded separately in the
// combo; a modifier on its own is returned when it is released.
ImGuiKey input_capture_pressed_key(void);

#endif // INPUT_H
