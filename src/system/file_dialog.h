#ifndef FILE_DIALOG_H
#define FILE_DIALOG_H

#include <stdbool.h>

// Native file dialogs, run on a thread of their own so the editor keeps drawing while one is up.
// The editor ignores its own input meanwhile (file_dialog_active). One dialog at a time.

// Called from file_dialog_update() on the main thread: the chosen path, or NULL when the dialog
// was cancelled or failed. The path is only valid during the call.
typedef void (*file_dialog_done_fn)(void *user, const char *path);

typedef struct {
  bool save;                // a save dialog rather than an open one
  const char *filter_name;  // one file type, e.g. "TAS Project"; NULL for any file
  const char *filter_ext;   // "tasp", or a list such as "z64,n64"
  const char *default_dir;  // where the dialog starts; NULL for the toolkit's choice
  const char *default_name; // the file name a save dialog suggests
} file_dialog_t;

// Opens a dialog; the strings are copied. False when one is already up or the run is headless, and
// `done` is then never called.
bool file_dialog_open(const file_dialog_t *dialog, file_dialog_done_fn done, void *user);
// Hands a finished dialog's answer to its `done`. Once per frame, on the main thread.
void file_dialog_update(void);
bool file_dialog_active(void);
// The dialog opened for `user`, if one is up, delivers its answer nowhere: what asked for it is going away.
void file_dialog_forget(void *user);
void file_dialog_shutdown(void);

#endif // FILE_DIALOG_H
