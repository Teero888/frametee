#ifndef USER_INTERFACE_INPUT_EFFECTS_H
#define USER_INTERFACE_INPUT_EFFECTS_H

#include <stdbool.h>
#include <user_interface/timeline/timeline_types.h>

// Marks all derived snippet buffers stale. The next world or input lookup
// rebuilds the complete ordered pipeline once, then playback returns to cached
// lookups.
void input_effects_invalidate(timeline_state_t *timeline);
// Re-evaluates effect parameters, enablement or order while retaining every
// compatible upstream stage. Authored inputs and simulation context changes
// use the full invalidation above.
void input_effects_refresh(timeline_state_t *timeline);
bool input_effects_ensure(timeline_state_t *timeline);

// Returns the evaluated window when one exists, otherwise the authored one.
const input_record_t *input_effects_snippet_window(const input_snippet_t *snippet);

// Derived buffers are never copied or saved. Configurations are deep-copied
// with the snippet and released with its authored input source.
void input_effects_snippet_cleanup(input_snippet_t *snippet);
void input_effects_snippet_discard_cache(input_snippet_t *snippet);
void input_effects_snippet_clone(input_snippet_t *destination, const input_snippet_t *source);

void input_effect_destroy(input_effect_t *effect);
bool input_effect_copy(input_effect_t *destination, const input_effect_t *source);
void input_effect_stack_destroy(input_effect_t *effects, int count);
input_effect_t *input_effect_stack_copy(const input_effect_t *effects, int count);
bool input_effect_stack_equal(const input_effect_t *left, int left_count, const input_effect_t *right, int right_count);
// Merging fuses two windows into one snippet, so both sides have to agree on a
// single stack: an empty side adopts the other's effects, while two different
// non-empty stacks have no combined representation.
bool input_effect_stack_mergeable(const input_effect_t *left, int left_count, const input_effect_t *right, int right_count);
// Replaces the snippet's stack with a deep copy of the given effects.
bool input_effects_snippet_set_stack(input_snippet_t *snippet, const input_effect_t *effects, int count);
bool input_effect_init(game_host_t *host, unsigned type_index, input_effect_t *effect);
const ft_input_effect_desc *input_effect_descriptor(game_host_t *host, const input_effect_t *effect, int *out_index);
int input_effects_enabled_count(const input_snippet_t *snippet);

#endif // USER_INTERFACE_INPUT_EFFECTS_H
