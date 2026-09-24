// The demo library's reconstruction, compiled on its own: its internal names would clash with this
// module's, and it has to stay free of anything that changes float results (it replays DDNet's
// physics bit for bit). The module talks to it through ddnet_demo_state.h only; ddnet_demo.h's own
// implementation lives in dd_demo.c.
#include <ddnet_demo/ddnet_demo.h>
#define DDNET_DEMO_STATE_IMPLEMENTATION
#include <ddnet_demo/ddnet_demo_state.h>
