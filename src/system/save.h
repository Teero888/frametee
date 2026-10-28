#ifndef SAVE_H
#define SAVE_H

#include <types.h>
#include <user_interface/user_interface.h>

#define TAS_PROJECT_FILE_MAGIC "TASP"
#define TAS_PROJECT_FILE_VERSION 32

bool save_project(ui_handler_t *ui, const char *path);
// Writes the current project without changing its save path or dirty state.
bool save_project_snapshot(ui_handler_t *ui, const char *path);
bool load_project(ui_handler_t *ui, const char *path);
// Moves the open project to the game module `game_index`, a backend of the active game's
// (ft_game_info::backend_of), and that game loads the level. Everything goes along unchanged: the
// timeline with its recordings, its track uids and snippet ids, the starting overrides (of properties
// that game lacks too), the camera, the video settings, the project data, the selection and the undo
// history, so moving it back gives the project it was. The starting worlds the old game stored are
// made again from the tracks' overrides, which is all they hold. Project data or a ruleset the other
// game rejects is left out, with a warning each (logger_warnings). A project it has no room for
// (recordings it cannot play, more players in a group than its worlds hold) stays. Changes the
// active game: call it between frames. `error` gets why it did not move, for the user; the project,
// its history included, is then as it was.
bool switch_project_backend(ui_handler_t *ui, int game_index, char *error, size_t error_size);
bool import_project_as_group(ui_handler_t *ui, const char *path);

#endif // SAVE_H
