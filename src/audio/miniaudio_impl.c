// miniaudio's implementation, with stb_vorbis so it decodes Ogg Vorbis too
// (TrackMania's ambiences and music are .ogg).
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
