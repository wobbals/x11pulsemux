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
#include <libavutil/audio_fifo.h>
#include <uv.h>
#include "pulse_audio_source.h"
#include "resampler.h"
#include "pulse_audio_source.h"
}

#include <queue>
#include <map>

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

  std::map<int64_t, AVFrame*> frame_map;
  AVAudioFifo* sample_fifo;

  int64_t buffer_pts;
};

static int min_buffered_frames = 10;

static int pulse_worker_read_frame(struct pulse_s* pthis, AVFrame** frame_out) {
  int ret, got_frame = 0;
  AVPacket packet = { 0 };
  *frame_out = NULL;

  ret = av_read_frame(pthis->format_context, &packet);
  if (ret) {
    printf("pulse_worker_read_frame: err=%s\n", av_err2str(ret));
    return ret;
  }
  if (packet.stream_index != pthis->stream_index) {
    return AVERROR(EAGAIN);
  }

  ret = avcodec_send_packet(pthis->codec_context, &packet);
  av_packet_unref(&packet);
  if (ret) {
    printf("pulse_worker_read_frame decode error=%s\n", av_err2str(ret));
    return ret;
  }
  AVFrame* frame = av_frame_alloc();
  ret = avcodec_receive_frame(pthis->codec_context, frame);
  if (!ret) {
    printf("pulse_audio_src: extracted  %lld (diff %lld) nb_samples=%d\n",
           frame->pts, frame->pts - pthis->last_pts_read,
           frame->nb_samples);
    pthis->last_pts_read = frame->pts;
    *frame_out = frame;
  } else {
    av_frame_free(&frame);
  }
  return ret;
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
    // pulse frames are not always linear; push to a heap and pop once enough
    // buffers have passed that we are confident we won't see another out of
    // order insertion later on.
    pthis->frame_map[frame->pts] = frame;

    if (pthis->frame_map.size() < min_buffered_frames) {
      continue;
    }
    frame = pthis->frame_map.begin()->second;
    pthis->frame_map.erase(frame->pts);

    // Clock will drift if we're relying on pulseaudio to produce the correct
    // number of samples at the correct timestamps.
    if (frame->pts > pthis->buffer_pts) {
      // error adjustement
      size_t samples_buffered = av_audio_fifo_size(pthis->sample_fifo);
      AVRational time_base = pthis->stream->time_base;
      int64_t error = samples_buffered * time_base.den;
      error /= pthis->codec_context->sample_rate;
      error *= time_base.num;
      pthis->buffer_pts = frame->pts - error;
      // disallow negative pts values.
      pthis->buffer_pts = MAX(0, pthis->buffer_pts);
    }

    ret = av_audio_fifo_write(pthis->sample_fifo,
                        (void**)frame->data,
                        frame->nb_samples);
    av_frame_free(&frame);

    int read_samples = 1024; // todo: import from downstream encoder frame_size
    while (av_audio_fifo_size(pthis->sample_fifo) > read_samples) {
      frame = av_frame_alloc();
      frame->nb_samples = read_samples;
      frame->format = pthis->codec_context->sample_fmt;
      frame->channel_layout = pthis->codec_context->channel_layout;
      frame->sample_rate = pthis->codec_context->sample_rate;
      frame->pts = pthis->buffer_pts;
      AVRational time_base = pthis->stream->time_base;
      int64_t frame_length = time_base.den;
      frame_length *= read_samples;
      frame_length /= pthis->codec_context->sample_rate;
      frame_length /= time_base.num;
      pthis->buffer_pts += frame_length;
      ret = av_frame_get_buffer(frame, 0);
      assert(ret == 0);
      ret = av_audio_fifo_read(pthis->sample_fifo,
                               (void**)frame->data,
                               read_samples);

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
}

void pulse_alloc(struct pulse_s** pulse_out) {
  struct pulse_s* pthis = (struct pulse_s*) calloc(1, sizeof(struct pulse_s));
  uv_mutex_init(&pthis->queue_lock);
  pthis->queue = std::queue<AVFrame*>();
  pthis->frame_map = std::map<int64_t, AVFrame*>();
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

  // reorder samples in a fifo by holding on to nonlinear frames as they are
  // read from the device.
  pthis->sample_fifo =
  av_audio_fifo_alloc(pthis->codec_context->sample_fmt,
                      pthis->codec_context->channels,
                      pthis->codec_context->sample_rate * min_buffered_frames);

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
  printf("pulse_audio_src: pop frame pts=%lld\n", frame->pts);
  return ret;
}

int64_t pulse_get_head_ts(struct pulse_s* pthis) {
  int64_t ret;
  uv_mutex_lock(&pthis->queue_lock);
  if (pthis->queue.empty()) {
    ret = EAGAIN;
  } else {
    AVFrame* frame = pthis->queue.front();
    ret = frame->pts;
  }
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

double pulse_convert_frame_pts(struct pulse_s* pthis, int64_t from_pts) {
  AVRational time_base = pthis->stream->time_base;
  double pts = from_pts * time_base.num;
  pts /= (double)time_base.den;
  return pts;
}
