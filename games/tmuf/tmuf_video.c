// The game's video pictures (Bink, .bik: the Stadium's arrow signs, the
// circuit screens), decoded whole at load through FFmpeg's libavcodec: every
// frame as RGBA, bottom row first as the game keeps its pictures. The game
// plays them looped; tm_video_frame picks the frame for a time.

#include "tmuf_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef TM_HAS_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// the frames of the video at path: *out (count * w * h * 4 bytes, the caller
// frees it), its size and frame rate; 0 frames when it cannot be read
static uint32_t decode(const char *path, uint8_t **out, uint32_t *w, uint32_t *h, float *fps) {
  *out = NULL;
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return 0;
  uint32_t count = 0, cap = 0;
  AVCodecContext *ctx = NULL;
  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  struct SwsContext *sws = NULL;
  const int stream = avformat_find_stream_info(fmt, NULL) >= 0
                         ? av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)
                         : -1;
  const AVCodec *codec = stream >= 0 ? avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id) : NULL;
  if (codec && frame && packet && (ctx = avcodec_alloc_context3(codec)) != NULL &&
      avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) >= 0 && avcodec_open2(ctx, codec, NULL) >= 0) {
    const AVRational rate = fmt->streams[stream]->avg_frame_rate.num ? fmt->streams[stream]->avg_frame_rate
                                                                       : fmt->streams[stream]->r_frame_rate;
    *fps = rate.den ? (float)rate.num / (float)rate.den : 15.f;
    *w = (uint32_t)ctx->width, *h = (uint32_t)ctx->height;
    const size_t frame_size = (size_t)*w * *h * 4;
    // every packet, then the decoder drained: each loop ends
    bool reading = true;
    while (reading) {
      if (av_read_frame(fmt, packet) < 0) {
        reading = false;
        avcodec_send_packet(ctx, NULL);
      } else {
        const bool ours = packet->stream_index == stream;
        const int sent = ours ? avcodec_send_packet(ctx, packet) : 0;
        av_packet_unref(packet);
        if (!ours || sent < 0) continue;
      }
      while (avcodec_receive_frame(ctx, frame) == 0) {
        if (count == cap) {
          const uint32_t grown_cap = cap ? cap * 2 : 32;
          uint8_t *grown = realloc(*out, frame_size * grown_cap);
          if (!grown) continue;
          *out = grown, cap = grown_cap;
        }
        sws = sws_getCachedContext(sws, frame->width, frame->height, (enum AVPixelFormat)frame->format, (int)*w,
                                   (int)*h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws) continue;
        // bottom row first: write the rows from the frame's last
        uint8_t *dst = *out + frame_size * count + (size_t)(*h - 1) * *w * 4;
        uint8_t *planes[4] = {dst, NULL, NULL, NULL};
        const int strides[4] = {-(int)(*w * 4), 0, 0, 0};
        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, planes, strides);
        count++;
      }
    }
  }
  sws_freeContext(sws);
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);
  if (!count) {
    free(*out);
    *out = NULL;
  }
  return count;
}
#endif

bool tm_video_load(ft_game *game, const char *path, tm_video *out) {
  memset(out, 0, sizeof *out);
#ifdef TM_HAS_FFMPEG
  uint8_t *pixels = NULL;
  uint32_t w = 0, h = 0;
  float fps = 15.f;
  const uint32_t count = decode(path, &pixels, &w, &h, &fps);
  if (!count) return false;
  out->frames = calloc(count, sizeof *out->frames);
  if (!out->frames) {
    free(pixels);
    return false;
  }
  const size_t frame_size = (size_t)w * h * 4;
  for (uint32_t i = 0; i < count; i++) {
    const tg_image image = {TG_RGBA8, w, h, 1, 1, pixels + frame_size * i, frame_size};
    out->frames[i] = tg_texture_create(game->gpu, &image);
    if (!out->frames[i]) break;
    out->count = i + 1;
  }
  free(pixels);
  out->fps = fps;
  return out->count > 0;
#else
  (void)game, (void)path;
  return false;
#endif
}

void tm_video_free(ft_game *game, tm_video *video) {
  for (uint32_t i = 0; i < video->count; i++)
    tg_texture_destroy(game->gpu, video->frames[i]);
  free(video->frames);
  memset(video, 0, sizeof *video);
}

tg_texture tm_video_frame(const tm_video *video, double seconds) {
  if (!video->count) return 0;
  const double at = seconds > 0.0 ? seconds * (double)video->fps : 0.0;
  return video->frames[(uint64_t)at % video->count];
}
