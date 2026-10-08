#include "entity_match.h"
#include <stdbool.h>
#include <stdlib.h>

static bool both_identified(const uint64_t *last_identity, int k, const uint64_t *now_identity, int e) {
  return last_identity && now_identity && last_identity[k] && now_identity[e];
}

static float distance2(ft_vec2 a, ft_vec2 b) {
  const float dx = a.x - b.x, dy = a.y - b.y;
  return dx * dx + dy * dy;
}

void entity_trail_match(const ft_vec2 *last, const ft_vec2 *last_motion, const uint64_t *last_identity, int last_count,
                        const ft_vec2 *now, const uint64_t *now_identity, int now_count, float reach, int *match) {
  for (int e = 0; e < now_count; ++e) match[e] = -1;
  if (last_count <= 0 || now_count <= 0) return;
  bool *taken = calloc((size_t)last_count, sizeof(*taken));
  int *nearest_now = malloc(sizeof(*nearest_now) * (size_t)last_count);
  ft_vec2 *heading = malloc(sizeof(*heading) * (size_t)last_count);
  if (!taken || !nearest_now || !heading) {
    free(taken);
    free(nearest_now);
    free(heading);
    return;
  }
  const float reach2 = reach * reach;
  // where each was heading: as far again as it last moved
  for (int k = 0; k < last_count; ++k)
    heading[k] = last_motion ? (ft_vec2){last[k].x + last_motion[k].x, last[k].y + last_motion[k].y} : last[k];

  // Two entities that both have an identity are the same one exactly when it agrees.
  for (int e = 0; now_identity && last_identity && e < now_count; ++e) {
    if (!now_identity[e]) continue;
    int nearest = -1;
    float best = reach2;
    for (int k = 0; k < last_count; ++k) {
      if (taken[k] || last_identity[k] != now_identity[e]) continue;
      const float d2 = distance2(heading[k], now[e]);
      if (d2 <= best) {
        best = d2;
        nearest = k;
      }
    }
    if (nearest >= 0) {
      match[e] = nearest;
      taken[nearest] = true;
    }
  }

  // The rest, where one has none: only pairs that are each other's nearest.
  for (int k = 0; k < last_count; ++k) {
    nearest_now[k] = -1;
    if (taken[k]) continue;
    float best = reach2;
    for (int e = 0; e < now_count; ++e) {
      if (match[e] >= 0 || both_identified(last_identity, k, now_identity, e)) continue;
      const float d2 = distance2(heading[k], now[e]);
      if (d2 < best) {
        best = d2;
        nearest_now[k] = e;
      }
    }
  }
  for (int e = 0; e < now_count; ++e) {
    if (match[e] >= 0) continue;
    int nearest = -1;
    float best = reach2;
    for (int k = 0; k < last_count; ++k) {
      if (taken[k] || both_identified(last_identity, k, now_identity, e)) continue;
      const float d2 = distance2(heading[k], now[e]);
      if (d2 < best) {
        best = d2;
        nearest = k;
      }
    }
    if (nearest >= 0 && nearest_now[nearest] == e) {
      match[e] = nearest;
      taken[nearest] = true;
    }
  }
  free(heading);
  free(nearest_now);
  free(taken);
}
