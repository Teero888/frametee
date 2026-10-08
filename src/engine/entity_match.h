#ifndef ENGINE_ENTITY_MATCH_H
#define ENGINE_ENTITY_MATCH_H

#include <frametee/game_abi.h>
#include <stdint.h>

// Pairs one step's entities of a class with the step before's, for the trails the prediction draws
// (entity indices do not last from one step to the next). Two entities that both have an identity
// (FT_PROP_IDENTITY folded into a value; 0 for none) are the same one exactly when it agrees.
// Without one, entities pair up only where each is the other's nearest, measured from where the
// earlier one was heading (`last` plus `last_motion`, how far it moved into its step), so that a
// projectile which appears while another is still close by does not take that one's trail. Every
// pair is within `reach`. `match[e]` becomes the index into `last`, or -1 for a trail that starts.
// `last_motion`, `last_identity` and `now_identity` may be NULL.
void entity_trail_match(const ft_vec2 *last, const ft_vec2 *last_motion, const uint64_t *last_identity, int last_count,
                        const ft_vec2 *now, const uint64_t *now_identity, int now_count, float reach, int *match);

#endif // ENGINE_ENTITY_MATCH_H
