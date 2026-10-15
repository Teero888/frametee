#include "raw_mouse.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The way SDL 3 reads raw input: a thread with a message-only window that the mouse's raw input is
// sent to, which sleeps until there is some and then takes all that has come with GetRawInputBuffer.
// Raw input goes to one window per process, so GLFW's raw motion stays off while this runs (see
// input_set_mouse_captured), or its window would take the mouse back.

static HANDLE g_thread, g_stop, g_ready;
static volatile LONG g_running;
static volatile LONG64 g_dx, g_dy;

// A pointing device that reports positions rather than motion: a tablet, or the mouse of a remote
// desktop or a virtual machine. Its motion is the difference between two positions, in pixels.
static void add_absolute(const RAWMOUSE *mouse, LONG *last_x, LONG *last_y, bool *has_last) {
  const bool virtual_desktop = (mouse->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
  const int left = virtual_desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
  const int top = virtual_desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
  const int width = GetSystemMetrics(virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
  const int height = GetSystemMetrics(virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
  const LONG x = (LONG)((mouse->lLastX / 65535.0) * width) + left;
  const LONG y = (LONG)((mouse->lLastY / 65535.0) * height) + top;
  if (*has_last) {
    InterlockedExchangeAdd64(&g_dx, x - *last_x);
    InterlockedExchangeAdd64(&g_dy, y - *last_y);
  }
  *last_x = x;
  *last_y = y;
  *has_last = true;
}

// NEXTRAWINPUTBLOCK, which MinGW's headers cannot expand (they have no QWORD): blocks start at
// pointer-size boundaries.
static RAWINPUT *next_block(RAWINPUT *input) {
  const ULONG_PTR align = sizeof(void *);
  return (RAWINPUT *)(((ULONG_PTR)input + input->header.dwSize + align - 1) & ~(align - 1));
}

// Everything that has come in since the last time.
static void drain(LONG *last_x, LONG *last_y, bool *has_last) {
  // (RAWINPUT, so the blocks are aligned as GetRawInputBuffer wants them; a few hundred reports)
  RAWINPUT buffer[256];
  for (;;) {
    UINT size = sizeof(buffer);
    const UINT count = GetRawInputBuffer(buffer, &size, sizeof(RAWINPUTHEADER));
    if (count == 0 || count == (UINT)-1) return;
    RAWINPUT *input = buffer;
    for (UINT i = 0; i < count; ++i, input = next_block(input)) {
      if (input->header.dwType != RIM_TYPEMOUSE) continue;
      const RAWMOUSE *mouse = &input->data.mouse;
      if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE) add_absolute(mouse, last_x, last_y, has_last);
      else if (mouse->lLastX || mouse->lLastY) {
        InterlockedExchangeAdd64(&g_dx, mouse->lLastX);
        InterlockedExchangeAdd64(&g_dy, mouse->lLastY);
      }
    }
  }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

static DWORD WINAPI reader(LPVOID unused) {
  (void)unused;
  const HINSTANCE instance = GetModuleHandleW(NULL);
  WNDCLASSEXW window_class = {.cbSize = sizeof(window_class),
                              .lpfnWndProc = window_proc,
                              .hInstance = instance,
                              .lpszClassName = L"FrameTeeRawMouse"};
  RegisterClassExW(&window_class); // (fails harmlessly when a previous reader registered it)
  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, instance, NULL);
  // INPUTSINK: a message-only window is never in the foreground, and without it raw input only goes to
  // windows of the foreground. The motion is then the mouse's whichever window has it, see
  // raw_mouse_take.
  RAWINPUTDEVICE device = {.usUsagePage = 0x01 /* generic desktop */, .usUsage = 0x02 /* mouse */,
                           .dwFlags = RIDEV_INPUTSINK, .hwndTarget = window};
  const bool registered = window && RegisterRawInputDevices(&device, 1, sizeof(device));
  InterlockedExchange(&g_running, registered);
  SetEvent(g_ready);
  if (!registered) {
    if (window) DestroyWindow(window);
    return 1;
  }
  // Read promptly, as SDL does: the mouse reports far more often than this waits.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  LONG last_x = 0, last_y = 0;
  bool has_last = false;
  for (;;) {
    const DWORD woken = MsgWaitForMultipleObjects(1, &g_stop, FALSE, INFINITE, QS_RAWINPUT);
    if (woken != WAIT_OBJECT_0 + 1) break;
    // Cleared before reading, so that what arrives during the read wakes the next wait.
    GetQueueStatus(QS_RAWINPUT);
    drain(&last_x, &last_y, &has_last);
  }

  device.dwFlags = RIDEV_REMOVE;
  device.hwndTarget = NULL;
  RegisterRawInputDevices(&device, 1, sizeof(device));
  DestroyWindow(window);
  return 0;
}

bool raw_mouse_start(void) {
  if (g_thread) return g_running != 0;
  g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  InterlockedExchange64(&g_dx, 0);
  InterlockedExchange64(&g_dy, 0);
  if (g_stop && g_ready) g_thread = CreateThread(NULL, 0, reader, NULL, 0, NULL);
  if (g_thread) WaitForSingleObject(g_ready, INFINITE);
  if (!g_thread || !g_running) {
    raw_mouse_stop();
    return false;
  }
  return true;
}

void raw_mouse_stop(void) {
  if (g_thread) {
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
  }
  if (g_stop) CloseHandle(g_stop);
  if (g_ready) CloseHandle(g_ready);
  g_thread = g_stop = g_ready = NULL;
  InterlockedExchange(&g_running, 0);
}

bool raw_mouse_running(void) { return g_running != 0; }

void raw_mouse_take(double *out_dx, double *out_dy) {
  *out_dx = (double)InterlockedExchange64(&g_dx, 0);
  *out_dy = (double)InterlockedExchange64(&g_dy, 0);
}

#else

bool raw_mouse_start(void) { return false; }
void raw_mouse_stop(void) {}
bool raw_mouse_running(void) { return false; }
void raw_mouse_take(double *out_dx, double *out_dy) { *out_dx = *out_dy = 0.0; }

#endif
