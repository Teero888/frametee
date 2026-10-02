/*
 * refshot.dll: the real game as the reference renderer for the tmuf module.
 *
 * Injected into TmForever.exe (TMUF 2.11.26) by tmuf_physics'
 * tools/oracle/launcher.exe. Hooks CHmsCamera::Run, which every camera of the
 * scene runs once per rendered frame, and:
 *
 *   TMUF_REFSHOT_LOG=path   appends one line per call: "C frame camera
 *                           location[12] frustum[8]" (location: CHmsCamera
 *                           +0x58, rotation rows then translation; frustum:
 *                           +0x118..+0x134, int flag then 6 floats and the
 *                           fov mode)
 *   TMUF_REFSHOT_POSE=path  poses, one per line: "x0 x1 x2 y0 y1 y2 z0 z1 z2
 *                           tx ty tz fov" (the game's location layout: rows of
 *                           the rotation whose columns are the camera's left,
 *                           up and forward axes, then the eye; fov vertical in
 *                           degrees, 0 to keep the game's). The camera being
 *                           run holds pose i for TMUF_REFSHOT_HOLD (default
 *                           30) of its frames, after skipping
 *                           TMUF_REFSHOT_SKIP (default 120) frames, and the
 *                           back buffer of the last frame of each hold goes to
 *                           TMUF_REFSHOT_FRAMES/view_III.ppm
 *
 *   TMUF_REFSHOT_FRAMES=dir writes every TMUF_REFSHOT_EVERY-th (default 1)
 *                           presented frame to dir/frame_NNNNNN.ppm (the back
 *                           buffer, read back from the device), and always
 *                           dir/latest.ppm every 20 frames; no window needs to
 *                           be visible, so the game can render on a GPU
 *                           through DXVK while the X display is Xvfb
 *
 *   TMUF_REFSHOT_TRACE=dir  traces the frame of view TMUF_REFSHOT_TRACE_VIEW
 *                           (default 0): dir/draws.txt, one block per draw
 *                           call (shaders, textures, render and sampler
 *                           states, the shader constants), dir/vs_N.asm and
 *                           ps_N.asm (the shaders, disassembled by the game's
 *                           own D3DX), dir/tex_N.dds (the textures); each
 *                           draw names its render target ("rt surf S tex T":
 *                           T is the texture id when other draws sample it)
 *                           and vertex declaration, and every render target
 *                           is saved when the game leaves it
 *                           (dir/rt_DRAW_sS.bmp|dds, DRAW: its last draw).
 *                           TMUF_REFSHOT_TRACE_STEPS=1 also saves the render
 *                           target after every draw (dir/step_DRAW.bmp|dds);
 *                           TMUF_REFSHOT_TRACE_VB=1 each draw's
 *                           vertices (positions, indices, texcoord 0, colour)
 *                           (dir/vb_DRAW.bin, dump_vb);
 *                           dir/done.txt marks the end of a trace
 *
 *   TMUF_REFSHOT_TRACE_LOAD=dir traces the map's lightmap computation
 *                           (CHmsPackLightMap::ComputeLighting, at load) the
 *                           same way, from its first call until the scene's
 *                           camera first runs; TMUF_REFSHOT_TRACE_RT_MAX caps
 *                           the render targets it saves
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dx9shader.h>
#include <d3dx9tex.h>

enum { ST_RET = 9, ST_ARG0 = 10 };

typedef struct hook {
  const char *name;
  uintptr_t address;
  uint8_t expected[16];
  int stolen;
  void (*handler)(void *self, const uint32_t *stack);
} hook;

static FILE *g_log;
#define MAX_POSES 256
static uint32_t g_pose_count;
static float g_poses[MAX_POSES][13];
static uint32_t g_hold = 30, g_skip = 120;
static void *g_camera;          /* the camera run last */
static uint32_t g_camera_frames; /* its runs so far */
static uint32_t g_frame;
static int g_view_due = -1; /* the pose whose view the next present saves */
static char g_trace_dir[MAX_PATH];
static int g_trace_view, g_tracing, g_traced;
static char g_trace_load_dir[MAX_PATH];
static int g_trace_load; /* 1: waiting for the lightmap computation, 2: tracing it, 3: done */
static uint32_t g_rt_saves, g_rt_save_max = 0xffffffffu;
static char g_view_trace_dir[MAX_PATH]; /* the view trace's, while the lightmaps' goes on */
static int g_in_lighting;                /* ComputeLighting calls under way */
static uint32_t g_lighting_present;      /* the present of the last draw it made */
static uint32_t g_lighting_returns[8];   /* where they return to */
static void finish_load_trace(void);
/* what the draws are recorded for: a view's frame, or the lightmap
   computation's own draws */
static int trace_active(void) { return g_tracing && (g_trace_load != 2 || g_in_lighting > 0); }
static uint32_t g_cur_rt_draws;
static void trace_reset_dumps(void);
static void trace_done(void);
static void put_rel32(uint8_t *at, uintptr_t next, uintptr_t target);
static FILE *g_draws;
static uint32_t g_draw_index;
static void trace_hook_device(IDirect3DDevice9 *dev);
static void leave_rt(IDirect3DDevice9 *dev);

static void log_msg(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  OutputDebugStringA(buf);
  if (g_log) {
    fprintf(g_log, "# %s\n", buf);
    fflush(g_log);
  }
}

/* CHmsCamera */
#define CAM_PREVIOUS 0x18
#define CAM_LOCATION 0x58
#define CAM_FRUSTUM 0x118
#define CAM_FOV_MODE 0x134

static void on_camera_run(void *camera, const uint32_t *stack) {
  (void)stack;
  uint8_t *c = camera;
  if (camera != g_camera) {
    g_camera = camera;
    g_camera_frames = 0;
  }
  g_frame++;
  const uint32_t n = g_camera_frames++;
  /* the scene's camera (near plane 0.2 m), not the menus' */
  const float near_z = ((const float *)(c + CAM_FRUSTUM + 4))[2];
  if (g_pose_count && near_z < 1.0f && n >= g_skip && (n - g_skip) / g_hold < g_pose_count) {
    const uint32_t i = (n - g_skip) / g_hold;
    const float *pose = g_poses[i];
    memcpy(c + CAM_LOCATION, pose, 12 * sizeof(float));
    memcpy(c + CAM_PREVIOUS, pose, 12 * sizeof(float)); /* no motion blend */
    if (pose[12] > 0.0f) {
      /* GmFrustum: x min, y min, near, x max, y max, far at unit distance */
      float *f = (float *)(c + CAM_FRUSTUM + 4);
      const float aspect = f[4] != 0.0f ? f[3] / f[4] : 4.0f / 3.0f;
      const float t = tanf(pose[12] * 3.14159265f / 360.0f);
      f[1] = -t, f[4] = t;
      f[0] = -t * aspect, f[3] = t * aspect;
    }
    if ((n - g_skip) % g_hold == g_hold - 1) {
      g_view_due = (int)i;
      if (g_trace_load == 2 && !g_traced && (int)i == g_trace_view) {
        log_msg("[refshot] view %d not traced: the lightmap trace is still on", g_trace_view);
        g_traced = 1;
      } else if (g_trace_dir[0] && !g_traced && (int)i == g_trace_view) {
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%s\\draws.txt", g_trace_dir);
        g_draws = fopen(path, "w");
        g_tracing = 1;
        g_traced = 1;
        log_msg("[refshot] tracing view %d", g_trace_view);
      }
    }
  }
  if (g_log) {
    const float *loc = (const float *)(c + CAM_LOCATION);
    fprintf(g_log, "C %u %08x", g_frame, (unsigned)(uintptr_t)camera);
    for (int i = 0; i < 12; i++)
      fprintf(g_log, " %.6g", loc[i]);
    fprintf(g_log, " | %d", *(const int *)(c + CAM_FRUSTUM));
    const float *f = (const float *)(c + CAM_FRUSTUM + 4);
    for (int i = 0; i < 6; i++)
      fprintf(g_log, " %.6g", f[i]);
    fprintf(g_log, " %d\n", *(const int *)(c + CAM_FOV_MODE));
    fflush(g_log);
  }
}

/* ---- frames: IDirect3DDevice9::Present ---- */

static char g_frames_dir[MAX_PATH];
static uint32_t g_every = 1, g_presented;
typedef HRESULT(WINAPI *present_fn)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static present_fn g_present;
typedef HRESULT(WINAPI *create_device_fn)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                         IDirect3DDevice9 **);
static create_device_fn g_create_device;
typedef IDirect3D9 *(WINAPI *create9_fn)(UINT);
static create9_fn g_create9;

static void save_ppm(const char *path, const uint8_t *bits, int pitch, UINT w, UINT h) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  uint8_t *row = malloc((size_t)w * 3);
  for (UINT y = 0; row && y < h; y++) {
    const uint8_t *s = bits + (size_t)y * (size_t)pitch;
    for (UINT x = 0; x < w; x++) {
      row[x * 3 + 0] = s[x * 4 + 2];
      row[x * 3 + 1] = s[x * 4 + 1];
      row[x * 3 + 2] = s[x * 4 + 0];
    }
    fwrite(row, 3, w, f);
  }
  free(row);
  fclose(f);
}

/* a surface's contents in system memory (a multisampled one resolved first) */
static IDirect3DSurface9 *read_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, D3DSURFACE_DESC *d) {
  if (FAILED(IDirect3DSurface9_GetDesc(surf, d)))
    return NULL;
  IDirect3DSurface9 *resolved = NULL, *copy = NULL;
  if (d->MultiSampleType != D3DMULTISAMPLE_NONE) {
    if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, d->Width, d->Height, d->Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                   &resolved, NULL)))
      return NULL;
    if (FAILED(IDirect3DDevice9_StretchRect(dev, surf, NULL, resolved, NULL, D3DTEXF_NONE))) {
      IDirect3DSurface9_Release(resolved);
      return NULL;
    }
    surf = resolved;
  }
  if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d->Width, d->Height, d->Format, D3DPOOL_SYSTEMMEM,
                                                          &copy, NULL)) ||
      FAILED(IDirect3DDevice9_GetRenderTargetData(dev, surf, copy))) {
    if (copy)
      IDirect3DSurface9_Release(copy);
    copy = NULL;
  }
  if (resolved)
    IDirect3DSurface9_Release(resolved);
  return copy;
}

static void capture(IDirect3DDevice9 *dev, const char *path) {
  IDirect3DSurface9 *back = NULL, *copy = NULL;
  if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
    return;
  D3DSURFACE_DESC d;
  if ((copy = read_surface(dev, back, &d))) {
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DSurface9_LockRect(copy, &lr, NULL, D3DLOCK_READONLY))) {
      if (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8)
        save_ppm(path, lr.pBits, lr.Pitch, d.Width, d.Height);
      IDirect3DSurface9_UnlockRect(copy);
    }
  }
  if (copy)
    IDirect3DSurface9_Release(copy);
  IDirect3DSurface9_Release(back);
}

static void trace_done(void) {
  char path[MAX_PATH + 32];
  snprintf(path, sizeof path, "%s\\done.txt", g_trace_dir);
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "%u draws\n", g_draw_index);
    fclose(f);
  }
}

static void on_frame(IDirect3DDevice9 *dev) {
  g_presented++;
  if (g_trace_load == 2) {
    /* done once it has drawn nothing for a while */
    if (g_draw_index > 0 && g_presented - g_lighting_present > 100)
      finish_load_trace();
  } else if (g_tracing && g_draws)
    fprintf(g_draws, "present before %u\n", g_draw_index);
  if (g_tracing && g_trace_load != 2) {
    leave_rt(dev);
    trace_done();
    g_tracing = 0;
    if (g_draws)
      fclose(g_draws);
    g_draws = NULL;
    log_msg("[refshot] trace done: %u draws", g_draw_index);
  }
  if (g_frames_dir[0]) {
    char path[MAX_PATH + 32];
    if (g_view_due >= 0) {
      snprintf(path, sizeof path, "%s\\view_%03d.ppm", g_frames_dir, g_view_due);
      capture(dev, path);
      log_msg("[refshot] view %d at frame %u", g_view_due, g_frame);
      g_view_due = -1;
    }
    if (g_every && g_presented % g_every == 0) {
      snprintf(path, sizeof path, "%s\\frame_%06u.ppm", g_frames_dir, g_presented);
      capture(dev, path);
    }
    if (g_presented % 20 == 0) {
      char tmp[MAX_PATH + 32];
      snprintf(tmp, sizeof tmp, "%s\\latest.tmp", g_frames_dir);
      snprintf(path, sizeof path, "%s\\latest.ppm", g_frames_dir);
      capture(dev, tmp);
      MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
    }
  }
  if (g_log && g_presented % 100 == 0) {
    fprintf(g_log, "P %u %u\n", g_presented, g_frame);
    fflush(g_log);
  }
}

static HRESULT WINAPI my_present(IDirect3DDevice9 *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r) {
  on_frame(dev);
  return g_present(dev, a, b, w, r);
}

static void patch_slot(void **vtable, int slot, void *fn, void **old);

/* IDirect3DSwapChain9::Present: the frame of the swap chain's device */
typedef HRESULT(WINAPI *sc_present_fn)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *,
                                       DWORD);
static sc_present_fn g_sc_present;
static IDirect3DDevice9 *g_device;

static void on_frame(IDirect3DDevice9 *dev);

static HRESULT WINAPI my_sc_present(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b, HWND w, const RGNDATA *r,
                                    DWORD flags) {
  IDirect3DDevice9 *dev = NULL;
  if (SUCCEEDED(IDirect3DSwapChain9_GetDevice(sc, &dev)) && dev) {
    on_frame(dev);
    IDirect3DDevice9_Release(dev);
  }
  return g_sc_present(sc, a, b, w, r, flags);
}

typedef HRESULT(WINAPI *create_sc_fn)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **);
static create_sc_fn g_create_sc;

static HRESULT WINAPI my_create_sc(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **out) {
  HRESULT hr = g_create_sc(dev, pp, out);
  if (SUCCEEDED(hr) && *out && !g_sc_present) {
    patch_slot(*(void ***)*out, 3, (void *)my_sc_present, (void **)&g_sc_present);
    log_msg("[refshot] additional swap chain %ux%u, Present hooked", pp->BackBufferWidth, pp->BackBufferHeight);
  }
  return hr;
}

static void patch_slot(void **vtable, int slot, void *fn, void **old) {
  DWORD prot;
  VirtualProtect(&vtable[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &prot);
  if (old)
    *old = vtable[slot];
  vtable[slot] = fn;
  VirtualProtect(&vtable[slot], sizeof(void *), prot, &prot);
}

/* ---- 2x multisampling, for the sea's reflection ----
   The game gives a render texture that asks for multisampling (the sea's
   reflection, CPlugBitmap+0x54 bits 4-7) only a format that supports 2
   samples (CDx9DeviceCaps::FormatCheck); without one the reflection is
   never made. Wine on Xvfb (llvmpipe) supports 4 and 8 samples, not 2:
   report 2 as supported wherever 4 are, and make 4-sample surfaces when 2
   are asked (sea_reflection_spec.md). */
typedef HRESULT(WINAPI *cms_fn)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE, DWORD *);
static cms_fn g_cms;
static HRESULT WINAPI my_cms(IDirect3D9 *d, UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD *q) {
  HRESULT hr = g_cms(d, a, t, f, w, m, q);
  if (FAILED(hr) && m == D3DMULTISAMPLE_2_SAMPLES) {
    DWORD q4 = 0;
    if (SUCCEEDED(g_cms(d, a, t, f, w, D3DMULTISAMPLE_4_SAMPLES, &q4))) {
      if (q) *q = 1;
      return D3D_OK;
    }
  }
  return hr;
}
typedef HRESULT(WINAPI *crt_fn)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                IDirect3DSurface9 **, HANDLE *);
static crt_fn g_crt, g_cds;
static HRESULT WINAPI my_crt(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk,
                             IDirect3DSurface9 **s, HANDLE *sh) {
  HRESULT hr = g_crt(d, w, h, f, m, q, lk, s, sh);
  if (FAILED(hr) && m == D3DMULTISAMPLE_2_SAMPLES) hr = g_crt(d, w, h, f, D3DMULTISAMPLE_4_SAMPLES, 0, lk, s, sh);
  return hr;
}
static HRESULT WINAPI my_cds(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk,
                             IDirect3DSurface9 **s, HANDLE *sh) {
  HRESULT hr = g_cds(d, w, h, f, m, q, lk, s, sh);
  if (FAILED(hr) && m == D3DMULTISAMPLE_2_SAMPLES) hr = g_cds(d, w, h, f, D3DMULTISAMPLE_4_SAMPLES, 0, lk, s, sh);
  return hr;
}

static HRESULT WINAPI my_create_device(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND win, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
  HRESULT hr = g_create_device(d3d, adapter, type, win, flags, pp, out);
  if (SUCCEEDED(hr) && *out && !g_present) {
    patch_slot(*(void ***)*out, 17, (void *)my_present, (void **)&g_present);
    patch_slot(*(void ***)*out, 13, (void *)my_create_sc, (void **)&g_create_sc);
    patch_slot(*(void ***)*out, 28, (void *)my_crt, (void **)&g_crt);
    patch_slot(*(void ***)*out, 29, (void *)my_cds, (void **)&g_cds);
    IDirect3DSwapChain9 *sc = NULL;
    if (SUCCEEDED(IDirect3DDevice9_GetSwapChain(*out, 0, &sc)) && sc) {
      patch_slot(*(void ***)sc, 3, (void *)my_sc_present, (void **)&g_sc_present);
      IDirect3DSwapChain9_Release(sc);
    }
    g_device = *out;
    if (g_trace_dir[0] || g_trace_load)
      trace_hook_device(*out);
    log_msg("[refshot] device %ux%u, Present hooked", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0);
  }
  return hr;
}

static IDirect3D9 *WINAPI my_create9(UINT version) {
  IDirect3D9 *d3d = g_create9(version);
  if (d3d && !g_create_device) {
    patch_slot(*(void ***)d3d, 16, (void *)my_create_device, (void **)&g_create_device);
    patch_slot(*(void ***)d3d, 11, (void *)my_cms, (void **)&g_cms);
  }
  return d3d;
}

/* the main module's import of d3d9!Direct3DCreate9 */
static void hook_import(void) {
  uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
    if (lstrcmpiA((const char *)(base + imp->Name), "d3d9.dll") != 0)
      continue;
    IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
    IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; names++, iat++) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG)
        continue;
      IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
      if (strcmp((const char *)n->Name, "Direct3DCreate9") != 0)
        continue;
      patch_slot((void **)&iat->u1.Function, 0, (void *)my_create9, (void **)&g_create9);
      log_msg("[refshot] Direct3DCreate9 hooked");
    }
  }
}


/* ---- frame trace ---- */

#define MAX_SHADERS 4096
#define MAX_TEXTURES 16384
typedef HRESULT(WINAPI *disasm_fn)(const DWORD *, BOOL, const char *, ID3DXBuffer **);
typedef HRESULT(WINAPI *save_tex_fn)(const char *, D3DXIMAGE_FILEFORMAT, IDirect3DBaseTexture9 *,
                                     const PALETTEENTRY *);
static disasm_fn g_disasm;
static save_tex_fn g_save_tex;
typedef HRESULT(WINAPI *save_surf_fn)(const char *, D3DXIMAGE_FILEFORMAT, IDirect3DSurface9 *, const PALETTEENTRY *,
                                      const RECT *);
static save_surf_fn g_save_surf;
static int g_trace_steps;
/* render target surfaces, by object; the one draws go to and its draws so far */
static void *g_surf_obj[MAX_TEXTURES];
static uint32_t g_surf_count;
static IDirect3DSurface9 *g_cur_rt;
/* shader and texture ids, by object */
static void *g_vs_obj[MAX_SHADERS], *g_ps_obj[MAX_SHADERS];
static uint32_t g_vs_count, g_ps_count;
static uint8_t g_vs_dumped[MAX_SHADERS], g_ps_dumped[MAX_SHADERS];
static void *g_tex_obj[MAX_TEXTURES];
static uint8_t g_tex_dumped[MAX_TEXTURES];
static uint32_t g_tex_count;
/* the device's state as the game set it */
static IDirect3DVertexShader9 *g_cur_vs;
static IDirect3DPixelShader9 *g_cur_ps;
static IDirect3DBaseTexture9 *g_cur_tex[16];
static float g_vs_const[256][4], g_ps_const[224][4];
static DWORD g_rs[256];
static DWORD g_tss[8][33]; /* texture stage states (the fixed-function pixel pipeline) */
static DWORD g_ss[16][14];

static void trace_reset_dumps(void) {
  memset(g_vs_dumped, 0, sizeof g_vs_dumped);
  memset(g_ps_dumped, 0, sizeof g_ps_dumped);
  memset(g_tex_dumped, 0, sizeof g_tex_dumped);
}

static uint32_t object_id(void **table, uint32_t *count, uint32_t cap, void *obj) {
  for (uint32_t i = 0; i < *count; i++)
    if (table[i] == obj)
      return i;
  if (*count >= cap)
    return cap - 1;
  table[*count] = obj;
  return (*count)++;
}

static void dump_shader(const char *prefix, uint32_t id, const DWORD *code) {
  if (!g_disasm || !code)
    return;
  ID3DXBuffer *buf = NULL;
  if (FAILED(g_disasm(code, FALSE, NULL, &buf)) || !buf)
    return;
  char path[MAX_PATH + 32];
  snprintf(path, sizeof path, "%s\\%s_%u.asm", g_trace_dir, prefix, id);
  FILE *f = fopen(path, "w");
  if (f) {
    fputs((const char *)buf->lpVtbl->GetBufferPointer(buf), f);
    fclose(f);
  }
  buf->lpVtbl->Release(buf);
}

static const DWORD *shader_code(IUnknown *shader, int pixel, DWORD **owned) {
  UINT size = 0;
  *owned = NULL;
  if (!shader)
    return NULL;
  HRESULT hr = pixel ? IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)shader, NULL, &size)
                     : IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)shader, NULL, &size);
  if (FAILED(hr) || !size)
    return NULL;
  *owned = malloc(size);
  if (!*owned)
    return NULL;
  hr = pixel ? IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)shader, *owned, &size)
             : IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)shader, *owned, &size);
  return SUCCEEDED(hr) ? *owned : NULL;
}

/* a render target's contents */
static void save_rt(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt, const char *name) {
  D3DSURFACE_DESC d;
  if (g_trace_load == 2 && g_rt_saves++ >= g_rt_save_max)
    return;
  if (!g_save_surf)
    return;
  IDirect3DSurface9 *copy = read_surface(dev, rt, &d);
  if (copy) {
    /* BMP (fast, no alpha) when 8 bits per channel, else DDS */
    const int bmp = d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\%s.%s", g_trace_dir, name, bmp ? "bmp" : "dds");
    g_save_surf(path, bmp ? D3DXIFF_BMP : D3DXIFF_DDS, copy, NULL, NULL);
  }
  if (copy)
    IDirect3DSurface9_Release(copy);
}

static uint32_t surface_id(IDirect3DSurface9 *s) { return object_id(g_surf_obj, &g_surf_count, MAX_TEXTURES, s); }

/* leaving the render target: keep what was drawn into it */
static void leave_rt(IDirect3DDevice9 *dev) {
  if (trace_active() && g_cur_rt && g_cur_rt_draws) {
    char name[64];
    snprintf(name, sizeof name, "rt_%u_s%u", g_draw_index - 1, surface_id(g_cur_rt));
    save_rt(dev, g_cur_rt, name);
  }
  g_cur_rt_draws = 0;
}

static void trace_draw(IDirect3DDevice9 *dev, const char *kind, UINT prims) {
  if (!trace_active() || !g_draws)
    return;
  g_lighting_present = g_presented;
  const uint32_t vs = g_cur_vs ? object_id(g_vs_obj, &g_vs_count, MAX_SHADERS, g_cur_vs) : 0xffffffffu;
  const uint32_t ps = g_cur_ps ? object_id(g_ps_obj, &g_ps_count, MAX_SHADERS, g_cur_ps) : 0xffffffffu;
  DWORD *owned;
  if (vs != 0xffffffffu && !g_vs_dumped[vs]) {
    dump_shader("vs", vs, shader_code((IUnknown *)g_cur_vs, 0, &owned));
    free(owned);
    g_vs_dumped[vs] = 1;
  }
  if (ps != 0xffffffffu && !g_ps_dumped[ps]) {
    dump_shader("ps", ps, shader_code((IUnknown *)g_cur_ps, 1, &owned));
    free(owned);
    g_ps_dumped[ps] = 1;
  }
  fprintf(g_draws, "draw %u %s prims %u vs %d ps %d\n", g_draw_index++, kind, prims, (int)vs, (int)ps);
  IDirect3DSurface9 *rt = NULL;
  if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)) && rt) {
    if (rt != g_cur_rt) {
      leave_rt(dev);
      g_cur_rt = rt;
    }
    g_cur_rt_draws++;
    D3DSURFACE_DESC d;
    memset(&d, 0, sizeof d);
    IDirect3DSurface9_GetDesc(rt, &d);
    void *container = NULL;
    int tex = -1;
    if (SUCCEEDED(IDirect3DSurface9_GetContainer(rt, &IID_IDirect3DBaseTexture9, &container)) && container) {
      tex = (int)object_id(g_tex_obj, &g_tex_count, MAX_TEXTURES, container);
      g_tex_dumped[tex] = 1;
      IUnknown_Release((IUnknown *)container);
    }
    fprintf(g_draws, "  rt surf %u tex %d %ux%u format %u\n", surface_id(rt), tex, d.Width, d.Height,
            (unsigned)d.Format);
    IDirect3DSurface9_Release(rt);
  }
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (SUCCEEDED(IDirect3DDevice9_GetVertexDeclaration(dev, &decl)) && decl) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &n))) {
      fprintf(g_draws, "  decl");
      for (UINT i = 0; i < n && el[i].Stream != 0xff; i++)
        fprintf(g_draws, " s%u+%u:t%u:u%u.%u", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage,
                el[i].UsageIndex);
      fprintf(g_draws, "\n");
    }
    IDirect3DVertexDeclaration9_Release(decl);
  }
  for (int stage = 0; stage < 16; stage++) {
    if (!g_cur_tex[stage])
      continue;
    const uint32_t t = object_id(g_tex_obj, &g_tex_count, MAX_TEXTURES, g_cur_tex[stage]);
    if (!g_tex_dumped[t] && g_save_tex) {
      g_tex_dumped[t] = 1;
      /* plain textures only: render targets and depth textures are the
         frame's own (shadow maps, reflections) */
      const D3DRESOURCETYPE type = IDirect3DBaseTexture9_GetType(g_cur_tex[stage]);
      D3DSURFACE_DESC d;
      memset(&d, 0, sizeof d);
      if (type == D3DRTYPE_TEXTURE)
        IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)g_cur_tex[stage], 0, &d);
      else if (type == D3DRTYPE_CUBETEXTURE)
        IDirect3DCubeTexture9_GetLevelDesc((IDirect3DCubeTexture9 *)g_cur_tex[stage], 0, &d);
      fprintf(g_draws, "  texinfo %u type %d %ux%u format %u usage %x pool %d\n", t, (int)type, d.Width, d.Height,
              (unsigned)d.Format, (unsigned)d.Usage, (int)d.Pool);
      fflush(g_draws);
      if ((type == D3DRTYPE_TEXTURE || type == D3DRTYPE_CUBETEXTURE) &&
          !(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && d.Pool != D3DPOOL_DEFAULT) {
        char path[MAX_PATH + 32];
        snprintf(path, sizeof path, "%s\\tex_%u.dds", g_trace_dir, t);
        g_save_tex(path, D3DXIFF_DDS, g_cur_tex[stage], NULL);
      }
    }
    fprintf(g_draws, "  tex %d = %u addr %u %u filter %u %u %u aniso %u srgb %u\n", stage, t,
            (unsigned)g_ss[stage][D3DSAMP_ADDRESSU], (unsigned)g_ss[stage][D3DSAMP_ADDRESSV],
            (unsigned)g_ss[stage][D3DSAMP_MAGFILTER], (unsigned)g_ss[stage][D3DSAMP_MINFILTER],
            (unsigned)g_ss[stage][D3DSAMP_MIPFILTER], (unsigned)g_ss[stage][D3DSAMP_MAXANISOTROPY],
            (unsigned)g_ss[stage][D3DSAMP_SRGBTEXTURE]);
  }
  fprintf(g_draws, "  rs zenable %u zwrite %u zfunc %u cull %u alphablend %u src %u dst %u op %u alphatest %u ref %u "
                   "func %u fog %u srgb %u colorwrite %x\n",
          (unsigned)g_rs[D3DRS_ZENABLE], (unsigned)g_rs[D3DRS_ZWRITEENABLE], (unsigned)g_rs[D3DRS_ZFUNC],
          (unsigned)g_rs[D3DRS_CULLMODE], (unsigned)g_rs[D3DRS_ALPHABLENDENABLE], (unsigned)g_rs[D3DRS_SRCBLEND],
          (unsigned)g_rs[D3DRS_DESTBLEND], (unsigned)g_rs[D3DRS_BLENDOP], (unsigned)g_rs[D3DRS_ALPHATESTENABLE],
          (unsigned)g_rs[D3DRS_ALPHAREF], (unsigned)g_rs[D3DRS_ALPHAFUNC], (unsigned)g_rs[D3DRS_FOGENABLE],
          (unsigned)g_rs[D3DRS_SRGBWRITEENABLE], (unsigned)g_rs[D3DRS_COLORWRITEENABLE]);
  {
    float bias, slope;
    memcpy(&bias, &g_rs[D3DRS_DEPTHBIAS], 4);
    memcpy(&slope, &g_rs[D3DRS_SLOPESCALEDEPTHBIAS], 4);
    if (bias != 0.f || slope != 0.f) fprintf(g_draws, "  depthbias %g slope %g\n", bias, slope);
  }
  /* without a pixel shader: the texture stages, up to the first disabled one */
  if (!g_cur_ps) {
    fprintf(g_draws, "  tfactor %08x\n", (unsigned)g_rs[D3DRS_TEXTUREFACTOR]);
    for (int st = 0; st < 8 && g_tss[st][D3DTSS_COLOROP] != D3DTOP_DISABLE; st++)
      fprintf(g_draws, "  tss %d color %u %x %x %x alpha %u %x %x %x tci %x ttf %x result %u\n", st,
              (unsigned)g_tss[st][D3DTSS_COLOROP], (unsigned)g_tss[st][D3DTSS_COLORARG1],
              (unsigned)g_tss[st][D3DTSS_COLORARG2], (unsigned)g_tss[st][D3DTSS_COLORARG0],
              (unsigned)g_tss[st][D3DTSS_ALPHAOP], (unsigned)g_tss[st][D3DTSS_ALPHAARG1],
              (unsigned)g_tss[st][D3DTSS_ALPHAARG2], (unsigned)g_tss[st][D3DTSS_ALPHAARG0],
              (unsigned)g_tss[st][D3DTSS_TEXCOORDINDEX], (unsigned)g_tss[st][D3DTSS_TEXTURETRANSFORMFLAGS],
              (unsigned)g_tss[st][D3DTSS_RESULTARG]);
  }
  float fog_start, fog_end, fog_density;
  memcpy(&fog_start, &g_rs[D3DRS_FOGSTART], 4);
  memcpy(&fog_end, &g_rs[D3DRS_FOGEND], 4);
  memcpy(&fog_density, &g_rs[D3DRS_FOGDENSITY], 4);
  fprintf(g_draws, "  fog color %08x table %u vertex %u range %u start %g end %g density %g blendop %u\n",
          (unsigned)g_rs[D3DRS_FOGCOLOR], (unsigned)g_rs[D3DRS_FOGTABLEMODE], (unsigned)g_rs[D3DRS_FOGVERTEXMODE],
          (unsigned)g_rs[D3DRS_RANGEFOGENABLE], fog_start, fog_end, fog_density, (unsigned)g_rs[D3DRS_BLENDOP]);
  /* the constants: only non-zero registers */
  for (int i = 0; i < 256; i++)
    if (g_vs_const[i][0] != 0.0f || g_vs_const[i][1] != 0.0f || g_vs_const[i][2] != 0.0f || g_vs_const[i][3] != 0.0f)
      fprintf(g_draws, "  vc%d %.6g %.6g %.6g %.6g\n", i, g_vs_const[i][0], g_vs_const[i][1], g_vs_const[i][2],
              g_vs_const[i][3]);
  for (int i = 0; i < 224; i++)
    if (g_ps_const[i][0] != 0.0f || g_ps_const[i][1] != 0.0f || g_ps_const[i][2] != 0.0f || g_ps_const[i][3] != 0.0f)
      fprintf(g_draws, "  pc%d %.6g %.6g %.6g %.6g\n", i, g_ps_const[i][0], g_ps_const[i][1], g_ps_const[i][2],
              g_ps_const[i][3]);
  fflush(g_draws);
}

typedef HRESULT(WINAPI *set_vs_fn)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
typedef HRESULT(WINAPI *set_ps_fn)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
typedef HRESULT(WINAPI *set_const_fn)(IDirect3DDevice9 *, UINT, const float *, UINT);
typedef HRESULT(WINAPI *set_tex_fn)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT(WINAPI *set_rs_fn)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(WINAPI *set_ss_fn)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(WINAPI *draw_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI *draw_idx_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *draw_up_fn)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
static set_vs_fn o_set_vs;
static set_ps_fn o_set_ps;
static set_const_fn o_set_vsc, o_set_psc;
static set_tex_fn o_set_tex;
static set_rs_fn o_set_rs;
static set_ss_fn o_set_ss;
static draw_fn o_draw;
static draw_idx_fn o_draw_idx;
static draw_up_fn o_draw_up;

static HRESULT WINAPI t_set_vs(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s) {
  g_cur_vs = s;
  return o_set_vs(d, s);
}
static HRESULT WINAPI t_set_ps(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s) {
  g_cur_ps = s;
  return o_set_ps(d, s);
}
static HRESULT WINAPI t_set_vsc(IDirect3DDevice9 *d, UINT r, const float *v, UINT n) {
  for (UINT i = 0; i < n && r + i < 256; i++)
    memcpy(g_vs_const[r + i], v + 4 * i, 16);
  return o_set_vsc(d, r, v, n);
}
static HRESULT WINAPI t_set_psc(IDirect3DDevice9 *d, UINT r, const float *v, UINT n) {
  for (UINT i = 0; i < n && r + i < 224; i++)
    memcpy(g_ps_const[r + i], v + 4 * i, 16);
  return o_set_psc(d, r, v, n);
}
static HRESULT WINAPI t_set_tex(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *t) {
  if (stage < 16)
    g_cur_tex[stage] = t;
  return o_set_tex(d, stage, t);
}
static HRESULT WINAPI t_set_rs(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v) {
  if ((unsigned)s < 256)
    g_rs[s] = v;
  return o_set_rs(d, s, v);
}
typedef HRESULT(WINAPI *set_tss_fn)(IDirect3DDevice9 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
static set_tss_fn o_set_tss;
static HRESULT WINAPI t_set_tss(IDirect3DDevice9 *d, DWORD stage, D3DTEXTURESTAGESTATETYPE s, DWORD v) {
  if (stage < 8 && (unsigned)s < 33)
    g_tss[stage][s] = v;
  return o_set_tss(d, stage, s, v);
}
static HRESULT WINAPI t_set_ss(IDirect3DDevice9 *d, DWORD stage, D3DSAMPLERSTATETYPE s, DWORD v) {
  if (stage < 16 && (unsigned)s < 14)
    g_ss[stage][s] = v;
  return o_set_ss(d, stage, s, v);
}
/* after a draw: the render target so far, when tracing steps */
static void trace_step(IDirect3DDevice9 *d) {
  if (!trace_active() || !g_trace_steps || !g_cur_rt)
    return;
  if (g_trace_steps > 1 && g_draw_index - 1 >= (uint32_t)g_trace_steps)
    return;
  char name[64];
  snprintf(name, sizeof name, "step_%u", g_draw_index - 1);
  save_rt(d, g_cur_rt, name);
}
static void dump_vb(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT base, UINT mn, UINT nv, UINT si, UINT n,
                    int indexed);
static UINT vertex_count(D3DPRIMITIVETYPE t, UINT n) {
  return t == D3DPT_TRIANGLELIST ? 3 * n : t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN ? n + 2
         : t == D3DPT_LINELIST ? 2 * n : t == D3DPT_LINESTRIP ? n + 1 : n;
}
static HRESULT WINAPI t_draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT start, UINT n) {
  trace_draw(d, "prim", n);
  dump_vb(d, t, (INT)start, 0, vertex_count(t, n), 0, n, 0);
  HRESULT hr = o_draw(d, t, start, n);
  trace_step(d);
  return hr;
}
/* TMUF_REFSHOT_TRACE_VB: a draw's vertices, into vb_<draw>.bin: uint32
   primitive type, vertex count, index count (0 for a non-indexed draw),
   flags (1 texcoord 0, 2 colour 0, 4 four-float positions), then the
   positions (POSITION 0 or POSITIONT 0, float3 or float4, from base + mn),
   the indices less mn (uint32), texcoord 0's float2 and colour 0's D3DCOLOR,
   both from the position's stream */
static void dump_vb(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE t, INT base, UINT mn, UINT nv, UINT si, UINT n,
                    int indexed) {
  static int enabled = -1;
  if (enabled < 0) enabled = GetEnvironmentVariableA("TMUF_REFSHOT_TRACE_VB", NULL, 0) > 0;
  if (!enabled || !trace_active() || !g_draws || !nv) return;
  IDirect3DVertexDeclaration9 *decl = NULL;
  if (FAILED(IDirect3DDevice9_GetVertexDeclaration(dev, &decl)) || !decl) return;
  D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
  UINT ne = MAXD3DDECLLENGTH + 1;
  int stream = -1, offset = 0, pos4 = 0, uv_stream = -1, uv_offset = 0, col_stream = -1, col_offset = 0;
  if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration(decl, el, &ne)))
    for (UINT i = 0; i < ne && el[i].Stream != 0xff; i++) {
      if ((el[i].Usage == D3DDECLUSAGE_POSITION || el[i].Usage == D3DDECLUSAGE_POSITIONT) && el[i].UsageIndex == 0 &&
          (el[i].Type == D3DDECLTYPE_FLOAT3 || el[i].Type == D3DDECLTYPE_FLOAT4))
        stream = el[i].Stream, offset = el[i].Offset, pos4 = el[i].Type == D3DDECLTYPE_FLOAT4;
      if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_FLOAT2)
        uv_stream = el[i].Stream, uv_offset = el[i].Offset;
      if (el[i].Usage == D3DDECLUSAGE_COLOR && el[i].UsageIndex == 0 && el[i].Type == D3DDECLTYPE_D3DCOLOR)
        col_stream = el[i].Stream, col_offset = el[i].Offset;
    }
  IDirect3DVertexDeclaration9_Release(decl);
  if (stream < 0) return;
  IDirect3DVertexBuffer9 *vb = NULL;
  UINT vb_off = 0, stride = 0;
  if (FAILED(IDirect3DDevice9_GetStreamSource(dev, (UINT)stream, &vb, &vb_off, &stride)) || !vb) return;
  IDirect3DIndexBuffer9 *ib = NULL;
  if (indexed) IDirect3DDevice9_GetIndices(dev, &ib);
  const UINT ni = !indexed ? 0 : t == D3DPT_TRIANGLELIST ? 3 * n : t == D3DPT_TRIANGLESTRIP ? n + 2 : 0;
  const int has_uv = uv_stream == stream, has_col = col_stream == stream, pf = pos4 ? 4 : 3;
  float *pos = malloc(sizeof(float) * 4 * nv), *uv = malloc(sizeof(float) * 2 * nv);
  uint32_t *col = malloc(sizeof(uint32_t) * nv), *idx = malloc(sizeof(uint32_t) * (ni ? ni : 1));
  void *vdata = NULL, *idata = NULL;
  int ok = pos && uv && col && idx && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &vdata, D3DLOCK_READONLY));
  if (ok) {
    for (UINT v = 0; v < nv; v++) {
      const char *at = (const char *)vdata + vb_off + (size_t)(base + (INT)mn + (INT)v) * stride;
      memcpy(pos + pf * v, at + offset, sizeof(float) * pf);
      if (has_uv) memcpy(uv + 2 * v, at + uv_offset, sizeof(float) * 2);
      if (has_col) memcpy(col + v, at + col_offset, sizeof(uint32_t));
    }
    IDirect3DVertexBuffer9_Unlock(vb);
  }
  if (ok && ib && ni) {
    D3DINDEXBUFFER_DESC id;
    IDirect3DIndexBuffer9_GetDesc(ib, &id);
    const UINT isize = id.Format == D3DFMT_INDEX32 ? 4 : 2;
    if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, 0, &idata, D3DLOCK_READONLY))) {
      for (UINT i = 0; i < ni; i++) {
        const uint32_t x = isize == 4 ? ((const uint32_t *)idata)[si + i] : ((const uint16_t *)idata)[si + i];
        idx[i] = x - mn;
      }
      IDirect3DIndexBuffer9_Unlock(ib);
    } else
      ok = 0;
  }
  if (ok) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\vb_%u.bin", g_trace_dir, g_draw_index - 1);
    FILE *f = fopen(path, "wb");
    if (f) {
      const uint32_t head[4] = {(uint32_t)t, nv, ni, (uint32_t)(has_uv | has_col << 1 | pos4 << 2)};
      fwrite(head, sizeof head, 1, f);
      fwrite(pos, sizeof(float) * pf, nv, f);
      fwrite(idx, sizeof(uint32_t), ni, f);
      if (has_uv) fwrite(uv, sizeof(float) * 2, nv, f);
      if (has_col) fwrite(col, sizeof(uint32_t), nv, f);
      fclose(f);
    }
  }
  free(pos);
  free(uv);
  free(col);
  free(idx);
  if (ib) IDirect3DIndexBuffer9_Release(ib);
  IDirect3DVertexBuffer9_Release(vb);
}

static HRESULT WINAPI t_draw_idx(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT mn, UINT nv, UINT si, UINT n) {
  trace_draw(d, "indexed", n);
  dump_vb(d, t, base, mn, nv, si, n, 1);
  HRESULT hr = o_draw_idx(d, t, base, mn, nv, si, n);
  trace_step(d);
  return hr;
}
static HRESULT WINAPI t_draw_up(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT stride) {
  trace_draw(d, "up", n);
  HRESULT hr = o_draw_up(d, t, n, v, stride);
  trace_step(d);
  return hr;
}
typedef HRESULT(WINAPI *set_rt_fn)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT(WINAPI *clear_fn)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(WINAPI *stretch_fn)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *,
                                    const RECT *, D3DTEXTUREFILTERTYPE);
static set_rt_fn o_set_rt;
static clear_fn o_clear;
static stretch_fn o_stretch;
static HRESULT WINAPI t_set_rt(IDirect3DDevice9 *d, DWORD index, IDirect3DSurface9 *s) {
  if (index == 0 && s != g_cur_rt) {
    leave_rt(d);
    g_cur_rt = s;
    g_cur_rt_draws = 0;
  }
  return o_set_rt(d, index, s);
}
static HRESULT WINAPI t_clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z,
                              DWORD st) {
  if (trace_active() && g_draws)
    fprintf(g_draws, "clear before %u rt surf %u flags %x color %08x z %g\n", g_draw_index,
            g_cur_rt ? surface_id(g_cur_rt) : 0u, (unsigned)flags, (unsigned)c, z);
  return o_clear(d, n, r, flags, c, z, st);
}
static HRESULT WINAPI t_stretch(IDirect3DDevice9 *d, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst,
                                const RECT *dr, D3DTEXTUREFILTERTYPE f) {
  if (trace_active() && g_draws)
    fprintf(g_draws, "stretch before %u surf %u -> surf %u filter %d\n", g_draw_index, surface_id(src),
            surface_id(dst), (int)f);
  return o_stretch(d, src, sr, dst, dr, f);
}

static void patch_slot(void **vtable, int slot, void *fn, void **old);

static void trace_hook_device(IDirect3DDevice9 *dev) {
  void **vt = *(void ***)dev;
  patch_slot(vt, 92, (void *)t_set_vs, (void **)&o_set_vs);
  patch_slot(vt, 107, (void *)t_set_ps, (void **)&o_set_ps);
  patch_slot(vt, 94, (void *)t_set_vsc, (void **)&o_set_vsc);
  patch_slot(vt, 109, (void *)t_set_psc, (void **)&o_set_psc);
  patch_slot(vt, 65, (void *)t_set_tex, (void **)&o_set_tex);
  patch_slot(vt, 57, (void *)t_set_rs, (void **)&o_set_rs);
  patch_slot(vt, 69, (void *)t_set_ss, (void **)&o_set_ss);
  patch_slot(vt, 67, (void *)t_set_tss, (void **)&o_set_tss);
  /* Direct3D's defaults, until the game sets them */
  for (int st = 0; st < 8; st++) {
    g_tss[st][D3DTSS_COLOROP] = st ? D3DTOP_DISABLE : D3DTOP_MODULATE;
    g_tss[st][D3DTSS_ALPHAOP] = st ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
    g_tss[st][D3DTSS_COLORARG1] = g_tss[st][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    g_tss[st][D3DTSS_COLORARG2] = g_tss[st][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    g_tss[st][D3DTSS_TEXCOORDINDEX] = st;
  }
  patch_slot(vt, 81, (void *)t_draw, (void **)&o_draw);
  patch_slot(vt, 82, (void *)t_draw_idx, (void **)&o_draw_idx);
  patch_slot(vt, 83, (void *)t_draw_up, (void **)&o_draw_up);
  patch_slot(vt, 37, (void *)t_set_rt, (void **)&o_set_rt);
  patch_slot(vt, 43, (void *)t_clear, (void **)&o_clear);
  patch_slot(vt, 34, (void *)t_stretch, (void **)&o_stretch);
  HMODULE d3dx = GetModuleHandleA("d3dx9_30.dll");
  if (!d3dx)
    d3dx = LoadLibraryA("d3dx9_30.dll");
  if (d3dx) {
    g_disasm = (disasm_fn)(void *)GetProcAddress(d3dx, "D3DXDisassembleShader");
    g_save_tex = (save_tex_fn)(void *)GetProcAddress(d3dx, "D3DXSaveTextureToFileA");
    g_save_surf = (save_surf_fn)(void *)GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA");
  }
  log_msg("[refshot] tracing hooks installed (disassembler %s)", g_disasm ? "found" : "missing");
}

/* CHmsPackLightMap::ComputeLighting: the map's lightmaps are being made */
static uint8_t *g_lighting_thunk;

/* the thunk ComputeLighting returns through: where it really returns to */
static uint32_t __cdecl on_compute_lighting_return(void) {
  g_in_lighting--;
  return g_lighting_returns[g_in_lighting & 7];
}

static void finish_load_trace(void) {
  g_trace_load = 3;
  g_tracing = 0;
  if (g_draws)
    fclose(g_draws);
  g_draws = NULL;
  g_cur_rt_draws = 0;
  trace_done();
  log_msg("[refshot] lightmap trace done: %u draws", g_draw_index);
  memcpy(g_trace_dir, g_view_trace_dir, sizeof g_trace_dir);
  g_draw_index = 0;
  g_rt_saves = 0;
  trace_reset_dumps();
}

static void on_compute_lighting(void *self, const uint32_t *stack) {
  (void)self;
  if (g_trace_load == 2 && g_lighting_thunk && g_in_lighting < 8) {
    /* return through the thunk: draws until then are the computation's */
    uint32_t *ret = (uint32_t *)&stack[ST_RET];
    g_lighting_returns[g_in_lighting & 7] = *ret;
    g_in_lighting++;
    *ret = (uint32_t)(uintptr_t)g_lighting_thunk;
    return;
  }
  if (g_trace_load != 1)
    return;
  if (!g_lighting_thunk) {
    /* sub esp,4; pushad; call handler; mov [esp+32],eax; popad; ret */
    uint8_t *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
      return;
    uint8_t *p = t;
    *p++ = 0x83, *p++ = 0xec, *p++ = 0x04;
    *p++ = 0x60;
    *p++ = 0xe8;
    put_rel32(p, (uintptr_t)(p + 4), (uintptr_t)on_compute_lighting_return);
    p += 4;
    *p++ = 0x89, *p++ = 0x44, *p++ = 0x24, *p++ = 0x20;
    *p++ = 0x61;
    *p++ = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)(p - t));
    g_lighting_thunk = t;
  }
  g_trace_load = 2;
  memcpy(g_view_trace_dir, g_trace_dir, sizeof g_trace_dir);
  memcpy(g_trace_dir, g_trace_load_dir, sizeof g_trace_dir);
  char path[MAX_PATH + 32];
  snprintf(path, sizeof path, "%s\\draws.txt", g_trace_dir);
  g_draws = fopen(path, "w");
  g_draw_index = 0;
  g_tracing = 1;
  g_lighting_present = g_presented;
  log_msg("[refshot] tracing the lightmap computation");
  on_compute_lighting(self, stack); /* this call's draws too */
}

static hook g_hooks[] = {
    {"CHmsCamera::Run", 0x0053ea00, {0x83, 0xec, 0x68, 0xa1, 0x40, 0x48, 0xd7, 0x00}, 8, on_camera_run},
    {"CHmsPackLightMap::ComputeLighting", 0x005465d0, {0x83, 0xec, 0x10, 0x55, 0x56, 0x8b, 0xf1}, 7,
     on_compute_lighting},
};

static void put_rel32(uint8_t *at, uintptr_t next, uintptr_t target) {
  int32_t rel = (int32_t)(target - next);
  memcpy(at, &rel, 4);
}

static int install(hook *h) {
  uint8_t *target = (uint8_t *)h->address;
  if (memcmp(target, h->expected, (size_t)h->stolen) != 0) {
    log_msg("[refshot] %s: unexpected bytes at 0x%08x, not hooking", h->name, (unsigned)h->address);
    return 0;
  }
  uint8_t *t = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!t)
    return 0;
  uint8_t *p = t;
  *p++ = 0x60; /* pushad */
  *p++ = 0x9c; /* pushfd */
  *p++ = 0x54; /* push esp */
  *p++ = 0x51; /* push ecx */
  *p++ = 0xe8;
  put_rel32(p, (uintptr_t)(p + 4), (uintptr_t)h->handler);
  p += 4;
  *p++ = 0x83, *p++ = 0xc4, *p++ = 0x08; /* add esp, 8 */
  *p++ = 0x9d;                           /* popfd */
  *p++ = 0x61;                           /* popad */
  memcpy(p, target, (size_t)h->stolen);
  p += h->stolen;
  *p++ = 0xe9;
  put_rel32(p, (uintptr_t)(p + 4), h->address + (uintptr_t)h->stolen);
  p += 4;
  FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)(p - t));
  DWORD old;
  if (!VirtualProtect(target, (SIZE_T)h->stolen, PAGE_EXECUTE_READWRITE, &old))
    return 0;
  target[0] = 0xe9;
  put_rel32(target + 1, h->address + 5, (uintptr_t)t);
  for (int i = 5; i < h->stolen; i++)
    target[i] = 0x90;
  VirtualProtect(target, (SIZE_T)h->stolen, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, (SIZE_T)h->stolen);
  log_msg("[refshot] hooked %s at 0x%08x", h->name, (unsigned)h->address);
  return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
  (void)inst, (void)reserved;
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("TMUF_REFSHOT_LOG", path, sizeof path);
  if (n > 0 && n < sizeof path)
    g_log = fopen(path, "w");
  n = GetEnvironmentVariableA("TMUF_REFSHOT_POSE", path, sizeof path);
  if (n > 0 && n < sizeof path) {
    FILE *f = fopen(path, "r");
    if (f) {
      while (g_pose_count < MAX_POSES) {
        int k = 0;
        while (k < 13 && fscanf(f, "%f", &g_poses[g_pose_count][k]) == 1)
          k++;
        if (k < 13)
          break;
        g_pose_count++;
      }
      fclose(f);
    }
    log_msg("[refshot] %u poses", g_pose_count);
  }
  char num[32];
  n = GetEnvironmentVariableA("TMUF_REFSHOT_HOLD", num, sizeof num);
  if (n > 0 && n < sizeof num && atoi(num) > 0)
    g_hold = (uint32_t)atoi(num);
  n = GetEnvironmentVariableA("TMUF_REFSHOT_SKIP", num, sizeof num);
  if (n > 0 && n < sizeof num)
    g_skip = (uint32_t)atoi(num);
  n = GetEnvironmentVariableA("TMUF_REFSHOT_FRAMES", g_frames_dir, sizeof g_frames_dir);
  if (n == 0 || n >= sizeof g_frames_dir)
    g_frames_dir[0] = 0;
  char every[32];
  n = GetEnvironmentVariableA("TMUF_REFSHOT_EVERY", every, sizeof every);
  if (n > 0 && n < sizeof every)
    g_every = (uint32_t)strtoul(every, NULL, 10);
  n = GetEnvironmentVariableA("TMUF_REFSHOT_TRACE", g_trace_dir, sizeof g_trace_dir);
  if (n == 0 || n >= sizeof g_trace_dir)
    g_trace_dir[0] = 0;
  n = GetEnvironmentVariableA("TMUF_REFSHOT_TRACE_LOAD", g_trace_load_dir, sizeof g_trace_load_dir);
  if (n > 0 && n < sizeof g_trace_load_dir)
    g_trace_load = 1;
  n = GetEnvironmentVariableA("TMUF_REFSHOT_TRACE_RT_MAX", num, sizeof num);
  if (n > 0 && n < sizeof num)
    g_rt_save_max = (uint32_t)strtoul(num, NULL, 10);
  n = GetEnvironmentVariableA("TMUF_REFSHOT_TRACE_STEPS", num, sizeof num);
  g_trace_steps = n > 0 && n < sizeof num && atoi(num) > 0 ? atoi(num) : 0; /* 1: every draw, N > 1: the first N */
  n = GetEnvironmentVariableA("TMUF_REFSHOT_TRACE_VIEW", num, sizeof num);
  if (n > 0 && n < sizeof num)
    g_trace_view = atoi(num);
  hook_import();
  for (size_t i = 0; i < sizeof g_hooks / sizeof g_hooks[0]; i++)
    install(&g_hooks[i]);
  return TRUE;
}
