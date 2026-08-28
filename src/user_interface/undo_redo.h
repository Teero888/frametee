#ifndef UNDO_REDO_H
#define UNDO_REDO_H

#include <stdbool.h>
#include <types.h>

struct undo_command_t {
  char description[64];
  // A function to reverse the action.
  void (*undo)(void *cmd, void *ts);
  // A function to re-apply the action.
  void (*redo)(void *cmd, void *ts);
  // A function to free any memory held by the command itself.
  void (*cleanup)(void *cmd);
};

// The manager holds separate stacks for undo and redo commands.
//
// Each stack carries a second array naming who registered each command. A
// command is three function pointers, and a plugin's point into a library the
// user can unload, so the manager has to be able to find and drop those before
// the code behind them goes away. It is kept beside the commands rather than
// inside them because a plugin allocates the command struct itself.
struct undo_manager_t {
  undo_command_t **undo_stack;
  const void **undo_owners;
  undo_command_t **redo_stack;
  const void **redo_owners;
  int undo_count;
  int redo_count;
  int undo_capacity;
  int redo_capacity;
  bool show_history_window;
};

// Public API

void undo_manager_init(undo_manager_t *manager);
void undo_manager_cleanup(undo_manager_t *manager);

// Call this AFTER an action is performed to register its corresponding undo command.
void undo_manager_register_command(undo_manager_t *manager, undo_command_t *command);

// Same, for a command whose code belongs to something that can go away. The
// owner is an opaque tag, matched by pointer value.
void undo_manager_register_command_owned(undo_manager_t *manager, undo_command_t *command, const void *owner);

// Drops every command registered under `owner` from both stacks, cleaning each
// one up while its code is still there to do it. Returns how many were dropped.
// The history it leaves is shorter, which is the honest outcome: nothing can
// reverse an action whose code has been unloaded.
int undo_manager_purge_owner(undo_manager_t *manager, const void *owner);

// Perform undo/redo operations.
void undo_manager_undo(undo_manager_t *manager, void *ts);
void undo_manager_redo(undo_manager_t *manager, void *ts);

// Check if undo/redo is possible.
bool undo_manager_can_undo(const undo_manager_t *manager);
bool undo_manager_can_redo(const undo_manager_t *manager);

void undo_manager_render_history_window(undo_manager_t *manager);

#endif // UNDO_REDO_H
