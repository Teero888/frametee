// Run only under an isolated Xvfb server: this changes that server's keyboard
// settings to verify that we read the system delay, interval, and enable state.
#include <GLFW/glfw3.h>
#include <X11/XKBlib.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <system/input_repeat.h>

int glfwGetPlatform(void) { return GLFW_PLATFORM_X11; }

int main(void) {
  Display *display = XOpenDisplay(NULL);
  assert(display);
  assert(XkbSetAutoRepeatRate(display, XkbUseCoreKbd, 750, 80));
  XAutoRepeatOn(display);
  XSync(display, False);
  input_repeat_init();
  input_repeat_settings_t settings = input_repeat_settings();
  assert(settings.enabled);
  assert(fabs(settings.delay - 0.75) < 1e-9);
  assert(fabs(settings.interval - 0.08) < 1e-9);

  assert(XkbSetAutoRepeatRate(display, XkbUseCoreKbd, 250, 40));
  XSync(display, False);
  input_repeat_refresh();
  settings = input_repeat_settings();
  assert(settings.enabled);
  assert(fabs(settings.delay - 0.25) < 1e-9);
  assert(fabs(settings.interval - 0.04) < 1e-9);

  XAutoRepeatOff(display);
  XSync(display, False);
  input_repeat_refresh();
  assert(!input_repeat_settings().enabled);
  input_repeat_shutdown();
  XCloseDisplay(display);
  puts("X11 system repeat settings tests passed");
  return 0;
}
