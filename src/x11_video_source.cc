//
//  x11_video_source.c
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

extern "C" {

#include <assert.h>
#include <stdlib.h>
#include <uv.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <signal.h>
#include "x11_video_source.h"

}

#include <queue>

// Workaround C++ issue with ffmpeg macro
#ifndef __clang__
#undef av_err2str
#define av_err2str(errnum) \
av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), \
AV_ERROR_MAX_STRING_SIZE, errnum)
#endif

struct x11_s {
  std::queue<AVFrame*> queue;
  uv_thread_t worker_thread;
  uv_mutex_t queue_lock;
  volatile sig_atomic_t interrupted;
  AVInputFormat* input_format;
  AVFormatContext* format_context;
  AVCodecContext* codec_context;
  AVCodec* codec;
  struct SwsContext* sws_ctx;
  int stream_index;
  AVStream* stream;
  int64_t last_pts_read;
};

void x11_alloc(struct x11_s** x11_out) {
  struct x11_s* pthis = (struct x11_s*)calloc(1, sizeof(struct x11_s));
  uv_mutex_init(&pthis->queue_lock);
  pthis->queue = std::queue<AVFrame*>();
  *x11_out = pthis;
}

void x11_free(struct x11_s* x11) {
  free(x11);
}

static int _read_frame(struct x11_s* pthis, AVFrame** frame_out) {
  int ret, have_frame = 0;
  AVPacket packet = { 0 };
  *frame_out = NULL;
  
  while (!have_frame) {
    ret = av_read_frame(pthis->format_context, &packet);
    if (ret < 0) {
      printf("x11_read_frame: %s\n", av_err2str(ret));
      return ret;
    }
    
    AVFrame* frame = av_frame_alloc();
    if (packet.stream_index == pthis->stream_index) {
      ret = avcodec_send_packet(pthis->codec_context, &packet);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error decoding video: %s\n",
               av_err2str(ret));
      }
      
      ret = avcodec_receive_frame(pthis->codec_context, frame);
      if (!ret) {
        frame->pts = av_frame_get_best_effort_timestamp(frame);
        printf("x11_video_source: extracted %lld (diff %lld)\n",
               frame->pts, frame->pts - pthis->last_pts_read);
        pthis->last_pts_read = frame->pts;
        *frame_out = frame;
        have_frame = 1;
      } else if (ret != AVERROR(EAGAIN)) {
        av_frame_free(&frame);
        return ret;
      }
    }
    av_packet_unref(&packet);
  }
  return !have_frame;
}


// Converts RGB frame to YUV before passing downstream.
AVFrame* _convert_frame(struct x11_s* pthis, AVFrame* frame) {
  int ret;
  pthis->sws_ctx = sws_getCachedContext(pthis->sws_ctx,
                                        frame->width, frame->height,
                                        (enum AVPixelFormat) frame->format,
                                        frame->width, frame->height,
                                        AV_PIX_FMT_YUV420P,
                                        SWS_FAST_BILINEAR,
                                        NULL, NULL, NULL);

  AVFrame* converted_frame = av_frame_alloc();
//  ret = av_image_alloc(converted_frame->data, converted_frame->linesize,
//                       frame->width, frame->height, AV_PIX_FMT_YUV420P, 16);

  converted_frame->width = frame->width;
  converted_frame->height = frame->height;
  converted_frame->format = AV_PIX_FMT_YUV420P;
  converted_frame->pict_type = frame->pict_type;
  converted_frame->key_frame = frame->key_frame;
  converted_frame->pts = frame->pts;
  converted_frame->pkt_dts = frame->pkt_dts;
  converted_frame->pkt_duration = frame->pkt_duration;
  // fun fact about av_image_alloc: the references on underlying buffers do not
  // get passed along. av_image_fill_arrays does the right thing and keeps
  // av_frame_free on this frame working as expected.
  ret = av_frame_get_buffer(converted_frame, 16);

  sws_scale(pthis->sws_ctx, frame->data, frame->linesize, 0,
            frame->height, converted_frame->data, converted_frame->linesize);
  av_frame_free(&frame);
  return converted_frame;
}

void x11grab_main(void* p) {
  int ret;
  AVFrame* frame = NULL;
  struct x11_s* pthis = (struct x11_s*)p;
  while (!pthis->interrupted) {
    ret = _read_frame(pthis, &frame);
    if (ret || !frame) {
      continue;
    }
    if (!ret) {
      frame = _convert_frame(pthis, frame);
      uv_mutex_lock(&pthis->queue_lock);
      pthis->queue.push(frame);
      uv_mutex_unlock(&pthis->queue_lock);
      frame = NULL;
    }
  }
}

int x11_start(struct x11_s* pthis, struct x11_grab_config_s* config) {
  int ret;
  pthis->input_format = av_find_input_format("x11grab");
  if (!pthis->input_format) {
    printf("x11_start: x11grab input device not found\n");
    return -1;
  }
  AVDictionary *opts = NULL;
  av_dict_set(&opts, "video_size", "2560x1440", 0);
  av_dict_set(&opts, "framerate", "ntsc", 0);
  ret = avformat_open_input(&pthis->format_context, config->device_name,
                            pthis->input_format, &opts);
  av_dict_free(&opts);

  if (ret) {
    printf("failed to open input %s\n", pthis->input_format->name);
    return ret;
  }

  ret = avformat_find_stream_info(pthis->format_context, NULL);
  if (ret < 0) {
    printf("Could not find stream information\n");
    return ret;
  }
  
  ret = av_find_best_stream(pthis->format_context,
                            AVMEDIA_TYPE_VIDEO,
                            -1, -1,
                            &pthis->codec, 0);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR,
           "Cannot find a video stream in the input file\n");
    return ret;
  }
  pthis->stream_index = ret;
  pthis->stream = pthis->format_context->streams[pthis->stream_index];
  pthis->codec = avcodec_find_decoder(pthis->stream->codecpar->codec_id);
  if (!pthis->codec) {
    printf("Failed to find audio codec\n");
    return AVERROR(EINVAL);
  }
  
  // Allocate a codec context for the decoder
  pthis->codec_context = avcodec_alloc_context3(pthis->codec);
  
  ret = avcodec_parameters_to_context(pthis->codec_context,
                                      pthis->stream->codecpar);
  if (ret < 0) {
    printf("Failed to copy stream codec parameters to codec context\n");
    return ret;
  }
  
  av_opt_set_int(pthis->codec_context, "refcounted_frames", 1, 0);
  
  /* init the decoder */
  ret = avcodec_open2(pthis->codec_context, pthis->codec, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "x11_open: cannot open video decoder\n");
  }
  
  pthis->interrupted = 0;
  return uv_thread_create(&pthis->worker_thread, x11grab_main, pthis);
}

int x11_stop(struct x11_s* pthis) {
  pthis->interrupted = 1;
  return uv_thread_join(&pthis->worker_thread);
}

char x11_has_next(struct x11_s* pthis) {
  char ret = 0;
  uv_mutex_lock(&pthis->queue_lock);
  ret = pthis->queue.size() > 0;
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

int x11_get_next(struct x11_s* pthis, AVFrame** frame_out) {
  AVFrame* frame = NULL;
  int ret;
  uv_mutex_lock(&pthis->queue_lock);
  if (pthis->queue.empty()) {
    ret = EAGAIN;
  } else {
    frame = pthis->queue.front();
    pthis->queue.pop();
    ret = 0;
  }
  uv_mutex_unlock(&pthis->queue_lock);
  *frame_out = frame;
  return ret;
}

double x11_get_initial_ts(struct x11_s* pthis) {
  return (double)pthis->stream->start_time /
  (double)pthis->stream->time_base.den;
}

int64_t x11_get_first_pts(struct x11_s* pthis) {
  return pthis->stream->start_time;
}

double x11_convert_pts(struct x11_s* pthis, int64_t pts) {
  double result = pts;
  AVRational time_base = pthis->stream->time_base;
  result *= time_base.num;
  result /= time_base.den;
  return result;
}
