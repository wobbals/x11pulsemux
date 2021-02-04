//
//  pulse_audio_source.c
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

extern "C" {
#include <assert.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <uv.h>
#include "pulse_audio_source.h"
#include "resampler.h"
#include "pulse_audio_source.h"
}

#include <queue>

// Workaround C++ issue with ffmpeg macro
#ifndef __clang__
#undef av_err2str
#define av_err2str(errnum) \
av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), \
AV_ERROR_MAX_STRING_SIZE, errnum)
#endif

struct pulse_s {
  AVInputFormat* input_format;
  AVFormatContext* format_context;
  AVCodecContext* codec_context;
  AVCodec* codec;
  int stream_index;
  AVStream* stream;
  uv_thread_t worker_thread;
  uv_mutex_t queue_lock;
  std::queue<AVFrame*> queue;
  char is_interrupted;
  char is_running;
  int64_t initial_timestamp;
  int64_t last_pts_read;
  struct resampler_s* resampler;

  void (*on_audio_data)(struct pulse_s* pulse, void* p);
  void* audio_data_cb_p;

};

static int pulse_worker_read_frame(struct pulse_s* pthis, AVFrame** frame_out) {
  int ret, got_frame = 0;
  AVPacket packet = { 0 };
  *frame_out = NULL;

  /* pump packet reader until fifo is populated, or file ends */
  while (!got_frame) {
    ret = av_read_frame(pthis->format_context, &packet);
    if (ret < 0) {
      printf("%s\n", av_err2str(ret));
      return ret;
    }

    AVFrame* frame = av_frame_alloc();
    got_frame = 0;
    if (packet.stream_index == pthis->stream_index)
    {
      ret = avcodec_decode_audio4(pthis->codec_context, frame,
                                  &got_frame, &packet);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error decoding audio: %s\n",
               av_err2str(ret));
      }

      if (got_frame) {
        frame->pts = av_frame_get_best_effort_timestamp(frame);
        printf("audio source: extracted %lld (diff %lld) n_samples=%d\n",
               frame->pts, frame->pts - pthis->last_pts_read,
               frame->nb_samples);
        pthis->last_pts_read = frame->pts;
        *frame_out = frame;
      }
    } else {
      av_frame_free(&frame);
    }

    av_packet_unref(&packet);
  }

  return !got_frame;

}

static void pulse_worker_main(void* p) {
  int ret;
  AVFrame* frame;
  AVFrame* resampled_frame;
  struct pulse_s* pthis = (struct pulse_s*)p;
  pthis->is_running = 1;
  while (!pthis->is_interrupted) {
    ret = pulse_worker_read_frame(pthis, &frame);
    if (ret || !frame) {
      continue;
    }
    ret = resampler_convert(pthis->resampler, frame, &resampled_frame);
    av_frame_free(&frame);
    if (!ret) {
      uv_mutex_lock(&pthis->queue_lock);
      pthis->queue.push(resampled_frame);
      uv_mutex_unlock(&pthis->queue_lock);
      if (pthis->on_audio_data) {
        pthis->on_audio_data(pthis, pthis->audio_data_cb_p);
      }
    }
  }
}

void pulse_alloc(struct pulse_s** pulse_out) {
  struct pulse_s* pthis = (struct pulse_s*) calloc(1, sizeof(struct pulse_s));
  uv_mutex_init(&pthis->queue_lock);
  pthis->queue = std::queue<AVFrame*>();
  pthis->is_interrupted = 0;
  resampler_alloc(&pthis->resampler);
  *pulse_out = pthis;
}

void pulse_free(struct pulse_s* pthis) {
  while (!pthis->queue.empty()) {
    AVFrame* frame = pthis->queue.front();
    pthis->queue.pop();
    av_frame_free(&frame);
  }
  uv_mutex_destroy(&pthis->queue_lock);
  avcodec_free_context(&pthis->codec_context);
  avformat_close_input(&pthis->format_context);
  resampler_free(pthis->resampler);
  free(pthis);
}

void pulse_load_config(struct pulse_s* pthis, struct pulse_config_s* config) {
  pthis->on_audio_data = config->on_audio_data;
  pthis->audio_data_cb_p = config->audio_data_cb_p;
}

int pulse_start(struct pulse_s* pthis) {
  int ret;
  pthis->input_format = av_find_input_format("pulse");
  if (!pthis->input_format) {
    printf("can't find pulse input format. is it registered?\n");
    return -1;
  }
  // if developing on OSX: you'll need to find the right interface to capture
  // meaningful audio. use `pactl list sources` and put the interface number
  // in that corresponds to your mic or loopback device. just please don't
  // commit that change :-)
  // const char* input_device = "6";
  const char* input_device = "default";

  ret = avformat_open_input(&pthis->format_context, input_device,
                            pthis->input_format, NULL);
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
                            AVMEDIA_TYPE_AUDIO,
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

  assert(pthis->codec->sample_fmts[0] == AV_SAMPLE_FMT_S16);
  pthis->codec_context->request_sample_fmt = pthis->codec->sample_fmts[0];
  // TODO: There's no reason we shouldn't support stereo audio here
  pthis->codec_context->channels = 2;
  pthis->codec_context->request_channel_layout = AV_CH_LAYOUT_STEREO;
  pthis->codec_context->channel_layout = AV_CH_LAYOUT_STEREO;

  /* init the decoder */
  ret = avcodec_open2(pthis->codec_context, pthis->codec, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
  }

  struct resampler_config_s config;
  config.channel_layout_in = pthis->codec_context->channel_layout;
  config.channel_layout_in = AV_CH_LAYOUT_STEREO;
  config.channel_layout_out = AV_CH_LAYOUT_STEREO;
  config.format_in = pthis->codec_context->sample_fmt;
  config.format_out = AV_SAMPLE_FMT_FLTP;
  config.sample_rate_in = pthis->codec_context->sample_rate;
  config.sample_rate_out = 48000;
  config.nb_channels_in = pthis->codec_context->channels;
  config.nb_channels_out = 2;
  resampler_load_config(pthis->resampler, &config);

  uv_thread_create(&pthis->worker_thread, pulse_worker_main, pthis);

  return ret;
}

int pulse_stop(struct pulse_s* pthis) {
  pthis->is_interrupted = 1;
  int ret = uv_thread_join(&pthis->worker_thread);
  pthis->is_running = 0;
  return ret;
}

char pulse_is_running(struct pulse_s* pthis) {
  return pthis->is_running;
}

char pulse_has_next(struct pulse_s* pthis) {
  char ret = 0;
  uv_mutex_lock(&pthis->queue_lock);
  ret = pthis->queue.size() > 0;
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

int pulse_get_next(struct pulse_s* pthis, AVFrame** frame_out) {
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

double pulse_get_initial_ts(struct pulse_s* pthis) {
  return (double)pthis->stream->start_time /
  (double)pthis->stream->time_base.den;
}

double pulse_convert_frame_pts(struct pulse_s* pthis, int64_t from_pts) {
  double pts = from_pts;
  pts /= (double)pthis->stream->time_base.den;
  pts -= pulse_get_initial_ts(pthis);
  return pts;
}

AVRational pulse_get_time_base(struct pulse_s* pthis) {
  return pthis->stream->time_base;
}
