#include "file_dialog.h"
#include <logger/logger.h>
#include <nfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/compat_threads.h>

#define LOG_SOURCE "FileDialog"

typedef enum { DIALOG_IDLE, DIALOG_REQUESTED, DIALOG_OPEN, DIALOG_DONE } dialog_state_t;

// One thread owns every dialog for the whole run. Each toolkit wants its dialogs on the thread that
// set it up: COM's apartment on Windows, GTK, the portal's D-Bus connection.
static struct {
  bool started;
  bool quit;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t wake;

  // Guarded by the mutex. The request's strings belong to the main thread, which only replaces them
  // while the state is DIALOG_IDLE.
  dialog_state_t state;
  bool save;
  char *filter_name, *filter_ext, *default_dir, *default_name;
  char *result; // NULL when cancelled or failed
  char error[256];
  file_dialog_done_fn done;
  void *user;
} g_dialog;

static char *copy_string(const char *text) {
  if (!text) return NULL;
  const size_t size = strlen(text) + 1;
  char *copy = malloc(size);
  if (copy) memcpy(copy, text, size);
  return copy;
}

static void free_request(void) {
  free(g_dialog.filter_name);
  free(g_dialog.filter_ext);
  free(g_dialog.default_dir);
  free(g_dialog.default_name);
  g_dialog.filter_name = g_dialog.filter_ext = g_dialog.default_dir = g_dialog.default_name = NULL;
}

// Runs one dialog on the calling thread. Writes NFD's error, if any, into `error`.
static char *run_dialog(bool save, const char *filter_name, const char *filter_ext, const char *default_dir,
                        const char *default_name, char *error, size_t error_size) {
  const nfdu8filteritem_t filter = {filter_name, filter_ext};
  const bool has_filter = filter_name && filter_ext;
  nfdu8char_t *path = NULL;
  nfdresult_t result;
  if (save) {
    nfdsavedialogu8args_t args = {0};
    args.filterList = has_filter ? &filter : NULL;
    args.filterCount = has_filter ? 1 : 0;
    args.defaultPath = default_dir;
    args.defaultName = default_name;
    result = NFD_SaveDialogU8_With(&path, &args);
  } else {
    nfdopendialogu8args_t args = {0};
    args.filterList = has_filter ? &filter : NULL;
    args.filterCount = has_filter ? 1 : 0;
    args.defaultPath = default_dir;
    result = NFD_OpenDialogU8_With(&path, &args);
  }
  if (result == NFD_ERROR) snprintf(error, error_size, "%s", NFD_GetError() ? NFD_GetError() : "unknown error");
  char *chosen = result == NFD_OKAY && path ? copy_string(path) : NULL;
  if (path) NFD_FreePathU8(path);
  return chosen;
}

static void *dialog_thread(void *arg) {
  (void)arg;
  char init_error[256] = "";
  const bool ready = NFD_Init() == NFD_OKAY;
  if (!ready) snprintf(init_error, sizeof(init_error), "%s", NFD_GetError() ? NFD_GetError() : "unknown error");

  pthread_mutex_lock(&g_dialog.mutex);
  for (;;) {
    while (!g_dialog.quit && g_dialog.state != DIALOG_REQUESTED)
      pthread_cond_wait(&g_dialog.wake, &g_dialog.mutex);
    if (g_dialog.quit) break;
    g_dialog.state = DIALOG_OPEN;
    const bool save = g_dialog.save;
    const char *filter_name = g_dialog.filter_name, *filter_ext = g_dialog.filter_ext;
    const char *default_dir = g_dialog.default_dir, *default_name = g_dialog.default_name;
    pthread_mutex_unlock(&g_dialog.mutex);

    char error[256] = "";
    char *chosen = NULL;
    if (ready) chosen = run_dialog(save, filter_name, filter_ext, default_dir, default_name, error, sizeof(error));
    else snprintf(error, sizeof(error), "file dialogs are unavailable: %s", init_error);

    pthread_mutex_lock(&g_dialog.mutex);
    g_dialog.result = chosen;
    snprintf(g_dialog.error, sizeof(g_dialog.error), "%s", error);
    g_dialog.state = DIALOG_DONE;
  }
  pthread_mutex_unlock(&g_dialog.mutex);
  if (ready) NFD_Quit();
  return NULL;
}

static bool start_thread(void) {
  if (g_dialog.started) return true;
  pthread_mutex_init(&g_dialog.mutex, NULL);
  pthread_cond_init(&g_dialog.wake, NULL);
  g_dialog.quit = false;
  g_dialog.state = DIALOG_IDLE;
  if (pthread_create(&g_dialog.thread, NULL, dialog_thread, NULL) != 0) {
    log_error(LOG_SOURCE, "Could not start the file dialog thread.");
    pthread_cond_destroy(&g_dialog.wake);
    pthread_mutex_destroy(&g_dialog.mutex);
    return false;
  }
  g_dialog.started = true;
  return true;
}

bool file_dialog_open(const file_dialog_t *dialog, file_dialog_done_fn done, void *user) {
  extern bool g_is_headless;
  if (g_is_headless || !dialog) return false;
#ifdef __APPLE__
  // Cocoa only shows dialogs from the main thread, so here the dialog still holds up the frame.
  static bool ready = false;
  if (!ready) ready = NFD_Init() == NFD_OKAY;
  if (!ready || g_dialog.state != DIALOG_IDLE) return false;
  g_dialog.result = run_dialog(dialog->save, dialog->filter_name, dialog->filter_ext, dialog->default_dir,
                               dialog->default_name, g_dialog.error, sizeof(g_dialog.error));
  g_dialog.done = done;
  g_dialog.user = user;
  g_dialog.state = DIALOG_DONE;
  return true;
#else
  if (!start_thread()) return false;
  pthread_mutex_lock(&g_dialog.mutex);
  if (g_dialog.state != DIALOG_IDLE) {
    pthread_mutex_unlock(&g_dialog.mutex);
    return false;
  }
  free_request();
  g_dialog.save = dialog->save;
  g_dialog.filter_name = copy_string(dialog->filter_name);
  g_dialog.filter_ext = copy_string(dialog->filter_ext);
  g_dialog.default_dir = copy_string(dialog->default_dir);
  g_dialog.default_name = copy_string(dialog->default_name);
  g_dialog.done = done;
  g_dialog.user = user;
  g_dialog.state = DIALOG_REQUESTED;
  pthread_cond_broadcast(&g_dialog.wake);
  pthread_mutex_unlock(&g_dialog.mutex);
  return true;
#endif
}

// Takes the lock; on macOS everything stays on the main thread and there is none. False when no
// dialog has ever been opened.
static bool lock(void) {
#ifdef __APPLE__
  return true;
#else
  if (!g_dialog.started) return false;
  pthread_mutex_lock(&g_dialog.mutex);
  return true;
#endif
}

static void unlock(void) {
#ifndef __APPLE__
  pthread_mutex_unlock(&g_dialog.mutex);
#endif
}

void file_dialog_update(void) {
  if (!lock()) return;
  if (g_dialog.state != DIALOG_DONE) {
    unlock();
    return;
  }
  char *result = g_dialog.result;
  char error[sizeof(g_dialog.error)];
  snprintf(error, sizeof(error), "%s", g_dialog.error);
  const file_dialog_done_fn done = g_dialog.done;
  void *user = g_dialog.user;
  g_dialog.result = NULL;
  g_dialog.error[0] = '\0';
  g_dialog.done = NULL;
  g_dialog.user = NULL;
  g_dialog.state = DIALOG_IDLE;
  unlock();

  // Some backends report a dialog cancelled from the keyboard as an error, hence only a warning.
  if (error[0]) log_warn(LOG_SOURCE, "File dialog: %s", error);
  if (done) done(user, result);
  free(result);
}

bool file_dialog_active(void) {
  if (!lock()) return false;
  const bool active = g_dialog.state != DIALOG_IDLE;
  unlock();
  return active;
}

void file_dialog_forget(void *user) {
  if (!lock()) return;
  if (g_dialog.state != DIALOG_IDLE && g_dialog.user == user) g_dialog.done = NULL;
  unlock();
}

void file_dialog_shutdown(void) {
#ifdef __APPLE__
  free(g_dialog.result);
  g_dialog.result = NULL;
  g_dialog.state = DIALOG_IDLE;
#else
  if (!g_dialog.started) return;
  pthread_mutex_lock(&g_dialog.mutex);
  const bool showing = g_dialog.state == DIALOG_OPEN;
  g_dialog.quit = true;
  pthread_cond_broadcast(&g_dialog.wake);
  pthread_mutex_unlock(&g_dialog.mutex);
  // A dialog still up holds the thread inside the toolkit; it goes when the process does, and so
  // does everything it might still touch.
  if (showing) {
    pthread_detach(g_dialog.thread);
    return;
  }
  pthread_join(g_dialog.thread, NULL);
  pthread_cond_destroy(&g_dialog.wake);
  pthread_mutex_destroy(&g_dialog.mutex);
  free_request();
  free(g_dialog.result);
  memset(&g_dialog, 0, sizeof(g_dialog));
#endif
}
