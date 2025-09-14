#include "anim_system.h"

static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

void anim_seq_eval(const anim_sequence_t *seq, float time, anim_keyframe_t *out) {
  if (seq->num_frames == 0) {
    out->time = 0.0f;
    out->x = 0.0f;
    out->y = 0.0f;
    out->angle = 0.0f;
    return;
  }
  if (seq->num_frames == 1) {
    *out = seq->frames[0];
    return;
  }
  /* linear search (short lists, <=6 frames per anim) */
  for (int i = 1; i < seq->num_frames; i++) {
    const anim_keyframe_t *f1 = &seq->frames[i - 1];
    const anim_keyframe_t *f2 = &seq->frames[i];
    if (f1->time <= time && f2->time >= time) {
      float blend = (time - f1->time) / (f2->time - f1->time);
      out->time = time;
      out->x = lerp(f1->x, f2->x, blend);
      out->y = lerp(f1->y, f2->y, blend);
      out->angle = lerp(f1->angle, f2->angle, blend);
      return;
    }
  }
  /* clamp to last */
  *out = seq->frames[seq->num_frames - 1];
}

void anim_add_keyframe(anim_keyframe_t *dst, const anim_keyframe_t *src, float amount) {
  dst->x += src->x * amount;
  dst->y += src->y * amount;
  dst->angle += src->angle * amount;
}

void anim_state_set(anim_state_t *s, const animation_t *a, float t) {
  anim_seq_eval(&a->body, t, &s->body);
  anim_seq_eval(&a->back_foot, t, &s->back_foot);
  anim_seq_eval(&a->front_foot, t, &s->front_foot);
  anim_seq_eval(&a->attach, t, &s->attach);
}

void anim_state_add(anim_state_t *s, const animation_t *a, float t, float amt) {
  anim_state_t tmp;
  anim_state_set(&tmp, a, t);
  anim_add_keyframe(&s->body, &tmp.body, amt);
  anim_add_keyframe(&s->back_foot, &tmp.back_foot, amt);
  anim_add_keyframe(&s->front_foot, &tmp.front_foot, amt);
  anim_add_keyframe(&s->attach, &tmp.attach, amt);
}
