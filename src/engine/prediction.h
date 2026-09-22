#ifndef ENGINE_PREDICTION_H
#define ENGINE_PREDICTION_H

#include <frametee/game_abi.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_PREDICTION_LINES 8
#define MAX_PREDICTION_NAME 64
#define MAX_PREDICTION_COLOR_RULES 8

typedef enum prediction_rule_component_t {
  PREDICTION_COMPONENT_VALUE = 0,
  PREDICTION_COMPONENT_X,
  PREDICTION_COMPONENT_Y,
  PREDICTION_COMPONENT_Z,
  PREDICTION_COMPONENT_MAGNITUDE,
} prediction_rule_component_t;

typedef enum prediction_rule_comparison_t {
  PREDICTION_COMPARE_EQUAL = 0,
  PREDICTION_COMPARE_LESS,
  PREDICTION_COMPARE_GREATER,
  PREDICTION_COMPARE_CHANGED,
} prediction_rule_comparison_t;

typedef enum prediction_change_mode_t {
  PREDICTION_CHANGE_ANY = 0,
  PREDICTION_CHANGE_INCREASING,
  PREDICTION_CHANGE_DECREASING,
} prediction_change_mode_t;

typedef struct prediction_color_rule_t {
  // Player-property ids come from ft_prop_desc and remain stable when a game
  // reorders its reflected property table.
  char property_id[FT_ID_MAX];
  float color[4];
  double target;
  prediction_rule_component_t component;
  prediction_rule_comparison_t comparison;
  prediction_change_mode_t change_mode;
  bool enabled;
} prediction_color_rule_t;

typedef struct prediction_line_t {
  char name[MAX_PREDICTION_NAME];
  float color[4];
  uint64_t controls;
  bool enabled;
  // The first line follows authored inputs. Additional lines hold the input at
  // the playhead and apply their selected reflected controls to it.
  bool use_timeline_inputs;
  // Ordered rules evaluated per segment, falling back to the line colour.
  // Later matches win; CHANGED matches only ticks with a property change.
  prediction_color_rule_t color_rules[MAX_PREDICTION_COLOR_RULES];
  int color_rule_count;
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
void prediction_line_default(prediction_line_t *line, int index);
void prediction_group_cleanup(struct timeline_state *timeline, struct timeline_group_t *group);
void prediction_render_group(struct ui_handler_t *ui, int group_index, const struct ft_world *previous,
                             const struct ft_world *current, float alpha);
void prediction_render_menu(struct timeline_state *timeline);

#endif
