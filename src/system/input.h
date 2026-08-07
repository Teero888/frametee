#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <system/include_cimgui.h>

struct GLFWwindow;

// Input read straight from GLFW rather than through imgui.
//
// Imgui trickles its input queue: once a key, mouse button or wheel event has been applied in a
// frame, every mouse position event queued behind it is held back until the next frame. With a high
// polling rate mouse there are hundreds of position events per frame, so a single key or wheel event
// pushes the cursor several frames behind the hardware, which is what made viewport panning feel
// delayed and buffered while zooming or holding a bind.
//
// Polling GLFW gives the current state with no queue in between, and mouse motion is summed by the
// GLFW callback as it arrives, so no movement is lost no matter how fast the mouse reports.

void input_init(struct GLFWwindow *window);
// Samples the keyboard and mouse for this frame. Must run after glfwPollEvents().
void input_new_frame(void);

// Fed from the GLFW callbacks, which see every event even between frames.
void input_accumulate_mouse_delta(double dx, double dy);
void input_accumulate_scroll(double x, double y);
// Called for GLFW_REPEAT, so held binds repeat at the delay and rate configured in the OS.
void input_accumulate_key_repeat(int glfw_key);

// GLFW keycodes / GLFW mouse buttons.
bool input_key_down(int glfw_key);
bool input_key_pressed(int glfw_key, bool repeat);
bool input_mouse_down(int glfw_button);
bool input_mouse_pressed(int glfw_button);

bool input_ctrl_down(void);
bool input_alt_down(void);
bool input_shift_down(void);
bool input_super_down(void);

// Motion summed over every event since the previous frame, in pixels.
void input_mouse_delta(double *out_dx, double *out_dy);
double input_scroll_y(void);
void input_cursor_pos(double *out_x, double *out_y);

// Keybinds are stored as ImGuiKey so that saved configs and the rebinding UI keep working.
// Returns -1 when the key has no GLFW equivalent.
int input_glfw_key_from_imgui(ImGuiKey key);
// Returns -1 when the key is not one of ImGuiKey_MouseLeft..ImGuiKey_MouseX2.
int input_glfw_button_from_imgui(ImGuiKey key);

// For the rebinding UI: the first key or mouse button pressed this frame, as the ImGuiKey the
// binding is stored as. Modifiers are skipped since they are recorded separately in the combo.
ImGuiKey input_capture_pressed_key(void);

#endif // INPUT_H
