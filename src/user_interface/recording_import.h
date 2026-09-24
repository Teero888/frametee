#ifndef RECORDING_IMPORT_H
#define RECORDING_IMPORT_H

#include <stdbool.h>
#include <types.h>

// Importing a recording (a DDNet demo) as a new timeline group: one track per chosen player, each
// replaying that player with a playback snippet. The recording opens in the background while a
// dialog shows its progress, then lists its players.

// Whether the active game can open recordings at all. Importing into the open project also needs a level.
bool recording_import_available(ui_handler_t *ui);
// Asks for a recording file and starts importing it. `new_project` starts a fresh project on the
// recording's own level instead of adding it to the open one.
void recording_import_begin(ui_handler_t *ui, bool new_project);
// The same for a file already chosen, e.g. from the start screen.
void recording_import_open(ui_handler_t *ui, const char *path, bool new_project);
// The import dialog, once per frame.
void recording_import_render(ui_handler_t *ui);
// Whether an import dialog is up. The start screen stays away meanwhile, even with no level loaded:
// a demo opened from it brings its own level only once it is imported.
bool recording_import_active(void);

#endif // RECORDING_IMPORT_H
