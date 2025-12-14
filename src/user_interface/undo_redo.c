#include "undo_redo.h"
#include "timeline/timeline_model.h"
#include <system/include_cimgui.h>
#include <stdlib.h>
#include <string.h>

static void clear_stack(undo_command_t ***stack, int *count, int *capacity) {
  if (!*stack) return;
  for (int i = 0; i < *count; ++i) {
    if ((*stack)[i] && (*stack)[i]->cleanup) {
      (*stack)[i]->cleanup((*stack)[i]);
    }
  }
  free(*stack);
  *stack = NULL;
  *count = 0;
  *capacity = 0;
}

static void push_to_stack(undo_command_t ***stack, int *count, int *capacity, undo_command_t *command) {
  if (*count >= *capacity) {
    int new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    undo_command_t **new_stack = realloc(*stack, sizeof(undo_command_t *) * new_capacity);
    if (!new_stack) return; // Allocation failed
    *stack = new_stack;
    *capacity = new_capacity;
  }
  (*stack)[(*count)++] = command;
}

static undo_command_t *pop_from_stack(undo_command_t **stack, int *count) {
  if (*count == 0) return NULL;
  return stack[--(*count)];
}

// Public API Implementation

void undo_manager_init(undo_manager_t *manager) { memset(manager, 0, sizeof(undo_manager_t)); }

void undo_manager_cleanup(undo_manager_t *manager) {
  clear_stack(&manager->undo_stack, &manager->undo_count, &manager->undo_capacity);
  clear_stack(&manager->redo_stack, &manager->redo_count, &manager->redo_capacity);
}

void undo_manager_register_command(undo_manager_t *manager, undo_command_t *command) {
  if (!command) return;
  // Push the new action to the undo stack
  push_to_stack(&manager->undo_stack, &manager->undo_count, &manager->undo_capacity, command);
  // A new action clears the redo history
  clear_stack(&manager->redo_stack, &manager->redo_count, &manager->redo_capacity);
}

bool undo_manager_can_undo(const undo_manager_t *manager) { return manager->undo_count > 0; }

bool undo_manager_can_redo(const undo_manager_t *manager) { return manager->redo_count > 0; }

void undo_manager_undo(undo_manager_t *manager, void *ts) {
  undo_command_t *command = pop_from_stack(manager->undo_stack, &manager->undo_count);
  if (command) {
    command->undo(command, ts);
    push_to_stack(&manager->redo_stack, &manager->redo_count, &manager->redo_capacity, command);
    model_recalc_physics((timeline_state_t *)ts, 0); // Recalculate physics to be safe
  }
}

void undo_manager_redo(undo_manager_t *manager, void *ts) {
  undo_command_t *command = pop_from_stack(manager->redo_stack, &manager->redo_count);
  if (command) {
    command->redo(command, ts);
    push_to_stack(&manager->undo_stack, &manager->undo_count, &manager->undo_capacity, command);
    model_recalc_physics((timeline_state_t *)ts, 0); // Recalculate physics to be safe
  }
}

void undo_manager_render_history_window(undo_manager_t *manager) {
  if (!manager->show_history_window) return;

  igSetNextWindowSize((ImVec2){300, 400}, ImGuiCond_FirstUseEver);
  if (igBegin("Undo History", &manager->show_history_window, 0)) {
    if (igButton("Clear History", (ImVec2){0, 0})) {
      clear_stack(&manager->undo_stack, &manager->undo_count, &manager->undo_capacity);
      clear_stack(&manager->redo_stack, &manager->redo_count, &manager->redo_capacity);
    }
    igSeparator();

    igText("Undo Stack:");
    igBeginChild_Str("UndoStack", (ImVec2){0, 150}, true, 0);
    for (int i = manager->undo_count - 1; i >= 0; i--) {
      igText("%d. %s", i + 1, manager->undo_stack[i]->description);
    }
    igEndChild();

    igSeparator();
    igText("Redo Stack:");
    igBeginChild_Str("RedoStack", (ImVec2){0, 150}, true, 0);
    for (int i = manager->redo_count - 1; i >= 0; i--) {
      igText("%d. %s", i + 1, manager->redo_stack[i]->description);
    }
    igEndChild();
  }
  igEnd();
}
