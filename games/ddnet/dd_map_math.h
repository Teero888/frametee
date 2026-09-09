#ifndef DD_MAP_MATH_H
#define DD_MAP_MATH_H
#include "ddnet_map_loader.h"
/* Leaves default channels unchanged for absent/invalid envelopes. */
void dd_map_envelope(const map_data_t *map, int index, double time_ms, int channels, float result[4]);
/* Bounds are in frametee tile units: left, top, right, bottom. */
void dd_map_group_view(const map_group_t *group, const float camera[4], float view[4]);
#endif
