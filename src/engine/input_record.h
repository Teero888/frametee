#ifndef ENGINE_INPUT_RECORD_H
#define ENGINE_INPUT_RECORD_H

// The engine's storage for one tick of one player's input.
//
// The bytes belong to the active game: the engine copies them into snippets,
// writes them to project files and hands them back at simulation time, but
// never interprets them. Fields are read and written only through the game's
// input schema, via the helpers below.
//
// The record is a fixed-size buffer rather than a pointer plus a runtime stride
// so that every array walk, assignment and memcpy in the timeline stays
// ordinary C. A game whose record does not fit is rejected at load time with a
// clear message, which is far better than every input operation in the editor
// paying for a dynamic stride.

#include "game_host.h"
#include <stdint.h>
#include <string.h>

// Generous next to DDNet's 16 bytes. Raising it costs memory per stored tick,
// so it is checked against the schema at load rather than being open ended.
#define ENGINE_MAX_INPUT_RECORD 64


typedef union input_record_t {
  // Gives bytes enough alignment for every record layout accepted by the
  // module validator, including DDNet's explicitly 8-byte-aligned input.
  long double scalar_alignment;
  void *pointer_alignment;
  uint8_t bytes[ENGINE_MAX_INPUT_RECORD];
} input_record_t;

#if defined(_MSC_VER) && !defined(__clang__)
#define ENGINE_INPUT_RECORD_ALIGNMENT __alignof(input_record_t)
#else
#define ENGINE_INPUT_RECORD_ALIGNMENT __alignof__(input_record_t)
#endif

// Resolves the schema field that follows the recording cursor. Safe to call
// whenever the game changes; no field identifier is special to the engine.
void engine_input_bind(game_host_t *host);
int engine_input_cursor_field(void);

// Field access. `field` is a schema index; passing -1 is a no-op returning 0,
// so callers can use an unresolved well-known field without branching.
long long engine_input_get(game_host_t *host, const input_record_t *record, int field);
void engine_input_set(game_host_t *host, input_record_t *record, int field, long long value);
float engine_input_get_float(game_host_t *host, const input_record_t *record, int field);
void engine_input_set_float(game_host_t *host, input_record_t *record, int field, float value);
ft_vec2 engine_input_get_vec2(game_host_t *host, const input_record_t *record, int field);
void engine_input_set_vec2(game_host_t *host, input_record_t *record, int field, ft_vec2 value);
void engine_input_default(game_host_t *host, input_record_t *record);

// True when the game's records fit the engine's storage. Checked once when a
// game is activated.
bool engine_input_record_fits(const game_host_t *host);

#endif // ENGINE_INPUT_RECORD_H
