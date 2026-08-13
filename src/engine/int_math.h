#ifndef ENGINE_INT_MATH_H
#define ENGINE_INT_MATH_H

// Integer helpers the timeline uses throughout.
//
// Engine-internal on purpose: they used to arrive through a game's physics
// header, and putting them in a header plugins see would collide with the very
// game headers a game-specific plugin includes.

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

#endif // ENGINE_INT_MATH_H
