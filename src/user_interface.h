#ifndef USER_INTERFACE_H
#define USER_INTERFACE_H
#include <stdbool.h>

#define MAX_SNIPPETS_PER_PLAYER 64

typedef struct {
  int direction;
  int jump;
  int hook;
  int targetx;
  int targety;
  int fire;
  int wanted_weapon;
} player_input;

typedef struct {
  int id;
  int start_tick;
  int end_tick;
  player_input *player_inputs;
  bool selected;
} input_snippet;

typedef struct {
  int player_id;
  char player_name[32];
  input_snippet snippets[MAX_SNIPPETS_PER_PLAYER];
  int snippet_count;
  bool visible;
} player_track;

typedef struct {
  int current_tick;
  int view_start_tick;
  float zoom;
  float track_height;
  int selected_player_track_index;
  int selected_snippet_id;

  int dragged_snippet_id;
  int dragged_player_track_index;

  int player_track_count;
  player_track *player_tracks;
} timeline_state;

typedef struct {
  bool show_timeline;
  timeline_state timeline;
} ui_handler;

void ui_init(ui_handler *ui);
void ui_render(ui_handler *ui);
void ui_cleanup(ui_handler *ui);

#endif
