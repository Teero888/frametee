#ifndef FRAMETEE_VIDEO_EXPORT_H
#define FRAMETEE_VIDEO_EXPORT_H

#include <stdbool.h>
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
} video_export_options_t;

typedef struct video_export_job_t {
  video_export_options_t options;
  char path[1024];
  char temporary_path[1056];
  char snapshot_path[1024];
  char progress_path[1056];
  char status[256];
  void *encoder;
  uint8_t *pixels;
  int64_t frame_index, frame_count;
  double sample_time; // camera seconds of the frame being drawn
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
void video_export_cancel(struct gfx_handler_t *handler, video_export_job_t *job);

#endif
