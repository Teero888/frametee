#ifndef TMUF_DATA_H
#define TMUF_DATA_H

#include <frametee/game_abi.h>

// Select the local data directory first, then a Steam installation. Called
// when checking the start screen and creating a game; Check again refreshes it.
bool tm_data_find(const ft_engine_api *engine);
// Packs and GameData use the selected installation. Module files and players'
// Documents still belong to FrameTee's data directory.
size_t tm_data_resolve_path(const ft_engine_api *engine, const char *relative, char *out, size_t out_size);

#endif
