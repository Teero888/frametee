#include "video_export.h"

#include <GLFW/glfw3.h>
#include <logger/logger.h>
#include <renderer/graphics_backend.h>
#include <system/compat_threads.h>
#include <system/fs.h>
#include <system/save.h>
#include <user_interface/camera/camera_timeline.h>
#include <user_interface/timeline/timeline_model.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#ifdef FT_HAS_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

typedef struct encoder_t {
  AVFormatContext *format;
  AVCodecContext *codec;
  AVStream *stream;
  AVFrame *frame;
  AVFrame *hardware_frame; // VAAPI only: `frame` is uploaded into this
  AVPacket *packet;
  AVBufferRef *device;
  struct SwsContext *converter;
  const char *name;
  // Frames are converted and encoded on their own thread while the next one
  // renders. Two buffers: the render thread fills one while this drains the other.
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t changed;
  bool threaded, stopping, failed;
  uint8_t *buffers[2];
  int64_t pts[2];
  bool filled[2];
  int fill_slot, drain_slot;
} encoder_t;

static const char *const software_encoders[] = {"libx264", "libx265", "libsvtav1"};
static const char *const codec_names[] = {"h264", "hevc", "av1"};

// Hardware encoders to try, in order; the first that opens is used.
typedef enum hardware_api_t { HARDWARE_NVENC, HARDWARE_AMF, HARDWARE_QSV, HARDWARE_VAAPI, HARDWARE_COUNT } hardware_api_t;
static const char *const hardware_suffixes[] = {"nvenc", "amf", "qsv", "vaapi"};

static bool encoder_drain(encoder_t *e) {
  for (;;) {
    int result = avcodec_receive_packet(e->codec, e->packet);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
    if (result < 0) return false;
    av_packet_rescale_ts(e->packet, e->codec->time_base, e->stream->time_base);
    e->packet->stream_index = e->stream->index;
    result = av_interleaved_write_frame(e->format, e->packet);
    av_packet_unref(e->packet);
    if (result < 0) return false;
  }
}

static void encoder_stop_thread(encoder_t *e) {
  if (!e->threaded) return;
  pthread_mutex_lock(&e->lock);
  e->stopping = true;
  pthread_cond_broadcast(&e->changed);
  pthread_mutex_unlock(&e->lock);
  pthread_join(e->thread, NULL);
  pthread_cond_destroy(&e->changed);
  pthread_mutex_destroy(&e->lock);
  e->threaded = false;
}

static void encoder_close(encoder_t *e) {
  if (!e) return;
  encoder_stop_thread(e);
  free(e->buffers[0]);
  free(e->buffers[1]);
  if (e->format && e->format->pb) avio_closep(&e->format->pb);
  if (e->format) avformat_free_context(e->format);
  avcodec_free_context(&e->codec);
  av_frame_free(&e->frame);
  av_frame_free(&e->hardware_frame);
  av_packet_free(&e->packet);
  av_buffer_unref(&e->device);
  sws_freeContext(e->converter);
  free(e);
}

static void *encoder_thread(void *data);

static void set_int(AVCodecContext *codec, const char *name, int64_t value) {
  av_opt_set_int(codec->priv_data, name, value, 0);
}

// Settings shared by every encoder. `format` is what the encoder is fed.
static AVCodecContext *codec_context(const AVCodec *codec, const video_export_options_t *o, bool global_header,
                                     enum AVPixelFormat format) {
  AVCodecContext *c = avcodec_alloc_context3(codec);
  if (!c) return NULL;
  c->width = o->width;
  c->height = o->height;
  c->time_base = (AVRational){o->fps_den, o->fps_num};
  c->framerate = (AVRational){o->fps_num, o->fps_den};
  c->pix_fmt = format;
  c->colorspace = AVCOL_SPC_BT709;
  c->color_primaries = AVCOL_PRI_BT709;
  c->color_trc = AVCOL_TRC_BT709;
  c->color_range = AVCOL_RANGE_MPEG;
  c->gop_size = o->fps_num / o->fps_den * 2;
  // A packet for the final displayed frame must extend the MP4 duration to
  // the requested interval. Avoid reorder delay for very short exports.
  c->max_b_frames = 0;
  if (global_header) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (o->quality_mode == 1) c->bit_rate = (int64_t)o->bitrate_kbps * 1000;
  return c;
}

static AVCodecContext *open_software(const video_export_options_t *o, bool global_header, char *error,
                                     size_t error_size) {
  const char *name = software_encoders[o->codec];
  const AVCodec *codec = avcodec_find_encoder_by_name(name);
  if (!codec) {
    snprintf(error, error_size, "%s encoder is unavailable in this FFmpeg build", name);
    return NULL;
  }
  AVCodecContext *c =
      codec_context(codec, o, global_header, o->bit_depth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P);
  if (!c) return NULL;
  // libavcodec defaults to one thread, unlike the ffmpeg CLI; 0 lets the
  // encoder use every core.
  c->thread_count = 0;
  const char *preset = o->preset == 0 ? "fast" : o->preset == 2 ? "slow" : "medium";
  if (o->codec == 2) preset = o->preset == 0 ? "8" : o->preset == 2 ? "4" : "6";
  av_opt_set(c->priv_data, "preset", preset, 0);
  if (o->quality_mode == 0 && av_opt_set_int(c->priv_data, "crf", o->quality, 0) < 0) {
    snprintf(error, error_size, "Selected encoder does not support constant quality");
    avcodec_free_context(&c);
    return NULL;
  }
  if (avcodec_open2(c, codec, NULL) < 0) {
    snprintf(error, error_size, "Could not open %s at %d-bit", name, o->bit_depth);
    avcodec_free_context(&c);
  }
  return c;
}

// Hardware encoders have no CRF; constant quality maps to a fixed QP (or
// NVENC's CQ) at the same number. The Speed setting maps to their presets.
static AVCodecContext *open_hardware(const video_export_options_t *o, hardware_api_t api, bool global_header,
                                     AVBufferRef **device) {
  char name[32];
  snprintf(name, sizeof(name), "%s_%s", codec_names[o->codec], hardware_suffixes[api]);
  const AVCodec *codec = avcodec_find_encoder_by_name(name);
  if (!codec) return NULL;
  const enum AVPixelFormat upload = o->bit_depth == 10 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
  AVCodecContext *c = codec_context(codec, o, global_header, api == HARDWARE_VAAPI ? AV_PIX_FMT_VAAPI : upload);
  if (!c) return NULL;
  const bool constant = o->quality_mode == 0;
  switch (api) {
  case HARDWARE_NVENC:
    av_opt_set(c->priv_data, "preset", o->preset == 0 ? "p2" : o->preset == 2 ? "p6" : "p4", 0);
    av_opt_set(c->priv_data, "rc", constant ? "vbr" : "cbr", 0);
    if (constant) av_opt_set_double(c->priv_data, "cq", o->quality > 0 ? o->quality : 1, 0);
    break;
  case HARDWARE_AMF:
    av_opt_set(c->priv_data, "quality", o->preset == 0 ? "speed" : o->preset == 2 ? "quality" : "balanced", 0);
    av_opt_set(c->priv_data, "rc", constant ? "cqp" : "vbr_peak", 0);
    if (constant) {
      set_int(c, "qp_i", o->quality);
      set_int(c, "qp_p", o->quality);
    }
    break;
  case HARDWARE_QSV:
    av_opt_set(c->priv_data, "preset", o->preset == 0 ? "veryfast" : o->preset == 2 ? "slow" : "medium", 0);
    if (constant) {
      c->flags |= AV_CODEC_FLAG_QSCALE;
      c->global_quality = o->quality * FF_QP2LAMBDA;
    }
    break;
  case HARDWARE_VAAPI: {
    av_opt_set(c->priv_data, "rc_mode", constant ? "CQP" : "VBR", 0);
    if (constant) set_int(c, "qp", o->quality > 0 ? o->quality : 1);
    // VAAPI takes frames in GPU surfaces; the CPU frame is uploaded into one.
    if (!*device && av_hwdevice_ctx_create(device, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) < 0) goto failed;
    AVBufferRef *frames = av_hwframe_ctx_alloc(*device);
    if (!frames) goto failed;
    AVHWFramesContext *surfaces = (AVHWFramesContext *)frames->data;
    surfaces->format = AV_PIX_FMT_VAAPI;
    surfaces->sw_format = upload;
    surfaces->width = o->width;
    surfaces->height = o->height;
    surfaces->initial_pool_size = 16;
    if (av_hwframe_ctx_init(frames) < 0) {
      av_buffer_unref(&frames);
      goto failed;
    }
    c->hw_frames_ctx = frames;
    break;
  }
  default: break;
  }
  if (avcodec_open2(c, codec, NULL) >= 0) return c;
failed:
  avcodec_free_context(&c);
  return NULL;
}

static encoder_t *encoder_open(const video_export_options_t *o, bool bgra, const char *path, char *error,
                               size_t error_size) {
  encoder_t *e = calloc(1, sizeof(*e));
  if (!e) return NULL;
  if (avformat_alloc_output_context2(&e->format, NULL, "mp4", path) < 0 || !e->format) goto failed;
  const bool global_header = e->format->oformat->flags & AVFMT_GLOBALHEADER;
  if (o->hardware) {
    // Encoders for hardware this system lacks fail loudly; that is expected.
    const int log_level = av_log_get_level();
    av_log_set_level(AV_LOG_FATAL);
    for (int api = 0; api < HARDWARE_COUNT && !e->codec; ++api)
      e->codec = open_hardware(o, (hardware_api_t)api, global_header, &e->device);
    av_log_set_level(log_level);
    if (!e->codec) {
      snprintf(error, error_size, "No hardware %s encoder at %d-bit found on this system",
               o->codec == 0 ? "H.264" : o->codec == 1 ? "HEVC" : "AV1", o->bit_depth);
      goto failed;
    }
  } else if (!(e->codec = open_software(o, global_header, error, error_size))) {
    goto failed;
  }
  e->name = e->codec->codec->name;
  e->stream = avformat_new_stream(e->format, NULL);
  e->frame = av_frame_alloc();
  e->packet = av_packet_alloc();
  if (!e->stream || !e->frame || !e->packet) goto failed;
  e->stream->time_base = e->codec->time_base;
  e->stream->avg_frame_rate = e->codec->framerate;
  if (avcodec_parameters_from_context(e->stream->codecpar, e->codec) < 0) goto failed;
  const bool surfaces = e->codec->hw_frames_ctx != NULL;
  e->frame->format = surfaces ? ((AVHWFramesContext *)e->codec->hw_frames_ctx->data)->sw_format : e->codec->pix_fmt;
  e->frame->width = o->width;
  e->frame->height = o->height;
  if (av_frame_get_buffer(e->frame, 32) < 0) goto failed;
  if (surfaces && !(e->hardware_frame = av_frame_alloc())) goto failed;
  // Same size in and out: the filter only matters for chroma subsampling.
  e->converter = sws_getContext(o->width, o->height, bgra ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, o->width,
                                o->height, e->frame->format, SWS_BILINEAR, NULL, NULL, NULL);
  if (!e->converter) goto failed;
  const int *bt709 = sws_getCoefficients(SWS_CS_ITU709);
  if (sws_setColorspaceDetails(e->converter, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16) < 0 ||
      avio_open(&e->format->pb, path, AVIO_FLAG_WRITE) < 0) goto failed;
  AVDictionary *mux_options = NULL;
  av_dict_set(&mux_options, "movflags", "+faststart", 0);
  int written = avformat_write_header(e->format, &mux_options);
  av_dict_free(&mux_options);
  if (written < 0) goto failed;
  const size_t size = (size_t)o->width * o->height * 4u;
  if (!(e->buffers[0] = malloc(size)) || !(e->buffers[1] = malloc(size))) goto failed;
  if (pthread_mutex_init(&e->lock, NULL) != 0) goto failed;
  if (pthread_cond_init(&e->changed, NULL) != 0) {
    pthread_mutex_destroy(&e->lock);
    goto failed;
  }
  if (pthread_create(&e->thread, NULL, encoder_thread, e) != 0) {
    pthread_cond_destroy(&e->changed);
    pthread_mutex_destroy(&e->lock);
    goto failed;
  }
  e->threaded = true;
  return e;
failed:
  if (!error[0]) snprintf(error, error_size, "Could not initialize MP4 encoder");
  encoder_close(e);
  return NULL;
}

static bool encoder_write(encoder_t *e, const uint8_t *pixels, int64_t pts) {
  if (av_frame_make_writable(e->frame) < 0) return false;
  const uint8_t *source[4] = {pixels, NULL, NULL, NULL};
  int stride[4] = {e->codec->width * 4, 0, 0, 0};
  if (sws_scale(e->converter, source, stride, 0, e->codec->height, e->frame->data, e->frame->linesize) <= 0)
    return false;
  AVFrame *frame = e->frame;
  if (e->hardware_frame) {
    frame = e->hardware_frame;
    av_frame_unref(frame);
    if (av_hwframe_get_buffer(e->codec->hw_frames_ctx, frame, 0) < 0 ||
        av_hwframe_transfer_data(frame, e->frame, 0) < 0) return false;
  }
  frame->pts = pts;
  return avcodec_send_frame(e->codec, frame) >= 0 && encoder_drain(e);
}

static void *encoder_thread(void *data) {
  encoder_t *e = data;
  pthread_mutex_lock(&e->lock);
  for (;;) {
    // Frames already handed over are still written when stopping.
    while (!e->filled[e->drain_slot] && !e->stopping) pthread_cond_wait(&e->changed, &e->lock);
    if (!e->filled[e->drain_slot]) break;
    const int slot = e->drain_slot;
    pthread_mutex_unlock(&e->lock);
    const bool written = encoder_write(e, e->buffers[slot], e->pts[slot]);
    pthread_mutex_lock(&e->lock);
    e->filled[slot] = false;
    e->drain_slot = slot ^ 1;
    e->failed |= !written;
    pthread_cond_broadcast(&e->changed);
    if (!written) break;
  }
  pthread_mutex_unlock(&e->lock);
  return NULL;
}

// The buffer the next frame is drawn into, once the encoder has let go of it.
static uint8_t *encoder_acquire(encoder_t *e) {
  pthread_mutex_lock(&e->lock);
  while (e->filled[e->fill_slot] && !e->failed) pthread_cond_wait(&e->changed, &e->lock);
  uint8_t *buffer = e->failed ? NULL : e->buffers[e->fill_slot];
  pthread_mutex_unlock(&e->lock);
  return buffer;
}

static void encoder_submit(encoder_t *e, int64_t pts) {
  pthread_mutex_lock(&e->lock);
  e->pts[e->fill_slot] = pts;
  e->filled[e->fill_slot] = true;
  e->fill_slot ^= 1;
  pthread_cond_broadcast(&e->changed);
  pthread_mutex_unlock(&e->lock);
}

static bool encoder_finish(encoder_t *e, int64_t frame_count) {
  encoder_stop_thread(e);
  if (e->failed || avcodec_send_frame(e->codec, NULL) < 0 || !encoder_drain(e)) return false;
  e->stream->duration = av_rescale_q(frame_count, e->codec->time_base, e->stream->time_base);
  return av_write_trailer(e->format) >= 0;
}
#endif

void video_export_defaults(video_export_options_t *o) {
  memset(o, 0, sizeof(*o));
  o->width = 1920; o->height = 1080;
  o->fps_num = 60; o->fps_den = 1;
  o->codec = 0; o->quality = 18; o->bitrate_kbps = 16000;
  o->preset = 1; o->bit_depth = 8;
}

int64_t video_export_frame_count(const video_export_options_t *o) {
  if (o->fps_num <= 0 || o->fps_den <= 0 || !(o->end_time >= o->start_time)) return 0;
  return (int64_t)floor((o->end_time - o->start_time) * o->fps_num / o->fps_den + 1e-9) + 1;
}

static bool range_valid(const video_export_options_t *o) {
  return isfinite(o->start_time) && isfinite(o->end_time) && o->start_time >= 0.0 && o->end_time >= o->start_time &&
         video_export_frame_count(o) > 0 && video_export_frame_count(o) < INT32_MAX;
}

bool video_export_available(void) {
#ifdef FT_HAS_FFMPEG
  return true;
#else
  return false;
#endif
}

static bool create_snapshot_path(char *path, size_t capacity) {
#ifdef _WIN32
  char directory[MAX_PATH];
  if (!GetTempPathA(sizeof(directory), directory) ||
      !GetTempFileNameA(directory, "ftr", 0, path)) return false;
  return strlen(path) < capacity;
#else
  const char *directory = getenv("TMPDIR");
  if (!directory || !*directory) directory = "/tmp";
  if (snprintf(path, capacity, "%s/frametee-render-XXXXXX", directory) >= (int)capacity) return false;
  int fd = mkstemp(path);
  if (fd < 0) return false;
  close(fd);
  return true;
#endif
}

#ifdef _WIN32
static bool append_windows_argument(wchar_t *line, size_t capacity, const char *utf8) {
  wchar_t wide[1200];
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide,
                          (int)(sizeof(wide) / sizeof(wide[0]))) <= 0) return false;
  size_t used = wcslen(line);
  if (used + 3 >= capacity) return false;
  if (used) line[used++] = L' ';
  line[used++] = L'"';
  for (const wchar_t *p = wide; *p;) {
    size_t slashes = 0;
    while (p[slashes] == L'\\') ++slashes;
    if (slashes) {
      p += slashes;
      size_t copies = *p == L'"' || *p == L'\0' ? slashes * 2 : slashes;
      if (used + copies + 2 >= capacity) return false;
      while (copies--) line[used++] = L'\\';
    }
    if (!*p) break;
    if (*p == L'"') line[used++] = L'\\';
    if (used + 2 >= capacity) return false;
    line[used++] = *p++;
  }
  line[used++] = L'"';
  line[used] = L'\0';
  return true;
}
#endif

static bool spawn_render_worker(video_export_job_t *job) {
  char executable[1200], directory[1100];
  if (!fs_get_executable_dir(directory, sizeof(directory)) ||
      snprintf(executable, sizeof(executable), "%s%cframetee%s", directory, PATH_SEP,
#ifdef _WIN32
               ".exe"
#else
               ""
#endif
               ) >= (int)sizeof(executable)) return false;
  char range[64], size[64], fps[64], quality[32], depth[16];
  snprintf(range, sizeof(range), "%.9g:%.9g", job->options.start_time, job->options.end_time);
  snprintf(size, sizeof(size), "%dx%d", job->options.width, job->options.height);
  snprintf(fps, sizeof(fps), "%d/%d", job->options.fps_num, job->options.fps_den);
  snprintf(quality, sizeof(quality), "%d", job->options.quality_mode ? job->options.bitrate_kbps : job->options.quality);
  snprintf(depth, sizeof(depth), "%d", job->options.bit_depth);
  const char *codecs[] = {"h264", "hevc", "av1"};
  const char *presets[] = {"fast", "medium", "slow"};
  const char *args[] = {executable, "--render-worker", "--project", job->snapshot_path,
                        "--render-video", job->path, "--render-progress", job->progress_path,
                        "--render-range", range, "--render-size", size, "--render-fps", fps,
                        "--render-codec", codecs[job->options.codec],
                        job->options.quality_mode ? "--render-bitrate" : "--render-crf", quality,
                        "--render-preset", presets[job->options.preset], "--render-depth", depth,
                        job->options.hardware ? "--render-hardware" : NULL, NULL};
#ifdef _WIN32
  wchar_t command[8192] = {0};
  for (int i = 0; args[i]; ++i)
    if (!append_windows_argument(command, sizeof(command) / sizeof(command[0]), args[i])) return false;
  STARTUPINFOW startup = {.cb = sizeof(startup)};
  PROCESS_INFORMATION process = {0};
  if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) return false;
  CloseHandle(process.hThread);
  job->worker_handle = (intptr_t)process.hProcess;
#else
  pid_t pid;
  int result = posix_spawn(&pid, executable, NULL, NULL, (char *const *)args, environ);
  if (result != 0) return false;
  job->worker_handle = (intptr_t)pid;
#endif
  return true;
}

bool video_export_start(gfx_handler_t *h, video_export_job_t *job, const video_export_options_t *o, const char *path) {
  if (!h || !job || !o || !path || !*path || !h->level || job->active) return false;
  job->status[0] = '\0';
  int tps = game_ticks_per_second(&h->game_host);
  if (!video_export_available() || o->width < 2 || o->height < 2 || (o->width & 1) || (o->height & 1) ||
      o->fps_num <= 0 || o->fps_den <= 0 || !range_valid(o) || tps <= 0 ||
      o->codec < 0 || o->codec > 2 || (o->bit_depth != 8 && o->bit_depth != 10) ||
      o->quality_mode < 0 || o->quality_mode > 1 || o->preset < 0 || o->preset > 2 || o->hardware < 0 || o->hardware > 1 ||
      o->quality < 0 || o->quality > 51 || o->bitrate_kbps <= 0) {
    snprintf(job->status, sizeof(job->status), "Invalid settings or MP4 encoder unavailable");
    return false;
  }
  video_export_job_t next = {0};
  next.options = *o;
  if (snprintf(next.path, sizeof(next.path), "%s", path) >= (int)sizeof(next.path) ||
      snprintf(next.temporary_path, sizeof(next.temporary_path), "%s.part.mp4", path) >=
          (int)sizeof(next.temporary_path) || !create_snapshot_path(next.snapshot_path, sizeof(next.snapshot_path))) {
    snprintf(job->status, sizeof(job->status), "Could not create render snapshot path");
    return false;
  }
  if (snprintf(next.progress_path, sizeof(next.progress_path), "%s.progress", next.snapshot_path) >=
          (int)sizeof(next.progress_path) || !save_project_snapshot(&h->user_interface, next.snapshot_path)) {
    fs_remove(next.snapshot_path);
    snprintf(job->status, sizeof(job->status), "Could not snapshot project for rendering");
    return false;
  }
  next.frame_count = video_export_frame_count(o);
  if (!spawn_render_worker(&next)) {
    fs_remove(next.snapshot_path);
    snprintf(job->status, sizeof(job->status), "Could not start render worker");
    return false;
  }
  next.active = true;
  next.started_at = glfwGetTime();
  next.worker_process = true;
  snprintf(next.status, sizeof(next.status), "Rendering in background: 0 / %lld", (long long)next.frame_count);
  *job = next;
  return true;
}

bool video_export_start_inline(gfx_handler_t *h, video_export_job_t *job, const video_export_options_t *o,
                               const char *path, const char *progress_path) {
  if (!h || !job || !o || !path || !*path || !h->level || job->active) return false;
  job->status[0] = '\0';
  int tps = game_ticks_per_second(&h->game_host);
  if (o->width < 2 || o->height < 2 || (o->width & 1) || (o->height & 1) ||
      o->fps_num <= 0 || o->fps_den <= 0 || !range_valid(o) || tps <= 0 ||
      o->codec < 0 || o->codec > 2 || (o->bit_depth != 8 && o->bit_depth != 10) ||
      o->quality_mode < 0 || o->quality_mode > 1 || o->preset < 0 || o->preset > 2 || o->hardware < 0 || o->hardware > 1 ||
      o->quality < 0 || o->quality > 51 || o->bitrate_kbps <= 0 ||
      (uint64_t)o->width * o->height > SIZE_MAX / 4u) {
    snprintf(job->status, sizeof(job->status), "Invalid range or video settings");
    return false;
  }
  if (game_host_active_id(&h->game_host) && strcmp(game_host_active_id(&h->game_host), "sm64") == 0 &&
      (o->width > 4096 || o->height > 4096)) {
    snprintf(job->status, sizeof(job->status), "SM64 render targets are limited to 4096 pixels per axis");
    return false;
  }
#ifndef FT_HAS_FFMPEG
  (void)path;
  snprintf(job->status, sizeof(job->status), "MP4 export requires FFmpeg development libraries at build time");
  return false;
#else
  memset(job, 0, sizeof(*job));
  job->options = *o;
  if (progress_path && snprintf(job->progress_path, sizeof(job->progress_path), "%s", progress_path) >=
                           (int)sizeof(job->progress_path)) return false;
  if (snprintf(job->path, sizeof(job->path), "%s", path) >= (int)sizeof(job->path) ||
      snprintf(job->temporary_path, sizeof(job->temporary_path), "%s.part.mp4", path) >= (int)sizeof(job->temporary_path)) {
    snprintf(job->status, sizeof(job->status), "Output path is too long");
    return false;
  }
  // The range is inclusive: emit samples at start + n/fps while the sample
  // does not pass the end. Its duration is the number of encoded frames.
  job->frame_count = video_export_frame_count(o);
  if (job->frame_count <= 0) return false;
  job->encoder = encoder_open(o, gfx_export_is_bgra(h), job->temporary_path, job->status, sizeof(job->status));
  if (!job->encoder) {
    fs_remove(job->temporary_path);
    return false;
  }
  log_info("Video", "Encoding with %s", ((encoder_t *)job->encoder)->name);
  job->old_tick = h->user_interface.timeline.current_tick;
  h->user_interface.timeline.is_playing = false;
  h->user_interface.timeline.is_reversing = false;
  job->active = true;
  job->started_at = glfwGetTime();
  snprintf(job->status, sizeof(job->status), "Rendering frame 0 / %lld", (long long)job->frame_count);
  return true;
#endif
}

double video_export_elapsed(const video_export_job_t *job) {
  return job->active ? glfwGetTime() - job->started_at : job->elapsed;
}

double video_export_speed(const video_export_job_t *job) {
  const double elapsed = video_export_elapsed(job);
  const video_export_options_t *o = &job->options;
  if (elapsed <= 0.0 || o->fps_num <= 0) return 0.0;
  return (double)job->frame_index * o->fps_den / o->fps_num / elapsed;
}

void video_export_format_duration(double seconds, char *out, size_t size) {
  if (!(seconds >= 0.0)) seconds = 0.0;
  if (seconds < 60.0) {
    snprintf(out, size, "%.1fs", seconds);
    return;
  }
  const long long whole = (long long)seconds;
  if (whole < 3600) snprintf(out, size, "%lld:%02lld", whole / 60, whole % 60);
  else snprintf(out, size, "%lld:%02lld:%02lld", whole / 3600, whole / 60 % 60, whole % 60);
}

static void write_finished_status(video_export_job_t *job) {
  // A background worker's last progress report may predate its final frame.
  job->frame_index = job->frame_count;
  char took[32];
  video_export_format_duration(job->elapsed, took, sizeof(took));
  snprintf(job->status, sizeof(job->status), "Wrote %s in %s (%.2fx real time)", job->path, took,
           video_export_speed(job));
}

static void video_export_stop(gfx_handler_t *h, video_export_job_t *job, bool success) {
#ifdef FT_HAS_FFMPEG
  encoder_t *e = job->encoder;
  if (success) success = encoder_finish(e, job->frame_count);
  encoder_close(e);
#endif
  job->encoder = NULL;
  if (success) success = fs_replace(job->temporary_path, job->path);
  if (!success) fs_remove(job->temporary_path);
  job->active = false;
  job->finished = success;
  job->failed = !success && !job->cancelled;
  h->user_interface.timeline.current_tick = job->old_tick;
  job->elapsed = glfwGetTime() - job->started_at;
  if (success) write_finished_status(job);
  else if (job->cancelled) snprintf(job->status, sizeof(job->status), "Render cancelled");
  else snprintf(job->status, sizeof(job->status), "MP4 render failed");
}

static void write_render_progress(const video_export_job_t *job) {
  if (!job->progress_path[0]) return;
  char temporary[sizeof(job->progress_path) + 8];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp", job->progress_path) >= (int)sizeof(temporary)) return;
  FILE *file = fs_open(temporary, "wb");
  if (!file) return;
  bool wrote = fprintf(file, "%lld %lld\n", (long long)job->frame_index,
                       (long long)job->frame_count) > 0;
  if (fclose(file) == 0 && wrote) fs_replace(temporary, job->progress_path);
  else fs_remove(temporary);
}

static void poll_render_worker(video_export_job_t *job) {
  FILE *progress = fs_open(job->progress_path, "rb");
  if (progress) {
    long long index, count;
    if (fscanf(progress, "%lld %lld", &index, &count) == 2 && index >= 0 && count > 0) {
      job->frame_index = index;
      job->frame_count = count;
      if (!job->cancelled)
        snprintf(job->status, sizeof(job->status), "Rendering in background: %lld / %lld", index, count);
    }
    fclose(progress);
  }
  bool done = false, success = false;
#ifdef _WIN32
  HANDLE process = (HANDLE)job->worker_handle;
  DWORD wait = WaitForSingleObject(process, 0);
  if (wait == WAIT_OBJECT_0) {
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    success = code == 0;
    done = true;
    CloseHandle(process);
  }
#else
  int code = 0;
  pid_t result = waitpid((pid_t)job->worker_handle, &code, WNOHANG);
  if (result != 0) {
    done = true;
    success = result > 0 && WIFEXITED(code) && WEXITSTATUS(code) == 0;
  }
#endif
  if (!done) return;
  job->worker_handle = 0;
  job->active = false;
  job->finished = success && !job->cancelled;
  job->failed = !success && !job->cancelled;
  fs_remove(job->snapshot_path);
  fs_remove(job->progress_path);
  if (!success || job->cancelled) fs_remove(job->temporary_path);
  job->elapsed = glfwGetTime() - job->started_at;
  if (job->finished) write_finished_status(job);
  else if (job->cancelled) snprintf(job->status, sizeof(job->status), "Render cancelled");
  else snprintf(job->status, sizeof(job->status), "Background render failed");
}

void video_export_step(gfx_handler_t *h, video_export_job_t *job, void (*draw)(gfx_handler_t *, float)) {
  if (!h || !job || !job->active) return;
  if (job->worker_process) { poll_render_worker(job); return; }
  if (!draw) return;
#ifdef FT_HAS_FFMPEG
  const video_export_options_t *o = &job->options;
  // Frames step through camera time; the game is shown wherever the camera's
  // time remap puts it, frozen, slowed or reversed.
  const double seconds = o->start_time + (double)job->frame_index * o->fps_den / o->fps_num;
  const double tps = game_ticks_per_second(&h->game_host) > 0 ? game_ticks_per_second(&h->game_host) : 50.0;
  const double tick = fmax(0.0, camera_game_tick(&h->user_interface.camera_timeline, seconds, tps));
  const double current = ceil(tick);
  if (current > INT32_MAX) { video_export_stop(h, job, false); return; }
  h->user_interface.timeline.current_tick = (int)current;
  const float alpha = (float)(1.0 - (current - tick));
  job->sample_time = seconds;
  uint8_t *pixels = encoder_acquire(job->encoder);
  if (!pixels || !gfx_render_export_frame(h, (uint32_t)o->width, (uint32_t)o->height, draw, alpha, pixels)) {
    video_export_stop(h, job, false);
    return;
  }
  encoder_submit(job->encoder, job->frame_index);
  ++job->frame_index;
  // The editor polls this file; a few updates a second are plenty.
  if (job->frame_index % 16 == 0 || job->frame_index == job->frame_count) write_render_progress(job);
  snprintf(job->status, sizeof(job->status), "Rendering frame %lld / %lld",
           (long long)job->frame_index, (long long)job->frame_count);
  if (job->frame_index == job->frame_count) video_export_stop(h, job, true);
#else
  (void)draw;
#endif
}

void video_export_cancel(gfx_handler_t *h, video_export_job_t *job) {
  if (!job || !job->active) return;
  if (job->worker_process) {
    job->cancelled = true;
    snprintf(job->status, sizeof(job->status), "Stopping background render...");
#ifdef _WIN32
    TerminateProcess((HANDLE)job->worker_handle, 1);
#else
    kill((pid_t)job->worker_handle, SIGTERM);
#endif
    return;
  }
  job->cancelled = true;
  video_export_stop(h, job, false);
}
