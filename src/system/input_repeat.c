#include "input_repeat.h"
#include <GLFW/glfw3.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <X11/XKBlib.h>
#endif

#ifdef FT_HAS_WAYLAND
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// A separate connection observes settings without consuming GLFW's events or
// replacing listeners on its keyboard. No surfaces or input grabs are created.
static struct {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct wl_keyboard *keyboard;
  uint32_t seat_name;
} g_wayland;
#endif

static input_repeat_settings_t g_settings;

#ifdef FT_HAS_WAYLAND
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size) {
  (void)data;
  (void)keyboard;
  (void)format;
  (void)size;
  close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)surface;
  (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)time;
  (void)key;
  (void)state;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)depressed;
  (void)latched;
  (void)locked;
  (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
  (void)data;
  (void)keyboard;
  g_settings = (input_repeat_settings_t){.enabled = rate > 0 && delay >= 0,
                                         .delay = delay >= 0 ? delay / 1000.0 : 0.0,
                                         .interval = rate > 0 ? 1.0 / rate : 0.0};
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave, .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat_info};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
  (void)data;
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wayland.keyboard) {
    g_wayland.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(g_wayland.keyboard, &keyboard_listener, NULL);
  } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && g_wayland.keyboard) {
    wl_keyboard_release(g_wayland.keyboard);
    g_wayland.keyboard = NULL;
    g_settings = (input_repeat_settings_t){0};
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data;
  (void)seat;
  (void)name;
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_capabilities, .name = seat_name};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void)data;
  // repeat_info was introduced in version 4. Bind only that version so all
  // keyboard events have listeners even with newer protocol headers/servers.
  if (!g_wayland.seat && version >= 4 && strcmp(interface, "wl_seat") == 0) {
    g_wayland.seat_name = name;
    g_wayland.seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
    wl_seat_add_listener(g_wayland.seat, &seat_listener, NULL);
  }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
  (void)data;
  (void)registry;
  if (!g_wayland.seat || name != g_wayland.seat_name) return;
  if (g_wayland.keyboard) wl_keyboard_release(g_wayland.keyboard);
  // wl_seat.release requires version 5; our seat is version 4.
  wl_seat_destroy(g_wayland.seat);
  g_wayland.keyboard = NULL;
  g_wayland.seat = NULL;
  g_settings = (input_repeat_settings_t){0};
}

static const struct wl_registry_listener registry_listener = {.global = registry_global, .global_remove = registry_remove};

static void wayland_update(void) {
  if (!g_wayland.display) return;
  while (wl_display_prepare_read(g_wayland.display) != 0) {
    if (wl_display_dispatch_pending(g_wayland.display) < 0) goto failed;
  }
  if (wl_display_flush(g_wayland.display) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(g_wayland.display);
    goto failed;
  }
  struct pollfd fd = {.fd = wl_display_get_fd(g_wayland.display), .events = POLLIN};
  int ready = poll(&fd, 1, 0);
  if (ready > 0 && (fd.revents & POLLIN)) {
    if (wl_display_read_events(g_wayland.display) < 0) goto failed;
  } else {
    wl_display_cancel_read(g_wayland.display);
    if ((ready < 0 && errno != EINTR) || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) goto failed;
  }
  if (wl_display_dispatch_pending(g_wayland.display) < 0) goto failed;
  return;
failed:
  input_repeat_shutdown();
}
#endif

void input_repeat_shutdown(void) {
#ifdef FT_HAS_WAYLAND
  if (g_wayland.keyboard) wl_keyboard_destroy(g_wayland.keyboard);
  if (g_wayland.seat) wl_seat_destroy(g_wayland.seat);
  if (g_wayland.registry) wl_registry_destroy(g_wayland.registry);
  if (g_wayland.display) wl_display_disconnect(g_wayland.display);
  memset(&g_wayland, 0, sizeof(g_wayland));
#endif
  g_settings = (input_repeat_settings_t){0};
}

void input_repeat_refresh(void) {
#ifdef _WIN32
  UINT delay, speed;
  g_settings = (input_repeat_settings_t){0};
  if (SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0) &&
      SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0)) {
    g_settings.enabled = true;
    g_settings.delay = (delay + 1) * 0.25;
    // Windows reports a 0..31 setting, approximately 2.5..30 repeats/sec.
    g_settings.interval = 1.0 / (2.5 + speed * (27.5 / 31.0));
  }
#elif defined(__linux__)
  if (glfwGetPlatform() != GLFW_PLATFORM_X11) return;
  g_settings = (input_repeat_settings_t){0};
  Display *display = XOpenDisplay(NULL);
  if (!display) return;
  unsigned int delay, interval;
  XKeyboardState keyboard;
  if (XkbGetAutoRepeatRate(display, XkbUseCoreKbd, &delay, &interval)) {
    XGetKeyboardControl(display, &keyboard);
    g_settings.enabled = keyboard.global_auto_repeat == AutoRepeatModeOn && interval > 0;
    g_settings.delay = delay / 1000.0;
    g_settings.interval = interval / 1000.0;
  }
  XCloseDisplay(display);
#endif
}

void input_repeat_init(void) {
  input_repeat_shutdown();
#ifdef FT_HAS_WAYLAND
  if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
    g_wayland.display = wl_display_connect(NULL);
    if (g_wayland.display) {
      g_wayland.registry = wl_display_get_registry(g_wayland.display);
      wl_registry_add_listener(g_wayland.registry, &registry_listener, NULL);
    }
  }
#endif
  input_repeat_refresh();
}

input_repeat_settings_t input_repeat_settings(void) {
#ifdef FT_HAS_WAYLAND
  wayland_update();
#endif
  return g_settings;
}
