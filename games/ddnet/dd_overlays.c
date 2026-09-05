// DDNet presentation that lives above the sprite/map passes: scalable text,
// authored chat and emotes, freeze bars, speedup arrows and doors.

#include "dd_internal.h"
#include "dd_profile.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static ft_vec2 character_position(const SCharacterCore *character, float alpha) {
  return (ft_vec2){lerpf(vgetx(character->m_PrevPos), vgetx(character->m_Pos), alpha) / PX_PER_TILE,
                   lerpf(vgety(character->m_PrevPos), vgety(character->m_Pos), alpha) / PX_PER_TILE};
}

static float pixels_to_world(ft_game *game, float pixels) {
  ft_camera camera;
  game->engine->camera_get(&camera);
  return camera.viewport.y > 0.f ? pixels * camera.visible.h / camera.viewport.y : pixels / PX_PER_TILE;
}

// Screen-space text: `size` is already in pixels, so it is also the size DDNet
// would have baked the glyph at, and the outline is DefaultTextOutlineColor.
static void draw_text_screen(ft_game *game, float x, float y, float size, ft_color color, const char *text) {
  if (!text || !text[0]) return;
  dd_text_draw_outlined(game, DD_Z_OVERLAYS, game->engine->screen_to_world((ft_vec2){x, y}),
                        pixels_to_world(game, size), color, (ft_color){0.f, 0.f, 0.f, 0.3f * color.a}, size, text);
}

static const char *profile_name(const ft_render_frame *frame, int player, char *fallback, size_t fallback_size) {
  if (player < 0 || (uint32_t)player >= frame->player_setup_count) {
    snprintf(fallback, fallback_size, "player %d", player + 1);
    return fallback;
  }
  const ft_player_setup *setup = &frame->player_setups[player];
  dd_player_profile_t profile;
  dd_profile_from_setup(setup, &profile);
  if (profile.name[0]) {
    snprintf(fallback, fallback_size, "%s", profile.name);
    return fallback;
  }
  if (setup->track_name && setup->track_name[0]) return setup->track_name;
  snprintf(fallback, fallback_size, "player %d", player + 1);
  return fallback;
}

// CNamePlates' geometry, in DDNet's world pixels. A plate is a stack of lines
// whose bottom edge sits cl_nameplates_offset above the tee and which grow
// upwards; every part is padded by DEFAULT_PADDING, and a line's box is its
// font size plus that padding. All of it is world space, so the plate zooms
// with the tees.
#define DD_NAMEPLATE_PADDING 5.f

// The size settings are percentages a world-pixel font size is derived from,
// which is why they reach down to -50: the base is what 0 percent gives.
static float nameplate_font_size(int percent) { return (18.f + 20.f * (float)percent / 100.f) / PX_PER_TILE; }

// Draws one line of the stack, centred on the tee and sitting on `bottom`, and
// returns the bottom edge the line above it starts from.
static float draw_nameplate_line(ft_game *game, float center_x, float bottom, float size, ft_color color,
                                 const char *text) {
  const float padding = DD_NAMEPLATE_PADDING / PX_PER_TILE;
  if (!text || !text[0]) return bottom;
  const ft_vec2 top_left = {center_x - dd_text_width(game, size, text) * 0.5f, bottom - size - padding * 0.5f};
  // CNamePlatePartText outlines at half alpha rather than the default 0.3, and
  // bakes its glyphs at interface scale, so the thickness follows the plate's
  // own font size rather than however far the camera is zoomed.
  dd_text_draw_outlined(game, DD_Z_OVERLAYS, top_left, size, color, (ft_color){0.f, 0.f, 0.f, 0.5f * color.a},
                        size * PX_PER_TILE, text);
  return bottom - size - padding;
}

static void render_nameplates(ft_game *game, const ft_render_frame *frame) {
  if (!game->settings.render_nameplates || !frame->world) return;
  const SWorldCore *world = &frame->world->core;
  ft_camera camera;
  game->engine->camera_get(&camera);

  const float name_size = nameplate_font_size(game->settings.nameplate_size);
  const float clan_size = nameplate_font_size(game->settings.nameplate_clan_size);

  for (int player = 0; player < world->m_NumCharacters; ++player) {
    ft_vec2 pos = character_position(&world->m_pCharacters[player], frame->alpha);
    // DDNet assumes the plate fits an 800x800 box above the tee, so a tee that
    // far below the view can still have a visible one.
    if (pos.x < camera.visible.x - 400.f / PX_PER_TILE || pos.x > camera.visible.x + camera.visible.w + 400.f / PX_PER_TILE ||
        pos.y < camera.visible.y || pos.y > camera.visible.y + camera.visible.h + 800.f / PX_PER_TILE)
      continue;

    char fallback[32];
    const char *name = profile_name(frame, player, fallback, sizeof(fallback));
    const ft_color color = {1.f, 1.f, 1.f, frame->opacity};

    float bottom = pos.y - (float)game->settings.nameplate_offset / PX_PER_TILE;
    bottom = draw_nameplate_line(game, pos.x, bottom, name_size, color, name);

    if (game->settings.nameplate_clan && (uint32_t)player < frame->player_setup_count) {
      dd_player_profile_t profile;
      dd_profile_from_setup(&frame->player_setups[player], &profile);
      draw_nameplate_line(game, pos.x, bottom, clan_size, color, profile.clan);
    }
  }
}

// One of CFreezeBars' six quads. Every subset it asks for runs from an edge of
// the sprite inwards, so a piece is a sprite (plain, or its mirrored twin), the
// fraction of that sprite to sample -- which the tiling factor selects, since
// the shader wraps the quad's 0..tiling range into the sprite -- and, when the
// subset runs right to left, a negated width that flips the quad in place.
static void draw_freeze_piece(ft_game *game, float x, float y, float width, float height, uint32_t sprite,
                              float subset, bool reversed, float alpha) {
  if (!(width > 0.f) || !(subset > 0.f)) return;
  const ft_sprite_draw draw = {.pos = {x + width * 0.5f, y + height * 0.5f},
                               .size = {reversed ? -width : width, height},
                               .rotation = 0.f,
                               .sprite_index = sprite,
                               .color = {1.f, 1.f, 1.f, alpha},
                               .tiling = {subset, 1.f}};
  dd_draw_sprites(game, game->gfx.freeze_bar, DD_Z_OVERLAYS, &draw, 1);
}

// CFreezeBars::RenderFreezeBarPos, in tiles rather than DDNet's world pixels.
// The bar is a rounded cap, a stretched middle and a second cap, each split
// into a full and an empty half wherever the progress boundary falls in it.
static void draw_freeze_bar(ft_game *game, ft_vec2 position, float progress, float alpha) {
  if (progress < 0.f) progress = 0.f;
  if (progress > 1.f) progress = 1.f;

  const float bar_width = 64.f / PX_PER_TILE;
  const float bar_height = 16.f / PX_PER_TILE;
  // Half of each end piece shows progress and half is the cap's rounding.
  const float rest_pct = 0.5f, prog_pct = 0.5f;
  // The sprites are square, so an end is as wide as the bar is tall.
  const float end_width = bar_height;
  const float middle_width = bar_width - end_width * 2.f;
  const float end_prog_width = end_width * prog_pct;
  const float end_rest_width = end_width * rest_pct;
  const float progress_width = bar_width - end_prog_width * 2.f;
  const float end_proportion = end_prog_width / progress_width;
  const float middle_proportion = middle_width / progress_width;

  float x = position.x - bar_width * 0.5f;
  const float y = position.y + 32.f / PX_PER_TILE;

  const float begin = progress <= end_proportion ? progress / end_proportion : 1.f;
  draw_freeze_piece(game, x, y, end_rest_width + end_prog_width * begin, bar_height, DD_FREEZE_FULL_LEFT,
                    rest_pct + prog_pct * begin, false, alpha);
  if (begin < 1.f)
    draw_freeze_piece(game, x + end_rest_width + end_prog_width * begin, y, end_prog_width * (1.f - begin),
                      bar_height, DD_FREEZE_EMPTY_RIGHT, prog_pct - prog_pct * begin, true, alpha);

  x += end_width;
  float middle = 1.f;
  if (progress <= end_proportion + middle_proportion)
    middle = progress <= end_proportion ? 0.f : (progress - end_proportion) / middle_proportion;
  const float full_middle = middle_width * middle;
  const float empty_middle = middle_width - full_middle;
  // Sampling only as much of the sprite as the piece is wide keeps the edge
  // pixels of a nearly empty bar from smearing across it.
  draw_freeze_piece(game, x, y, full_middle, bar_height, DD_FREEZE_FULL,
                    full_middle <= end_width ? full_middle / end_width : 1.f, false, alpha);
  draw_freeze_piece(game, x + full_middle, y, empty_middle, bar_height, DD_FREEZE_EMPTY,
                    empty_middle <= end_width ? empty_middle / end_width : 1.f, true, alpha);

  x += middle_width;
  float end = 1.f;
  if (progress <= end_proportion + middle_proportion)
    end = 0.f;
  else
    end = (progress - end_proportion - middle_proportion) / end_proportion;
  draw_freeze_piece(game, x, y, end_prog_width * end, bar_height, DD_FREEZE_MIRRORED + DD_FREEZE_FULL_LEFT,
                    prog_pct * end, false, alpha);
  draw_freeze_piece(game, x + end_prog_width * end, y, end_prog_width * (1.f - end) + end_rest_width, bar_height,
                    DD_FREEZE_MIRRORED + DD_FREEZE_EMPTY_RIGHT, 1.f - prog_pct * end, true, alpha);
}

static void render_freeze_bars(ft_game *game, const ft_render_frame *frame) {
  if (!game->settings.render_freeze_bars || !frame->world) return;
  const SWorldCore *world = &frame->world->core;

  for (int player = 0; player < world->m_NumCharacters; ++player) {
    const SCharacterCore *character = &world->m_pCharacters[player];
    if (character->m_DeepFrozen || character->m_IsInFreeze || character->m_FreezeTime <= 0 || character->m_FreezeStart <= 0)
      continue;

    // DDNet's Max is the whole freeze and its numerator the ticks left, which
    // is exactly what the core already tracks as m_FreezeTime.
    const int freeze_end = world->m_GameTick + character->m_FreezeTime;
    const int duration = freeze_end - character->m_FreezeStart;
    if (duration <= 0) continue;

    draw_freeze_bar(game, character_position(character, frame->alpha),
                    (float)character->m_FreezeTime / (float)duration, frame->opacity);
  }
}

typedef struct visible_event {
  int tick;
  dd_event_payload_t payload;
} visible_event;

static int recent_events(ft_game *game, const ft_render_frame *frame, dd_event_type_t type, int lifetime,
                         visible_event *out, int capacity) {
  if (!game->engine->timeline_event_count || !game->engine->timeline_event_get || capacity <= 0) return 0;
  int count = 0;
  const uint32_t event_count = game->engine->timeline_event_count();
  for (uint32_t index = 0; index < event_count; ++index) {
    ft_timeline_event event = {.struct_size = sizeof(event)};
    dd_event_payload_t payload;
    if (!game->engine->timeline_event_get(index, &event) || event.world_index != frame->world_index ||
        event.tick > frame->tick || event.tick <= frame->tick - lifetime || !dd_event_decode(&event, &payload) ||
        payload.type != (int)type)
      continue;

    int insert;
    if (count < capacity) {
      insert = count++;
    } else {
      // The output is chronological. Once it is full, discard the oldest
      // entry so chat and emote rendering always follows the newest events.
      if (event.tick <= out[0].tick) continue;
      memmove(out, out + 1, (size_t)(capacity - 1) * sizeof(*out));
      insert = capacity - 1;
    }
    while (insert > 0 && out[insert - 1].tick > event.tick) {
      if (insert < capacity) out[insert] = out[insert - 1];
      --insert;
    }
    out[insert] = (visible_event){.tick = event.tick, .payload = payload};
  }
  return count;
}

// CChat lays its messages out in a virtual screen 300 units tall, anchored at
// the bottom left and growing upwards with the newest message lowest. These are
// its defaults in that space: cl_chat_size 60 over ten, and the paddings and
// width derived from it.
#define DD_CHAT_VIRTUAL_HEIGHT 300.f
#define DD_CHAT_TEE_PADDING_RIGHT 0.5f
#define DD_CHAT_LEFT 5.f
// EnsureCoherentWidth: a width below this multiple of the font size is widened
// instead, so a large font never has to wrap after two characters.
#define DD_CHAT_FONTSIZE_WIDTH_RATIO 2.5f

// Everything CChat derives from cl_chat_size, in its 300-tall virtual space.
typedef struct {
  float font_size, padding_x, padding_y, rounding, tee_size, tee_gap, width;
} chat_metrics_t;

static chat_metrics_t chat_metrics(const ft_game *game) {
  chat_metrics_t m;
  m.font_size = (float)game->settings.chat_font_size / 10.f;
  m.padding_x = m.font_size * (5.f / 6.f);
  m.padding_y = m.font_size * (1.f / 6.f);
  m.rounding = m.font_size * (1.f / 2.f);
  m.tee_size = m.font_size * (7.f / 6.f);
  m.tee_gap = m.tee_size + DD_CHAT_TEE_PADDING_RIGHT;
  m.width = (float)game->settings.chat_width;
  if (m.width < m.font_size * DD_CHAT_FONTSIZE_WIDTH_RATIO) m.width = m.font_size * DD_CHAT_FONTSIZE_WIDTH_RATIO;
  return m;
}
// Messages stop here so they never climb into the scoreboard's half.
#define DD_CHAT_HEIGHT_LIMIT 200.f
#define DD_CHAT_LIFETIME (16 * GAME_TICK_SPEED)
#define DD_CHAT_FADE_START (14 * GAME_TICK_SPEED)
#define DD_CHAT_MAX_ROWS 8

// CTextRender::WordLength: everything up to and including the next separator,
// so a trailing space travels with its word and never starts the next line.
static int chat_word_length(const char *text) {
  int i = 0;
  // The separators are ASCII and a UTF-8 continuation byte is never one of
  // them, so scanning bytes lands on the same boundary decoding would.
  while (text[i] && text[i] != '\n' && text[i] != '\t' && text[i] != ' ') ++i;
  return text[i] ? i + 1 : i;
}

static float chat_measure(ft_game *game, float size, const char *text, int length) {
  char buffer[256];
  if (length >= (int)sizeof(buffer)) length = (int)sizeof(buffer) - 1;
  if (length < 0) length = 0;
  memcpy(buffer, text, (size_t)length);
  buffer[length] = '\0';
  return dd_text_width(game, size, buffer);
}

// TEXTFLAG_STOP_AT_END: the longest prefix of a word that still fits the space
// left on the line, in whole characters. Reports how many it placed, which is
// what decides whether the remainder is worth keeping here.
static int chat_fit(ft_game *game, float size, const char *text, int length, float budget, int *out_glyphs) {
  int fit = 0, glyphs = 0;
  for (int i = 0; i < length;) {
    int next = i + 1;
    while (next < length && ((unsigned char)text[next] & 0xC0u) == 0x80u) ++next;
    if (chat_measure(game, size, text, next) > budget) break;
    fit = next;
    ++glyphs;
    i = next;
  }
  *out_glyphs = glyphs;
  return fit;
}

// A port of the wrapping in CTextRender::TextEx. DrawX advances along the line
// while the budget is measured from StartX, so a word breaks when its *end*
// would pass StartX + LineWidth rather than when the word itself is too wide.
// Every row of one message shares that StartX -- CChat hands the message a
// cursor starting where the name ended -- so wrapped rows hang under the
// message instead of returning to the margin.
static int chat_wrap(ft_game *game, const char *text, float size, float line_width, uint16_t *starts, uint16_t *lengths) {
  const int length = (int)strlen(text);
  int rows = 0, cur = 0, row_start = 0, last_break = -1;
  float draw_x = 0.f;
  bool got_newline_last = false;

  while (cur < length && rows < DD_CHAT_MAX_ROWS) {
    int wlen = chat_word_length(text + cur);
    if (wlen > length - cur) wlen = length - cur;
    const float word_width = chat_measure(game, size, text + cur, wlen);
    bool new_line = false;

    if (word_width > line_width) {
      // The word cannot fit a line of its own, so it is cut where it runs out.
      int glyphs = 0;
      wlen = chat_fit(game, size, text + cur, wlen, line_width - draw_x, &glyphs);
      new_line = true;
      // Fewer than four characters left behind is not worth a fragment.
      if (glyphs <= 3 && !got_newline_last) wlen = 0;
    } else if (draw_x + word_width > line_width && !got_newline_last) {
      new_line = true;
      wlen = 0;
    }

    if (wlen > 0) {
      draw_x += chat_measure(game, size, text + cur, wlen);
      cur += wlen;
    }

    if (new_line) {
      // TextEx bails when a break fails to move the cursor on, which is what
      // keeps a line width narrower than one character from spinning here.
      if (cur == last_break) break;
      last_break = cur;
      starts[rows] = (uint16_t)row_start;
      lengths[rows] = (uint16_t)(cur - row_start);
      ++rows;
      row_start = cur;
      draw_x = 0.f;
      got_newline_last = true;
    } else {
      got_newline_last = false;
    }
  }

  if (cur > row_start && rows < DD_CHAT_MAX_ROWS) {
    starts[rows] = (uint16_t)row_start;
    lengths[rows] = (uint16_t)(cur - row_start);
    ++rows;
  }
  return rows;
}

// CreateRectQuadContainer with MessageRounding() and CORNER_ALL, at DDNet's
// eight subdivisions per corner.
static void chat_fill_rect(ft_game *game, float x, float y, float w, float h, float radius, ft_color color) {
  const ft_vec2 top_left = game->engine->screen_to_world((ft_vec2){x, y});
  const ft_vec2 bottom_right = game->engine->screen_to_world((ft_vec2){x + w, y + h});
  const ft_vec2 size = {bottom_right.x - top_left.x, bottom_right.y - top_left.y};
  const ft_vec2 radius_world = game->engine->screen_to_world((ft_vec2){x + radius, y});
  game->engine->draw_rect_rounded(DD_Z_OVERLAYS, top_left, size, radius_world.x - top_left.x, color, 8u);
}

// The avatar CChat puts left of a player's name: an idle tee looking slightly
// down and to the right. It is centred in the first row -- RowHeight in
// CChat is one row, not the whole message -- so it always sits beside the name.
static void chat_draw_tee(ft_game *game, const ft_render_frame *frame, const chat_metrics_t *m, int client_id, float row_y,
                          float scale, float blend) {
  if (client_id < 0 || (uint32_t)client_id >= frame->player_setup_count) return;

  dd_player_profile_t profile;
  dd_profile_from_setup(&frame->player_setups[client_id], &profile);

  dd_anim_state_t anim;
  dd_anim_state_set(&anim, &anim_base, 0.f);
  dd_anim_state_add(&anim, &anim_idle, 0.f, 1.f);

  vec3 body, feet;
  dd_hsl_to_rgb(profile.color_body, body);
  dd_hsl_to_rgb(profile.color_feet, feet);

  const float centre_x = DD_CHAT_LEFT + (m->padding_x + m->tee_size) / 2.f;
  // TeeRenderPos, centred on the first row rather than the whole message --
  // CChat's RowHeight is one row. dd_skin_push already centres the tee on the
  // point it is given, so RenderTee's OffsetToMid has no counterpart here.
  const float centre_y = row_y + (m->font_size + m->padding_y) / 2.f;
  const ft_vec2 world = game->engine->screen_to_world((ft_vec2){centre_x * scale, centre_y * scale});
  // A skin scale of one draws the full 64px tee, so half the wanted size in
  // tiles is the scale that gives it.
  const float size_world = pixels_to_world(game, m->tee_size * scale);

  dd_skin_push_overlay(game, (vec2){world.x, world.y}, size_world * 0.5f, dd_gfx_skin_index(game, profile.skin), EYE_NORMAL,
                       (vec2){1.f, 0.1f}, &anim, body, feet, profile.use_custom_color != 0, blend);
}

static void render_chat(ft_game *game, const ft_render_frame *frame) {
  if (!game->settings.render_chat || !frame->active) return;
  visible_event messages[8];
  const int count = recent_events(game, frame, DD_EVENT_CHAT, DD_CHAT_LIFETIME, messages, 8);
  if (count <= 0) return;

  ft_camera camera;
  game->engine->camera_get(&camera);
  if (camera.viewport.y <= 0.f) return;
  // The whole layout is in virtual units; this is the only place they become
  // pixels, which is what keeps the chat the same size at any window height.
  const float scale = camera.viewport.y / DD_CHAT_VIRTUAL_HEIGHT;
  const chat_metrics_t m = chat_metrics(game);

  const float text_left = DD_CHAT_LEFT + m.padding_x / 2.f;
  // OnRender's starting y, then the step it takes when the input line is idle.
  float y = DD_CHAT_VIRTUAL_HEIGHT - 20.f * m.font_size / 6.f - m.font_size * (8.f / 6.f);

  // Newest first, walking upwards, exactly as CChat steps back through m_aLines.
  for (int i = count - 1; i >= 0; --i) {
    const dd_event_payload_t *chat = &messages[i].payload;
    const int age = frame->tick - messages[i].tick;
    float blend = age > DD_CHAT_FADE_START
                      ? 1.f - (float)(age - DD_CHAT_FADE_START) / (float)(2 * GAME_TICK_SPEED)
                      : 1.f;
    if (blend <= 0.f) continue;
    if (blend > 1.f) blend = 1.f;

    // cl_message_system_color, cl_message_team_color and cl_message_color, with
    // CalculateNameColor's darkened variant for a team name. A server message
    // carries "*** " where a player carries their name and a colon, and gets no
    // avatar, which is also what leaves its text hard against the padding.
    ft_color name_color, text_color;
    char prefix[64] = "";
    const bool from_player = chat->client_id >= 0;
    if (!from_player) {
      snprintf(prefix, sizeof(prefix), "*** ");
      name_color = text_color = (ft_color){1.000000f, 0.994095f, 0.498039f, blend};
    } else {
      char fallback[32];
      snprintf(prefix, sizeof(prefix), "%s: ", profile_name(frame, chat->client_id, fallback, sizeof(fallback)));
      if (chat->team > 0) {
        name_color = (ft_color){0.440659f, 0.893459f, 0.440659f, blend};
        text_color = (ft_color){0.647059f, 1.000000f, 0.647059f, blend};
      } else {
        name_color = (ft_color){0.8f, 0.8f, 0.8f, blend};
        text_color = (ft_color){1.f, 1.f, 1.f, blend};
      }
    }

    // dd_text_width scales linearly with the size it is given, so measuring at
    // the virtual font size answers in virtual units and no conversion is
    // needed. Converting here is what previously overlapped name and message.
    const float tee_gap = from_player ? m.tee_gap : 0.f;
    const float prefix_width = dd_text_width(game, m.font_size, prefix);
    const float message_left = text_left + tee_gap + prefix_width;
    const float message_width = m.width - m.padding_x * 1.5f - tee_gap - prefix_width;

    uint16_t starts[DD_CHAT_MAX_ROWS], lengths[DD_CHAT_MAX_ROWS];
    const int rows = chat_wrap(game, chat->message, m.font_size, message_width, starts, lengths);
    const float height = (float)(rows > 0 ? rows : 1) * m.font_size + m.padding_y;

    y -= height;
    if (y < DD_CHAT_HEIGHT_LIMIT) break;

    float widest = 0.f;
    for (int row = 0; row < rows; ++row) {
      char buffer[256];
      const int n = lengths[row] < sizeof(buffer) ? lengths[row] : (int)sizeof(buffer) - 1;
      memcpy(buffer, chat->message + starts[row], (size_t)n);
      buffer[n] = '\0';
      const float width = dd_text_width(game, m.font_size, buffer);
      if (width > widest) widest = width;
    }

    // cl_chat_background_color: black at four percent, behind the name and the
    // longest wrapped row together, which is CChat's FullWidth.
    chat_fill_rect(game, DD_CHAT_LEFT * scale, y * scale,
                   (m.padding_x * 1.5f + tee_gap + prefix_width + widest) * scale, height * scale, m.rounding * scale,
                   (ft_color){0.f, 0.f, 0.f, 0.047059f * blend});

    if (from_player) chat_draw_tee(game, frame, &m, chat->client_id, y, scale, blend);

    const float text_y = y + m.padding_y / 2.f;
    draw_text_screen(game, (text_left + tee_gap) * scale, text_y * scale, m.font_size * scale, name_color, prefix);
    for (int row = 0; row < rows; ++row) {
      char buffer[256];
      const int n = lengths[row] < sizeof(buffer) ? lengths[row] : (int)sizeof(buffer) - 1;
      memcpy(buffer, chat->message + starts[row], (size_t)n);
      buffer[n] = '\0';
      draw_text_screen(game, message_left * scale, (text_y + (float)row * m.font_size) * scale, m.font_size * scale,
                       text_color, buffer);
    }
  }
}

static void render_emoticons(ft_game *game, const ft_render_frame *frame) {
  if (!game->settings.render_emoticons || !game->gfx.emoticons || !frame->world) return;
  visible_event emotes[64];
  const int count = recent_events(game, frame, DD_EVENT_EMOTICON, 2 * GAME_TICK_SPEED, emotes, 64);
  const SWorldCore *world = &frame->world->core;
  for (int i = 0; i < count; ++i) {
    const dd_event_payload_t *event = &emotes[i].payload;
    if (event->client_id < 0 || event->client_id >= world->m_NumCharacters || event->emoticon < 0 ||
        event->emoticon >= DD_EMOTICON_COUNT)
      continue;
    const float since = (float)(frame->tick - emotes[i].tick) + frame->alpha;
    const float until_end = 2.f * GAME_TICK_SPEED - since;
    float alpha = until_end < GAME_TICK_SPEED / 5.f ? until_end / (GAME_TICK_SPEED / 5.f) : 1.f;
    float height = since < GAME_TICK_SPEED / 10.f ? since / (GAME_TICK_SPEED / 10.f) : 1.f;
    if (alpha <= 0.f || height <= 0.f) continue;
    float wiggle = since < GAME_TICK_SPEED / 5.f ? since / (GAME_TICK_SPEED / 5.f) : 0.f;
    ft_vec2 base = character_position(&world->m_pCharacters[event->client_id], frame->alpha);
    vec2 pos = {base.x, base.y - (23.f + 32.f * height) / PX_PER_TILE};
    vec2 size = {2.f, 2.f * height};
    dd_draw_sprite(game, game->gfx.emoticons, DD_Z_OVERLAYS, pos, size,
                   (float)(M_PI / 6.0) * sinf(5.f * wiggle), (uint32_t)event->emoticon,
                   (vec4){1.f, 1.f, 1.f, alpha * frame->opacity});
  }
}

void dd_render_world_overlays(ft_game *game, const ft_render_frame *frame) {
  render_nameplates(game, frame);
  render_freeze_bars(game, frame);
  render_emoticons(game, frame);
  render_chat(game, frame);
  dd_skins_flush_overlay(game);
}

// DDNet draws editor/speed_arrow.png as a single 35px quad centred on the tile
// and rotated by the tile's angle. The engine turns sprites the other way round
// its y-down world, so DDNet's clockwise degrees become a negated radian angle.
static void draw_speedup_arrow(ft_game *game, int x, int y, int angle_degrees) {
  vec2 center = {(float)x + 0.5f, (float)y + 0.5f};
  vec2 size = {35.f / PX_PER_TILE, 35.f / PX_PER_TILE};
  dd_draw_sprite(game, game->gfx.speedup_arrow, DD_Z_MAP_SPEEDUP, center, size,
                 -(float)angle_degrees * (float)(M_PI / 180.0), 0, (vec4){1.f, 1.f, 1.f, 1.f});
}

static void map_text(ft_game *game, float z, int x, int y, int style, int value) {
  // DDNet's entity labels are pre-rendered 64x64 tile sprites rather than
  // baseline-positioned runtime text. Using the same top/center/bottom atlases
  // makes both their size and their Y coordinate invariant under camera zoom.
  dd_entity_text_draw(game, z, style, x, y, value, (ft_color){1.f, 1.f, 1.f, 1.f});
}

void dd_render_map_overlays(ft_game *game, const ft_render_frame *frame) {
  if ((!game->settings.render_entity_text && !game->settings.render_speedups) || !frame->level) return;
  const map_data_t *map = &frame->level->collision.m_MapData;
  ft_camera camera;
  game->engine->camera_get(&camera);
  int x0 = (int)floorf(camera.visible.x) - 1;
  int y0 = (int)floorf(camera.visible.y) - 1;
  int x1 = (int)ceilf(camera.visible.x + camera.visible.w) + 1;
  int y1 = (int)ceilf(camera.visible.y + camera.visible.h) + 1;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > map->width) x1 = map->width;
  if (y1 > map->height) y1 = map->height;

  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const int index = y * map->width + x;
      if (map->speedup_layer.type && map->speedup_layer.type[index]) {
        const int type = map->speedup_layer.type[index];
        const int force = map->speedup_layer.force[index];
        const int max_speed = map->speedup_layer.max_speed[index];
        // Outside the editor DDNet only shows a speedup that would do
        // something: the old tile needs a force, the new one either value.
        if ((force && type == TILE_SPEED_BOOST_OLD) || ((force || max_speed) && type == TILE_SPEED_BOOST)) {
          if (game->settings.render_speedups) draw_speedup_arrow(game, x, y, map->speedup_layer.angle[index]);
          if (game->settings.render_entity_text) {
            map_text(game, DD_Z_MAP_SPEEDUP_TEXT, x, y, DD_ENTITY_TEXT_TOP, max_speed);
            map_text(game, DD_Z_MAP_SPEEDUP_TEXT, x, y, DD_ENTITY_TEXT_BOTTOM, force);
          }
        }
      }
      if (!game->settings.render_entity_text) continue;
      // CFRM tiles use the last activated checkpoint, not their own number.
      if (map->tele_layer.type && map->tele_layer.type[index] &&
          map->tele_layer.type[index] != TILE_TELECHECKIN && map->tele_layer.type[index] != TILE_TELECHECKINEVIL)
        map_text(game, DD_Z_MAP_TELE_TEXT, x, y, DD_ENTITY_TEXT_CENTER, map->tele_layer.number[index]);
      if (map->switch_layer.type && map->switch_layer.type[index]) {
        map_text(game, DD_Z_MAP_SWITCH_TEXT, x, y, DD_ENTITY_TEXT_TOP, map->switch_layer.number[index]);
        map_text(game, DD_Z_MAP_SWITCH_TEXT, x, y, DD_ENTITY_TEXT_BOTTOM, map->switch_layer.delay[index]);
      }
    }
  }
}

// CItems::RenderLaser for LASERTYPE_DOOR. The laser object a door snaps is
// flipped -- CDoor::Read reads its m_To back as the door's own position -- so
// RenderLaser's `Pos`, the end that carries the head, is the door's tile, and
// its `From` is the clipped far end of the beam.
static void draw_door(ft_game *game, vec2 pos, vec2 to, float opacity) {
  // cl_laser_door_outline_color and cl_laser_door_inner_color, converted from
  // the packed HSL those defaults are stored as.
  const vec4 outer = {0.000000f, 0.133333f, 0.097255f, opacity};
  const vec4 inner = {0.263914f, 0.763537f, 0.640101f, opacity};
  // A door enters RenderLaser with TicksBody pinned to 1 instead of an age, so
  // unlike a shot it never collapses -- but it never reaches the full 7 and 5
  // offsets either. One tick against the default bounce delay leaves it here.
  const float ia = 1.f - (1000.f / GAME_TICK_SPEED) / 150.f;

  vec2 dir;
  glm_vec2_sub(pos, to, dir);
  if (glm_vec2_norm(dir) > 0.f) {
    dd_draw_line(game, DD_Z_PROJECTILES, to, pos, (float *)outer, 14.f * ia / PX_PER_TILE);
    // ExtraOutlineFrom is zero for a door where a shot would inset both ends:
    // only the head end is pulled in, so consecutive doors meet flush and read
    // as one beam instead of showing a seam at every join.
    glm_vec2_normalize(dir);
    glm_vec2_scale(dir, 1.f / PX_PER_TILE, dir);
    vec2 inner_to = {pos[0] - dir[0], pos[1] - dir[1]};
    dd_draw_line(game, DD_Z_PROJECTILES + 0.01f, to, inner_to, (float *)inner, 10.f * ia / PX_PER_TILE);
  }

  // The head is the untextured 16x16 quad and its 12x12 inset, on the tile the
  // door itself sits on.
  game->engine->draw_rect(DD_Z_PROJECTILES + 0.02f,
                          (ft_vec2){pos[0] - 8.f / PX_PER_TILE, pos[1] - 8.f / PX_PER_TILE},
                          (ft_vec2){16.f / PX_PER_TILE, 16.f / PX_PER_TILE},
                          (ft_color){outer[0], outer[1], outer[2], outer[3]});
  game->engine->draw_rect(DD_Z_PROJECTILES + 0.03f,
                          (ft_vec2){pos[0] - 6.f / PX_PER_TILE, pos[1] - 6.f / PX_PER_TILE},
                          (ft_vec2){12.f / PX_PER_TILE, 12.f / PX_PER_TILE},
                          (ft_color){inner[0], inner[1], inner[2], inner[3]});
}

void dd_render_doors(ft_game *game, const ft_render_frame *frame) {
  if (!game->settings.render_doors || !frame->active || !frame->world || !frame->level) return;
  const SCollision *collision = &frame->level->collision;
  const SWorldCore *world = &frame->world->core;
  ft_camera camera;
  game->engine->camera_get(&camera);

  for (int i = 0; i < collision->m_NumDoors; ++i) {
    const SDoor *door = &collision->m_pDoors[i];
    // A switcher that is on holds its door shut, which is also the state
    // get_move_restrictions blocks movement in. Switcher 0 is always on.
    if (door->m_Number > 0 &&
        (!world->m_pSwitches || door->m_Number >= world->m_NumSwitches || !world->m_pSwitches[door->m_Number].m_Status))
      continue;

    vec2 from = {vgetx(door->m_Pos) / PX_PER_TILE, vgety(door->m_Pos) / PX_PER_TILE};
    vec2 to = {vgetx(door->m_To) / PX_PER_TILE, vgety(door->m_To) / PX_PER_TILE};
    if ((from[0] < camera.visible.x && to[0] < camera.visible.x) ||
        (from[0] > camera.visible.x + camera.visible.w && to[0] > camera.visible.x + camera.visible.w) ||
        (from[1] < camera.visible.y && to[1] < camera.visible.y) ||
        (from[1] > camera.visible.y + camera.visible.h && to[1] > camera.visible.y + camera.visible.h))
      continue;
    draw_door(game, from, to, frame->opacity);
  }
}
