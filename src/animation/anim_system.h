#ifndef ANIM_SYSTEM_H
#define ANIM_SYSTEM_H

#include <types.h>

struct anim_keyframe_t {
  float time;
  float x, y;
  float angle;
};

struct anim_sequence_t {
  int num_frames;
  const anim_keyframe_t *frames; // points to static const array
};

struct animation_t {
  const char *name;
  anim_sequence_t body;
  anim_sequence_t back_foot;
  anim_sequence_t front_foot;
  anim_sequence_t attach;
};

struct anim_state_t {
  anim_keyframe_t body;
  anim_keyframe_t back_foot;
  anim_keyframe_t front_foot;
  anim_keyframe_t attach;
};

void anim_seq_eval(const anim_sequence_t *seq, float time, anim_keyframe_t *out);
void anim_add_keyframe(anim_keyframe_t *dst, const anim_keyframe_t *src, float amount);
void anim_state_set(anim_state_t *s, const animation_t *a, float t);
void anim_state_add(anim_state_t *s, const animation_t *a, float t, float amt);

#endif // ANIM_SYSTEM_H
