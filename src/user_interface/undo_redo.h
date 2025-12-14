#ifndef UNDO_REDO_H
#define UNDO_REDO_H

#include <stdbool.h>

typedef struct undo_command_t undo_command_t;
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
typedef struct {
  undo_command_t **undo_stack;
  undo_command_t **redo_stack;
  int undo_count;
  int redo_count;
  int undo_capacity;
  int redo_capacity;
  bool show_history_window;
} undo_manager_t;

// Public API

void undo_manager_init(undo_manager_t *manager);
void undo_manager_cleanup(undo_manager_t *manager);

// Call this AFTER an action is performed to register its corresponding undo command.
void undo_manager_register_command(undo_manager_t *manager, undo_command_t *command);

// Perform undo/redo operations.
void undo_manager_undo(undo_manager_t *manager, void *ts);
void undo_manager_redo(undo_manager_t *manager, void *ts);

// Check if undo/redo is possible.
bool undo_manager_can_undo(const undo_manager_t *manager);
bool undo_manager_can_redo(const undo_manager_t *manager);

void undo_manager_render_history_window(undo_manager_t *manager);

#endif // UNDO_REDO_H
