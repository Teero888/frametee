#ifndef INPUT_LANES_H
#define INPUT_LANES_H

// Input lanes: one horizontal strip per schema field, ticks along X.
//
// A lane turns a field into a plain number (a "channel") so every editing tool
// can treat a jump button, a weapon choice and an aim angle the same way. The
// drawing here knows nothing about which window it lives in; it only needs a
// tick-to-pixel mapping and a rectangle, so the timeline can host lanes later.

#include <engine/input_record.h>
#include <stdbool.h>
#include <system/include_cimgui.h>

#define INPUT_LANES_MAX_FIELDS 64
#define INPUT_LANES_MAX 128

typedef enum {
  INPUT_LANE_BARS,  // on/off: filled where held
  INPUT_LANE_ROWS,  // a handful of values: one row per value
  INPUT_LANE_CURVE, // a numeric range: a stepped graph
} input_lane_style_t;

typedef enum {
  INPUT_LANE_VALUE, // the field's own value
  INPUT_LANE_ANGLE, // VEC2 as degrees, 0 = right, 90 = up; writes keep the length
  INPUT_LANE_X,     // VEC2 x component
  INPUT_LANE_Y,     // VEC2 y component
} input_lane_channel_t;

typedef struct input_lane_t {
  int field_index;
  const ft_input_field *field;
  input_lane_channel_t channel;
  input_lane_style_t style;
  double min, max; // value range the lane shows
  int row_count;   // ROWS only
  float height;
  ImU32 color;
} input_lane_t;

// Maps a snippet-relative tick index to screen X.
typedef struct input_lane_view_t {
  float x0; // screen x of index 0
  float px_per_tick;
  int count;
  float clip_min_x, clip_max_x;
} input_lane_view_t;

// Builds the lanes for a schema. `vec2_xy` picks the X/Y view instead of the
// angle for a VEC2 field, `hidden` drops a field; both are indexed by field.
int input_lanes_build(const ft_input_schema *schema, const bool *vec2_xy, const bool *hidden, float scale, input_lane_t *out,
                      int max_lanes);

double input_lane_read(game_host_t *host, const input_lane_t *lane, const input_record_t *record);
void input_lane_write(game_host_t *host, const input_lane_t *lane, input_record_t *record, double value);
// Value under screen y, snapped to what the field can store.
double input_lane_value_at_y(const input_lane_t *lane, float top, float y);
void input_lane_format(const input_lane_t *lane, double value, char *out, size_t out_size);
// Label of one ROWS row, top to bottom.
void input_lane_row_label(const input_lane_t *lane, int row, char *out, size_t out_size);

float input_lane_tick_x(const input_lane_view_t *view, int index);
int input_lane_tick_at(const input_lane_view_t *view, float x); // unclamped
void input_lane_visible_range(const input_lane_view_t *view, int *first, int *last);

void input_lane_draw(ImDrawList *dl, game_host_t *host, const input_lane_t *lane, const input_record_t *records,
                     const input_lane_view_t *view, float top);

#endif // INPUT_LANES_H
