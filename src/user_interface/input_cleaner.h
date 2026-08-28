#ifndef USER_INTERFACE_INPUT_CLEANER_H
#define USER_INTERFACE_INPUT_CLEANER_H

#include <stdbool.h>
#include <stdint.h>
#include <types.h>

typedef struct input_clean_result_t {
  int changed_values;
  int changed_rows;
  int changed_fields;
  int passes;
  uint64_t simulations;
  uint64_t simulated_ticks;
} input_clean_result_t;

// Replaces run-irrelevant selected fields in one snippet with the game's
// canonical defaults. A NULL clean_fields selects the complete schema. Every
// accepted replacement preserves the exact run state of every player through
// the end of the authored group.
bool input_cleaner_clean_snippet(ui_handler_t *ui, int track_index, input_snippet_t *snippet, const bool *clean_fields,
                                 uint32_t clean_field_count, input_clean_result_t *out);

#endif // USER_INTERFACE_INPUT_CLEANER_H
