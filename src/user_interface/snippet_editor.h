#ifndef SNIPPET_EDITOR_H
#define SNIPPET_EDITOR_H

#include <types.h>

// Renders the snippet editor panel
void render_snippet_editor_panel(ui_handler_t *ui);

// Drop edit state that refers to the current timeline. Call this before a
// project, level, or game change; cleanup also releases the retained UI buffer.
void snippet_editor_reset(void);
void snippet_editor_cleanup(void);

#endif // SNIPPET_EDITOR_H
