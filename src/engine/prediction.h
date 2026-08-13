#ifndef ENGINE_PREDICTION_H
#define ENGINE_PREDICTION_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PREDICTION_LINES 8
#define MAX_PREDICTION_NAME 64

typedef struct prediction_line_t {
  char name[MAX_PREDICTION_NAME];
  float color[4];
  uint64_t controls;
  bool enabled;
  // The first line follows authored inputs. Additional lines hold the input at
  // the playhead and apply their selected reflected controls to it.
  bool use_timeline_inputs;
} prediction_line_t;

typedef struct prediction_settings_t {
  bool enabled;
  int length;
  float thickness;
  prediction_line_t lines[MAX_PREDICTION_LINES];
  int line_count;
} prediction_settings_t;

struct ft_world;
struct timeline_group_t;
struct timeline_state;
struct ui_handler_t;

void prediction_settings_default(prediction_settings_t *settings);
void prediction_group_cleanup(struct timeline_state *timeline, struct timeline_group_t *group);
void prediction_render_group(struct ui_handler_t *ui, int group_index, const struct ft_world *previous,
                             const struct ft_world *current, float alpha);
void prediction_render_menu(struct timeline_state *timeline);

#endif
