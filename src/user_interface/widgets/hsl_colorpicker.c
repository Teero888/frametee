#include "imcol.h"
#include <math.h>
#include <stdint.h>
#include <system/include_cimgui.h>

#define DARKEST_LGT (0.5f)

static float clampf_f(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/*
static ImU32 rgbf_to_u32(float r, float g, float b) {
  int R = (int)roundf(clampf_f(r, 0.0f, 1.0f) * 255.0f);
  int G = (int)roundf(clampf_f(g, 0.0f, 1.0f) * 255.0f);
  int B = (int)roundf(clampf_f(b, 0.0f, 1.0f) * 255.0f);
  return IM_COL32(R, G, B, 255);
}
*/

/* HSL -> RGB (floats) using standard chroma method */
static void hsl_to_rgbf(float h, float s, float l, float *out_r, float *out_g, float *out_b) {
  /* normalize */
  h = h - floorf(h); /* wrap */
  s = clampf_f(s, 0.0f, 1.0f);
  l = clampf_f(l, 0.0f, 1.0f);

  float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
  float h1 = h * 6.0f;
  float x = c * (1.0f - fabsf(fmodf(h1, 2.0f) - 1.0f));

  float r0 = 0.0f, g0 = 0.0f, b0 = 0.0f;
  int sector = (int)floorf(h1);
  sector %= 6;
  if (sector < 0) sector += 6;

  switch (sector) {
  case 0:
    r0 = c;
    g0 = x;
    break;
  case 1:
    r0 = x;
    g0 = c;
    break;
  case 2:
    g0 = c;
    b0 = x;
    break;
  case 3:
    g0 = x;
    b0 = c;
    break;
  case 4:
    r0 = x;
    b0 = c;
    break;
  case 5:
    r0 = c;
    b0 = x;
    break;
  }

  float m = l - 0.5f * c;
  *out_r = clampf_f(r0 + m, 0.0f, 1.0f);
  *out_g = clampf_f(g0 + m, 0.0f, 1.0f);
  *out_b = clampf_f(b0 + m, 0.0f, 1.0f);
}

/* Packed HSL (0xHHSSLL) -> float rgb[3], each in [0,1] */
void packed_hsl_to_rgb(uint32_t packed, float rgb[3]) {
  uint8_t Hb = (packed >> 16) & 0xFFu;
  uint8_t Sb = (packed >> 8) & 0xFFu;
  uint8_t Lb = (packed >> 0) & 0xFFu;

  float h = (float)Hb / 255.0f;
  float s = (float)Sb / 255.0f;
  float l_unclamped = (float)Lb / 255.0f;

  /* ddnet: l' = DARKEST + l * (1 - DARKEST) */
  float l = DARKEST_LGT + l_unclamped * (1.0f - DARKEST_LGT);

  float r, g, b;
  hsl_to_rgbf(h, s, l, &r, &g, &b);
  rgb[0] = r;
  rgb[1] = g;
  rgb[2] = b;
}
/* Convert packed HSL (0xHHSSLL) to packed RGB24 (0xRRGGBB) applying darkest remap */
/*
static uint32_t packed_hsl_to_rgb24(uint32_t packed_hsl, float darkest) {
  uint8_t Hb = (packed_hsl >> 16) & 0xFFu;
  uint8_t Sb = (packed_hsl >> 8) & 0xFFu;
  uint8_t Lb = (packed_hsl >> 0) & 0xFFu;

  float h = (float)Hb / 255.0f;
  float s = (float)Sb / 255.0f;
  float l_packed = (float)Lb / 255.0f;

  float l = darkest + l_packed * (1.0f - darkest);

  float r, g, b;
  hsl_to_rgbf(h, s, l, &r, &g, &b);

  uint32_t R = (uint32_t)roundf(clampf_f(r, 0.0f, 1.0f) * 255.0f) & 0xFFu;
  uint32_t G = (uint32_t)roundf(clampf_f(g, 0.0f, 1.0f) * 255.0f) & 0xFFu;
  uint32_t B = (uint32_t)roundf(clampf_f(b, 0.0f, 1.0f) * 255.0f) & 0xFFu;

  return (R << 16) | (G << 8) | B;
}
*/
static uint32_t hsl_to_packed(float h, float s, float l) {
  float l_back = (l - DARKEST_LGT) / (1 - DARKEST_LGT);
  l_back = clampf_f(l_back, 0, 1);
  uint32_t Hb = (uint32_t)roundf(clampf_f(h, 0, 1) * 255), Sb = (uint32_t)roundf(clampf_f(s, 0, 1) * 255), Lb = (uint32_t)roundf(l_back * 255);
  return (Hb << 16) | (Sb << 8) | Lb;
}

bool PackedHSLPicker(const char *label, uint32_t *packed_hsl) {
  if (!packed_hsl) return false;
  bool changed = false;

  igPushID_Str(label);

  ImVec2 avail;
  igGetContentRegionAvail(&avail);
  float hue_px = 12.0f;
  float sv_px = avail.x * 0.5 - (hue_px + 12.0f);

  ImVec2 pos;
  igGetCursorScreenPos(&pos);
  ImVec2 hue_pos = (ImVec2){pos.x + sv_px + 8, pos.y};
  uint32_t in = *packed_hsl;
  float h = (float)((in >> 16) & 0xFF) / 255.0f;
  float s = (float)((in >> 8) & 0xFF) / 255.0f;
  float l_p = (float)((in >> 0) & 0xFF) / 255.0f;
  float l = DARKEST_LGT + l_p * (1.0f - DARKEST_LGT);

  ImDrawList *dl = igGetWindowDrawList();

  int iw = (int)sv_px, ih = (int)sv_px;
  if (iw < 2) iw = 2;
  if (ih < 2) ih = 2;

  for (int x = 0; x < iw; x++) {
    float sx = (float)x / (iw - 1);
    float rt, gt, bt, rb, gb, bb;
    hsl_to_rgbf(h, sx, DARKEST_LGT, &rt, &gt, &bt);
    hsl_to_rgbf(h, sx, 1.0f, &rb, &gb, &bb);
    ImVec2 p0 = {pos.x + (float)x, pos.y};
    ImVec2 p1 = {pos.x + (float)x + 1, pos.y + sv_px};
    ImU32 col_t = IM_COL32((int)(rt * 255), (int)(gt * 255), (int)(bt * 255), 255);
    ImU32 col_b = IM_COL32((int)(rb * 255), (int)(gb * 255), (int)(bb * 255), 255);
    ImDrawList_AddRectFilledMultiColor(dl, p0, p1, col_t, col_t, col_b, col_b);
  }

  igSetCursorScreenPos(pos);
  igInvisibleButton("##sv", (ImVec2){sv_px, sv_px}, 0);
  if (igIsItemActive()) {
    ImVec2 mp = igGetIO_Nil()->MousePos;
    float nx = (mp.x - pos.x) / sv_px;
    float ny = (mp.y - pos.y) / sv_px;
    nx = clampf_f(nx, 0, 1);
    ny = clampf_f(ny, 0, 1);
    s = nx;
    l = DARKEST_LGT + ny * (1.0f - DARKEST_LGT);
    changed = true;
  }

  float hue_width = 12.0f;
  for (int y = 0; y < ih; y++) {
    float hy = (float)y / (ih - 1);
    float hh = 1.0f - hy;
    float rr, gg, bb;
    hsl_to_rgbf(hh, 1.0f, 0.5f, &rr, &gg, &bb);

    ImVec2 p0 = {hue_pos.x, hue_pos.y + (float)y};
    ImVec2 p1 = {hue_pos.x + hue_width, hue_pos.y + (float)y + 1};

    ImU32 col = IM_COL32((int)(rr * 255), (int)(gg * 255), (int)(bb * 255), 255);
    ImDrawList_AddRectFilled(dl, p0, p1, col, 0, 0);
  }
  igSetCursorScreenPos(hue_pos);
  igInvisibleButton("##hue", (ImVec2){hue_px, sv_px}, 0);
  if (igIsItemActive()) {
    ImVec2 mp = igGetIO_Nil()->MousePos;
    float hy = (mp.y - hue_pos.y) / sv_px;
    hy = clampf_f(hy, 0, 1);
    h = 1.0f - hy;
    changed = true;
  }

  igSetCursorScreenPos((ImVec2){pos.x, pos.y + sv_px + 6});

  igText("HSL values:");
  float h_deg = h * 360.0f;
  float s_pct = s * 100.0f;
  float l_pct = ((l - DARKEST_LGT) / (1.0f - DARKEST_LGT)) * 100.0f;

  if (igDragFloat("##Hue", &h_deg, 1.0f, 0.0f, 360.0f, "Hue: %.0f", 0)) {
    h = h_deg / 360.0f;
    changed = true;
  }
  if (igDragFloat("##Sat", &s_pct, 1.0f, 0.0f, 100.0f, "Sat: %.0f%%", 0)) {
    s = s_pct / 100.0f;
    changed = true;
  }
  if (igDragFloat("##Light", &l_pct, 1.0f, 0.0f, 100.0f, "Light: %.0f%%", 0)) {
    l = DARKEST_LGT + (l_pct / 100.0f) * (1.0f - DARKEST_LGT);
    changed = true;
  }

  int packed_int = (int)(*packed_hsl);
  if (igInputInt("##Packed", &packed_int, 1, 100, 0)) {
    *packed_hsl = (uint32_t)packed_int;
    igPopID();
    return true;
  }

  *packed_hsl = hsl_to_packed(h, s, l);

  igPopID();

  return changed;
}
