#ifndef RAW_MOUSE_H
#define RAW_MOUSE_H

/* Raw mouse motion read on a thread of its own, for the platform where reading it on the main thread
 * is what slows the editor down: on Windows every report of the mouse is a WM_INPUT message, a mouse
 * polling at 1-8 kHz sends that many a second, and GLFW's raw motion reads them one at a time inside
 * glfwPollEvents (GLFW issue #2684). The reader takes them in batches off the main thread instead,
 * and the main thread collects the sum once a frame. Elsewhere there is no reader: GLFW's own raw
 * motion is used, and these do nothing. */

#include <stdbool.h>

/* Starts reading, if it is not already. False where there is no reader or it could not start; the
 * caller then keeps to GLFW's raw motion. */
bool raw_mouse_start(void);
void raw_mouse_stop(void);
bool raw_mouse_running(void);
/* The motion since the previous call (or the start), in the units GLFW's raw motion reports: the
 * mouse's own counts, before the system's pointer speed and acceleration. It is the motion of the
 * mouse whichever window has it. */
void raw_mouse_take(double *out_dx, double *out_dy);

#endif // RAW_MOUSE_H
