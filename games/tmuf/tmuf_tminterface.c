// TMInterface's input scripts (.txt): a line per input change, at the race
// time of the step it drives ("0.00 press up", "1.23 steer -12345", "4.56
// press enter"): the input of tick n is written at n * 10 - 2600 ms, so race
// time 0.00 is tick 260, the race's first step (before it the car is held for
// the countdown and nothing counts). A TAS of D09 from the TMInterface
// community noseboosts with this and crashes a tick off.
//
// Steering is the analog axis, negative to the left (the game's keys are
// +-65536); a key event that leaves the input as it was (a second steering
// key, which cancels a Stunts master jump) is the steering written again.

#include "tmuf_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_TICK ((int32_t)(TMUF_RACE_START_MS / TMUF_TICK_MS)) // the step at 0.00
#define GAS_THRESHOLD 19661 // where an analog pedal counts as pressed (the game's)

static int32_t tick_ms(int32_t tick) { return tick * (int32_t)TMUF_TICK_MS - (int32_t)TMUF_RACE_START_MS; }
// the tick a command at `ms` drives (anything before the start: the first)
static int32_t tick_of(int32_t ms) { return ms < 0 ? FIRST_TICK : (ms + (int32_t)TMUF_RACE_START_MS) / (int32_t)TMUF_TICK_MS; }

// --- writing -----------------------------------------------------------------

typedef struct text {
  char *data;
  size_t size, cap;
  bool failed;
} text;

static void add(text *t, const char *fmt, ...) {
  char line[96];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0 || t->failed) return;
  if (t->size + (size_t)n + 1 > t->cap) {
    const size_t cap = t->cap ? t->cap * 2 + (size_t)n : 4096;
    char *grown = realloc(t->data, cap);
    if (!grown) {
      t->failed = true;
      return;
    }
    t->data = grown;
    t->cap = cap;
  }
  memcpy(t->data + t->size, line, (size_t)n + 1);
  t->size += (size_t)n;
}

char *tm_tmi_write(const tm_input *inputs, int32_t first_tick, int32_t count) {
  text t = {0};
  add(&t, "");
  tm_input held = {0};
  bool respawn_held = false;
  for (int32_t i = 0; i < count; i++) {
    const int32_t tick = first_tick + i;
    if (tick < FIRST_TICK) continue;
    const tm_input in = inputs[i];
    const int32_t ms = tick_ms(tick);
    char at[32];
    snprintf(at, sizeof at, "%d.%02d", ms / 1000, ms % 1000 / 10);
    if (respawn_held) add(&t, "%s rel enter\n", at), respawn_held = false;
    if (in.accelerate != held.accelerate) add(&t, "%s %s up\n", at, in.accelerate ? "press" : "rel");
    if (in.brake != held.brake) add(&t, "%s %s down\n", at, in.brake ? "press" : "rel");
    if (in.steer != held.steer || (in.input_event && in.accelerate == held.accelerate && in.brake == held.brake))
      add(&t, "%s steer %d\n", at, (int)in.steer);
    if (in.respawn) add(&t, "%s press enter\n", at), respawn_held = true;
    held = in;
  }
  if (t.failed) {
    free(t.data);
    return NULL;
  }
  return t.data;
}

// --- reading -----------------------------------------------------------------

enum { EV_PRESS, EV_REL, EV_STEER, EV_GAS };
enum { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_DELETE, KEY_COUNT };

typedef struct event {
  int32_t ms;
  uint32_t order; // in the file, for events at the same time
  uint8_t kind, key;
  int32_t value;
} event;

typedef struct events {
  event *items;
  uint32_t count, cap;
} events;

static bool push(events *e, int32_t ms, uint8_t kind, uint8_t key, int32_t value) {
  if (e->count == e->cap) {
    const uint32_t cap = e->cap ? e->cap * 2 : 256;
    event *grown = realloc(e->items, cap * sizeof *grown);
    if (!grown) return false;
    e->items = grown;
    e->cap = cap;
  }
  e->items[e->count] = (event){ms, e->count, kind, key, value};
  e->count++;
  return true;
}

// A time: "1.23" seconds, "1:02.34" minutes and seconds, "1230" milliseconds.
static bool parse_time(const char *s, size_t n, int32_t *out) {
  if (!n) return false;
  int64_t whole = 0, minutes = 0, frac = 0, frac_digits = 0;
  bool dot = false, colon = false, digits = false;
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      digits = true;
      if (dot) {
        if (frac_digits < 3) frac = frac * 10 + (c - '0'), frac_digits++;
      } else {
        whole = whole * 10 + (c - '0');
      }
      if (whole > 100000000) return false;
    } else if (c == ':' && !colon && !dot) {
      colon = true;
      minutes = whole;
      whole = 0;
    } else if (c == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  while (frac_digits < 3) frac *= 10, frac_digits++;
  // a bare number is milliseconds; with a point or a colon, seconds
  *out = dot || colon ? (int32_t)((minutes * 60 + whole) * 1000 + frac) : (int32_t)whole;
  return true;
}

static int key_of(const char *word) {
  static const char *const names[KEY_COUNT] = {"up", "down", "left", "right", "enter", "delete"};
  for (int k = 0; k < KEY_COUNT; k++)
    if (strcmp(word, names[k]) == 0) return k;
  return -1;
}

// One command ("1.00 press up", "1.00-2.00 steer 3000"); false when it is
// not an input command (TMInterface's other commands are skipped).
static bool parse_command(char *cmd, events *out) {
  char *words[4];
  int n = 0;
  for (char *w = strtok(cmd, " \t"); w && n < 4; w = strtok(NULL, " \t"))
    words[n++] = w;
  if (n < 2) return false;
  // the time, or a range "a-b"
  int32_t from, to = -1;
  char *dash = strchr(words[0], '-');
  if (dash ? !parse_time(words[0], (size_t)(dash - words[0]), &from) || !parse_time(dash + 1, strlen(dash + 1), &to)
           : !parse_time(words[0], strlen(words[0]), &from))
    return false;
  for (char *c = words[1]; *c; c++)
    *c = (char)tolower((unsigned char)*c);
  if ((!strcmp(words[1], "press") || !strcmp(words[1], "rel")) && n >= 3) {
    for (char *c = words[2]; *c; c++)
      *c = (char)tolower((unsigned char)*c);
    const int key = key_of(words[2]);
    if (key < 0) return false;
    const bool press = !strcmp(words[1], "press");
    if (!push(out, from, press ? EV_PRESS : EV_REL, (uint8_t)key, 0)) return false;
    return to < 0 || push(out, to, EV_REL, (uint8_t)key, 0);
  }
  if ((!strcmp(words[1], "steer") || !strcmp(words[1], "gas")) && n >= 3) {
    char *end;
    const long v = strtol(words[2], &end, 10);
    if (*end) return false;
    const uint8_t kind = !strcmp(words[1], "steer") ? EV_STEER : EV_GAS;
    if (!push(out, from, kind, 0, (int32_t)v)) return false;
    return to < 0 || push(out, to, kind, 0, 0);
  }
  return false;
}

static int compare_events(const void *a, const void *b) {
  const event *x = a, *y = b;
  if (x->ms != y->ms) return x->ms < y->ms ? -1 : 1;
  return x->order < y->order ? -1 : x->order > y->order;
}

tm_input *tm_tmi_read(const char *data, size_t size, int32_t *out_count, uint32_t *out_skipped) {
  *out_count = 0;
  if (out_skipped) *out_skipped = 0;
  char *copy = malloc(size + 1);
  if (!copy) return NULL;
  memcpy(copy, data, size);
  copy[size] = '\0';
  events ev = {0};
  uint32_t skipped = 0;
  for (char *line = copy; line && *line;) {
    char *next = strpbrk(line, "\r\n");
    if (next) *next++ = '\0';
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    // several commands on a line, separated by ';'
    for (char *cmd = line; cmd;) {
      char *semi = strchr(cmd, ';');
      if (semi) *semi++ = '\0';
      while (isspace((unsigned char)*cmd)) cmd++;
      if (*cmd && !parse_command(cmd, &ev)) skipped++;
      cmd = semi;
    }
    line = next;
  }
  free(copy);
  if (out_skipped) *out_skipped = skipped;
  qsort(ev.items, ev.count, sizeof *ev.items, compare_events);

  // the ticks up to the last event's
  int32_t last = FIRST_TICK;
  for (uint32_t i = 0; i < ev.count; i++) {
    const int32_t tick = tick_of(ev.items[i].ms);
    if (tick > last) last = tick;
  }
  const int32_t count = last + 1;
  tm_input *inputs = calloc((size_t)count, sizeof *inputs);
  if (!inputs) {
    free(ev.items);
    return NULL;
  }
  // the keys and axes as the game reads them: digital or analog, whichever
  // changed last
  bool keys[KEY_COUNT] = {0}, steer_analog = false, gas_analog = false;
  int32_t steer = 0, gas = 0;
  tm_input held = {0};
  uint32_t e = 0;
  for (int32_t tick = FIRST_TICK; tick < count; tick++) {
    tm_input in = held;
    in.respawn = in.input_event = 0;
    bool driving = false;
    for (; e < ev.count; e++) {
      const event *x = &ev.items[e];
      const int32_t at = tick_of(x->ms);
      if (at > tick) break;
      switch (x->kind) {
      case EV_PRESS:
      case EV_REL:
        if (x->key == KEY_ENTER && x->kind == EV_PRESS && !keys[KEY_ENTER]) in.respawn = 1;
        keys[x->key] = x->kind == EV_PRESS;
        if (x->key == KEY_LEFT || x->key == KEY_RIGHT) steer_analog = false, driving = true;
        if (x->key == KEY_UP || x->key == KEY_DOWN) gas_analog = false, driving = true;
        break;
      case EV_STEER:
        steer = x->value < -65536 ? -65536 : x->value > 65536 ? 65536 : x->value;
        steer_analog = driving = true;
        break;
      case EV_GAS:
        gas = x->value;
        gas_analog = driving = true;
        break;
      }
    }
    in.steer = steer_analog ? steer : keys[KEY_LEFT] ? -65536 : keys[KEY_RIGHT] ? 65536 : 0;
    in.accelerate = gas_analog ? gas >= GAS_THRESHOLD : keys[KEY_UP];
    in.brake = gas_analog ? gas <= -GAS_THRESHOLD : keys[KEY_DOWN];
    in.input_event = driving;
    inputs[tick] = in;
    held = in;
  }
  free(ev.items);
  *out_count = count;
  return inputs;
}
