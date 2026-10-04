#ifndef INPUT_REPEAT_H
#define INPUT_REPEAT_H

#include <stdbool.h>

typedef struct {
  bool enabled;
  double delay;
  double interval;
} input_repeat_settings_t;

// Settings for synthesizing mouse repeats. Keyboard repeats still come from GLFW.
void input_repeat_init(void);
void input_repeat_shutdown(void);
void input_repeat_refresh(void);
input_repeat_settings_t input_repeat_settings(void);

#endif
