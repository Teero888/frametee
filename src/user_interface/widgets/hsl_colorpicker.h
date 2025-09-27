#ifndef CIMGUI_HSL_COLORPICKER_H
#define CIMGUI_HSL_COLORPICKER_H

#include <stdbool.h>
#include <stdint.h>

bool PackedHSLPicker(const char *label, uint32_t *packed_hsl);
void packed_hsl_to_rgb(uint32_t packed, float rgb[3]);
#endif // CIMGUI_HSL_COLORPICKER_H
