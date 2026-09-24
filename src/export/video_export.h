#ifndef FRAMETEE_VIDEO_EXPORT_H
#define FRAMETEE_VIDEO_EXPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gfx_handler_t;

typedef struct video_export_options_t {
  int width, height;
  int fps_num, fps_den;
  // Camera time in seconds, both ends included. Each frame shows the game at
  // the tick the camera's time remap puts there.
  double start_time, end_time;
  int codec;         // H.264, HEVC, AV1
  int quality_mode;  // constant quality or target bitrate
  int quality;       // CRF/CQ: 0..51
  int bitrate_kbps;
  int preset;        // fast, medium, slow
  int bit_depth;     // 8 or 10
  int hardware;      // 1: the GPU's encoder (NVENC, AMF, Quick Sync or VAAPI)
} video_export_options_t;

typedef struct video_export_job_t {
  video_export_options_t options;
  char path[1024];
  char temporary_path[1056];
  char snapshot_path[1024];
  char progress_path[1056];
  char status[256];
  void *encoder;
  int64_t frame_index, frame_count;
  double sample_time; // camera seconds of the frame being drawn
  double started_at;  // glfwGetTime() when the render began
  double elapsed;     // wall-clock seconds the render took, once it ends
  int old_tick;
  intptr_t worker_handle;
  bool worker_process;
  bool active, finished, failed, cancelled;
} video_export_job_t;

void video_export_defaults(video_export_options_t *options);
int64_t video_export_frame_count(const video_export_options_t *options);
bool video_export_available(void);
bool video_export_start(struct gfx_handler_t *handler, video_export_job_t *job,
                        const video_export_options_t *options, const char *path);
// CLI/worker path: renders in this process, with no editor frame loop.
bool video_export_start_inline(struct gfx_handler_t *handler, video_export_job_t *job,
                               const video_export_options_t *options, const char *path,
                               const char *progress_path);
void video_export_step(struct gfx_handler_t *handler, video_export_job_t *job,
                       void (*draw)(struct gfx_handler_t *, float));
// Wall-clock seconds since the render began, or its total once it has ended.
double video_export_elapsed(const video_export_job_t *job);
// Seconds of video per second of rendering so far (2 = twice real time).
double video_export_speed(const video_export_job_t *job);
// "12.3s", "4:05" or "1:02:03".
void video_export_format_duration(double seconds, char *out, size_t size);
void video_export_cancel(struct gfx_handler_t *handler, video_export_job_t *job);

#endif
