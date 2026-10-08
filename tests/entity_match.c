// Which entity of the step before a prediction trail goes on with (src/engine/entity_match.c).
#include <assert.h>
#include <engine/entity_match.h>
#include <stdio.h>

#define REACH 4.f

// A grenade in flight and a new one fired beside it, listed first: the new one starts a trail of its
// own instead of taking the old one's, which drew the old one's path out of the new one's muzzle and
// started the old one's a step late.
static void new_projectile_beside_another(void) {
  const ft_vec2 last[] = {{10.f, 2.f}};
  const ft_vec2 motion[] = {{-0.6f, 0.1f}};
  const ft_vec2 now[] = {{12.5f, 4.f}, {9.4f, 2.1f}};
  int match[2];

  const uint64_t last_id[] = {11};
  const uint64_t now_id[] = {22, 11};
  entity_trail_match(last, motion, last_id, 1, now, now_id, 2, REACH, match);
  assert(match[0] == -1 && match[1] == 0);

  // A game that gives its entities no identity gets the same from where they were heading.
  entity_trail_match(last, motion, NULL, 1, now, NULL, 2, REACH, match);
  assert(match[0] == -1 && match[1] == 0);
}

// One explodes as another is fired next to it: different identities, so no line between them.
static void spawn_where_another_ended(void) {
  const ft_vec2 last[] = {{5.f, 5.f}};
  const ft_vec2 motion[] = {{0.5f, 0.f}};
  const uint64_t last_id[] = {1};
  const ft_vec2 now[] = {{6.f, 5.2f}};
  const uint64_t now_id[] = {2};
  int match[1];
  entity_trail_match(last, motion, last_id, 1, now, now_id, 1, REACH, match);
  assert(match[0] == -1);
}

// A shotgun's pellets start at one point, so only their identities tell them apart.
static void pellets_from_one_point(void) {
  const ft_vec2 last[] = {{0.f, 0.f}, {0.f, 0.f}, {0.f, 0.f}};
  const uint64_t last_id[] = {7, 8, 9};
  const ft_vec2 now[] = {{1.6f, -0.4f}, {1.7f, 0.f}, {1.6f, 0.4f}};
  const uint64_t now_id[] = {9, 7, 8};
  int match[3];
  entity_trail_match(last, NULL, last_id, 3, now, now_id, 3, REACH, match);
  assert(match[0] == 2 && match[1] == 0 && match[2] == 1);
}

// Two paths crossing keep their own trails by where each was heading; by distance alone they would
// swap, each being nearer the other's new position than its own.
static void crossing_paths(void) {
  const ft_vec2 last[] = {{0.f, 0.f}, {1.8f, 0.1f}};
  const ft_vec2 motion[] = {{1.f, 0.f}, {-1.f, 0.f}};
  const ft_vec2 now[] = {{0.8f, 0.1f}, {1.f, 0.f}};
  int match[2];
  entity_trail_match(last, motion, NULL, 2, now, NULL, 2, REACH, match);
  assert(match[0] == 1 && match[1] == 0);
  entity_trail_match(last, NULL, NULL, 2, now, NULL, 2, REACH, match);
  assert(match[0] == 0 && match[1] == 1);
}

// Nothing joins across more than the reach, and nothing joins twice.
static void reach_and_uniqueness(void) {
  const ft_vec2 last[] = {{0.f, 0.f}};
  const ft_vec2 far[] = {{10.f, 0.f}};
  int match[2];
  entity_trail_match(last, NULL, NULL, 1, far, NULL, 1, REACH, match);
  assert(match[0] == -1);

  const ft_vec2 two[] = {{0.5f, 0.f}, {0.6f, 0.f}};
  entity_trail_match(last, NULL, NULL, 1, two, NULL, 2, REACH, match);
  assert((match[0] == 0) + (match[1] == 0) == 1);
  entity_trail_match(last, NULL, NULL, 0, two, NULL, 2, REACH, match);
  assert(match[0] == -1 && match[1] == -1);
}

int main(void) {
  new_projectile_beside_another();
  spawn_where_another_ended();
  pellets_from_one_point();
  crossing_paths();
  reach_and_uniqueness();
  puts("Entity match tests passed");
  return 0;
}
