#ifndef ANIM_SYSTEM_H
#define ANIM_SYSTEM_H

typedef struct {
  float time;
  float x, y;
  float angle;
} anim_keyframe_t;

typedef struct {
  int num_frames;
  const anim_keyframe_t *frames; // points to static const array
} anim_sequence_t;

typedef struct {
  const char *name;
  anim_sequence_t body;
  anim_sequence_t back_foot;
  anim_sequence_t front_foot;
  anim_sequence_t attach;
} animation_t;

typedef struct {
  anim_keyframe_t body;
  anim_keyframe_t back_foot;
  anim_keyframe_t front_foot;
  anim_keyframe_t attach;
} anim_state_t;

void anim_seq_eval(const anim_sequence_t *seq, float time, anim_keyframe_t *out);
void anim_add_keyframe(anim_keyframe_t *dst, const anim_keyframe_t *src, float amount);
void anim_state_set(anim_state_t *s, const animation_t *a, float t);
void anim_state_add(anim_state_t *s, const animation_t *a, float t, float amt);

#endif // ANIM_SYSTEM_H
