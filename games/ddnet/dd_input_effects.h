#ifndef DD_INPUT_EFFECTS_H
#define DD_INPUT_EFFECTS_H

#include "dd_internal.h"

uint32_t dd_input_effect_count(ft_game *game);
const ft_input_effect_desc *dd_input_effect_desc(ft_game *game, uint32_t index);
void dd_input_effect_default(ft_game *game, uint32_t index, void *parameters, uint32_t parameter_size);
bool dd_input_effect_apply(ft_game *game, uint32_t index, const ft_input_effect_frame *frame,
                           const void *parameters, uint32_t parameter_size, void *runtime,
                           uint32_t runtime_size, void *inout_records);
bool dd_input_effect_ui(ft_game *game, uint32_t index, const ft_input_effect_ui_frame *frame,
                        void *parameters, uint32_t parameter_size, const void *runtime,
                        uint32_t runtime_size);

#endif // DD_INPUT_EFFECTS_H
