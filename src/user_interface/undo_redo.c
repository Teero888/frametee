#include "undo_redo.h"
#include "user_interface.h"
#include "timeline/timeline_model.h"
#include <stdlib.h>
#include <string.h>
#include <system/include_cimgui.h>

static void clear_stack(undo_command_t ***stack, const void ***owners, int *count, int *capacity) {
  if (!*stack) return;
  for (int i = 0; i < *count; ++i) {
    if ((*stack)[i] && (*stack)[i]->cleanup) {
      (*stack)[i]->cleanup((*stack)[i]);
    }
  }
  free(*stack);
  free(*owners);
  *stack = NULL;
  *owners = NULL;
  *count = 0;
  *capacity = 0;
}

static void push_to_stack(undo_command_t ***stack, const void ***owners, int *count, int *capacity, undo_command_t *command,
                          const void *owner) {
  if (*count >= *capacity) {
    int new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    undo_command_t **new_stack = realloc(*stack, sizeof(undo_command_t *) * new_capacity);
    if (!new_stack) return; // Allocation failed
    *stack = new_stack;
    // The two arrays are indexed together, so a half-grown pair would lose
    // track of who owns what.
    const void **new_owners = realloc(*owners, sizeof(const void *) * new_capacity);
    if (!new_owners) return;
    *owners = new_owners;
    *capacity = new_capacity;
  }
  (*owners)[*count] = owner;
  (*stack)[(*count)++] = command;
}

static undo_command_t *pop_from_stack(undo_command_t **stack, const void **owners, int *count, const void **out_owner) {
  if (*count == 0) return NULL;
  --(*count);
  if (out_owner) *out_owner = owners[*count];
  return stack[*count];
}

// Public API Implementation

void undo_manager_init(undo_manager_t *manager) { memset(manager, 0, sizeof(undo_manager_t)); }

void undo_manager_cleanup(undo_manager_t *manager) {
  clear_stack(&manager->undo_stack, &manager->undo_owners, &manager->undo_count, &manager->undo_capacity);
  clear_stack(&manager->redo_stack, &manager->redo_owners, &manager->redo_count, &manager->redo_capacity);
}

#include <stddef.h>

static inline void mark_ui_unsaved(undo_manager_t *manager) {
  if (!manager) return;
  ui_handler_t *ui = (ui_handler_t *)((char *)manager - offsetof(ui_handler_t, undo_manager));
  if (ui) {
    ui->has_unsaved_changes = true;
  }
}

void undo_manager_register_command(undo_manager_t *manager, undo_command_t *command) {
  undo_manager_register_command_owned(manager, command, NULL);
}

void undo_manager_register_command_owned(undo_manager_t *manager, undo_command_t *command, const void *owner) {
  if (!command) return;
  // Push the new action to the undo stack
  push_to_stack(&manager->undo_stack, &manager->undo_owners, &manager->undo_count, &manager->undo_capacity, command, owner);
  // A new action clears the redo history
  clear_stack(&manager->redo_stack, &manager->redo_owners, &manager->redo_count, &manager->redo_capacity);
  mark_ui_unsaved(manager);
}

static int purge_owner_from_stack(undo_command_t **stack, const void **owners, int *count, const void *owner) {
  int kept = 0;
  int dropped = 0;
  for (int i = 0; i < *count; ++i) {
    if (owners[i] == owner) {
      if (stack[i] && stack[i]->cleanup) stack[i]->cleanup(stack[i]);
      ++dropped;
      continue;
    }
    stack[kept] = stack[i];
    owners[kept] = owners[i];
    ++kept;
  }
  *count = kept;
  return dropped;
}

int undo_manager_purge_owner(undo_manager_t *manager, const void *owner) {
  if (!owner) return 0;
  int dropped = 0;
  if (manager->undo_stack) dropped += purge_owner_from_stack(manager->undo_stack, manager->undo_owners, &manager->undo_count, owner);
  if (manager->redo_stack) dropped += purge_owner_from_stack(manager->redo_stack, manager->redo_owners, &manager->redo_count, owner);
  return dropped;
}

bool undo_manager_can_undo(const undo_manager_t *manager) { return manager->undo_count > 0; }

bool undo_manager_can_redo(const undo_manager_t *manager) { return manager->redo_count > 0; }

void undo_manager_undo(undo_manager_t *manager, void *ts) {
  const void *owner = NULL;
  undo_command_t *command = pop_from_stack(manager->undo_stack, manager->undo_owners, &manager->undo_count, &owner);
  if (command) {
    command->undo(command, ts);
    push_to_stack(&manager->redo_stack, &manager->redo_owners, &manager->redo_count, &manager->redo_capacity, command, owner);
    model_recalc_physics((timeline_state_t *)ts, 0); // Recalculate physics to be safe
    mark_ui_unsaved(manager);
  }
}

void undo_manager_redo(undo_manager_t *manager, void *ts) {
  const void *owner = NULL;
  undo_command_t *command = pop_from_stack(manager->redo_stack, manager->redo_owners, &manager->redo_count, &owner);
  if (command) {
    command->redo(command, ts);
    push_to_stack(&manager->undo_stack, &manager->undo_owners, &manager->undo_count, &manager->undo_capacity, command, owner);
    model_recalc_physics((timeline_state_t *)ts, 0); // Recalculate physics to be safe
    mark_ui_unsaved(manager);
  }
}

// Both stacks grow with the session and are read newest first, so the rows are
// clipped: the child only builds the entries its 150px actually shows.
static void render_history_stack(const char *id, undo_command_t **stack, int count) {
  igBeginChild_Str(id, (ImVec2){0, 150}, true, 0);
  ImGuiListClipper *clipper = ImGuiListClipper_ImGuiListClipper();
  ImGuiListClipper_Begin(clipper, count, -1.f);
  while (ImGuiListClipper_Step(clipper)) {
    for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row) {
      const int i = count - 1 - row;
      igText("%d. %s", i + 1, stack[i]->description);
    }
  }
  ImGuiListClipper_End(clipper);
  ImGuiListClipper_destroy(clipper);
  igEndChild();
}

void undo_manager_render_history_window(undo_manager_t *manager) {
  if (!manager->show_history_window) return;

  igSetNextWindowSize((ImVec2){300, 400}, ImGuiCond_FirstUseEver);
  if (igBegin("Undo History", &manager->show_history_window, 0)) {
    if (igButton("Clear History", (ImVec2){0, 0})) {
      clear_stack(&manager->undo_stack, &manager->undo_owners, &manager->undo_count, &manager->undo_capacity);
      clear_stack(&manager->redo_stack, &manager->redo_owners, &manager->redo_count, &manager->redo_capacity);
    }
    igSeparator();

    igText("Undo Stack:");
    render_history_stack("UndoStack", manager->undo_stack, manager->undo_count);

    igSeparator();
    igText("Redo Stack:");
    render_history_stack("RedoStack", manager->redo_stack, manager->redo_count);
  }
  igEnd();
}
