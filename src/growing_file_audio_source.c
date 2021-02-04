//
//  growing_file_audio_source.c
//  ichabod
//
//  Created by Charley Robinson on 6/7/17.
//

#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>

#include "growing_file_audio_source.h"

// Workaround C++ issue with ffmpeg macro
#ifndef __clang__
#undef av_err2str
#define av_err2str(errnum) \
av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), \
AV_ERROR_MAX_STRING_SIZE, errnum)
#endif

struct audio_source_s {
  AVFormatContext* format_context;
  AVCodecContext* codec_context;
  AVCodec* codec;
  int stream_index;
  const char* file_path;
  int64_t last_pts_out;
  double initial_timestamp;
  double corrected_pts;
};

static int open_file_stream(struct audio_source_s* pthis)
{
  int ret;
  ret = avformat_open_input(&pthis->format_context, pthis->file_path,
                            NULL, NULL);
  if (ret < 0)
  {
    av_log(NULL, AV_LOG_ERROR, "Cannot open input file: %s\n", av_err2str(ret));
    return ret;
  }

  /* select appropriate stream */
  ret = av_find_best_stream(pthis->format_context,
                            AVMEDIA_TYPE_AUDIO,
                            -1, -1,
                            &pthis->codec, 0);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR,
           "Cannot find a video stream in the input file\n");
    return ret;
  }
  // prefer libopus over built-in opus
  if (AV_CODEC_ID_OPUS == pthis->codec->id &&
      strcmp("libopus", pthis->codec->name))
  {
    printf("Switch from %s to libopus\n", pthis->codec->name);
    pthis->codec = avcodec_find_decoder_by_name("libopus");
  }
  pthis->stream_index = ret;
  pthis->codec_context =
  pthis->format_context->streams[pthis->stream_index]->codec;

  assert(pthis->codec_context->sample_fmt = AV_SAMPLE_FMT_S16);
  pthis->codec_context->request_sample_fmt = AV_SAMPLE_FMT_S16;

  av_opt_set_int(pthis->codec_context, "refcounted_frames", 1, 0);

  /* init the decoder */
  ret = avcodec_open2(pthis->codec_context, pthis->codec, NULL);
  if (ret < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
  }

  return ret;
}

// Since we'll hit EOF many times during the life of this source, play the
// conservative route and reopen everything each time we revisit the stream,
// then seek to the last known position if possible.
static int reopen_file_stream(struct audio_source_s* pthis) {
  if (pthis->format_context) {
    avformat_close_input(&pthis->format_context);
    // codec context is quietly freed by the previous statement,
    // so we just treat our ref as a weak reference.
    pthis->codec_context = NULL;
  }
  int ret = open_file_stream(pthis);
  return ret;
}

void audio_source_alloc(struct audio_source_s** audio_source_out) {
  struct audio_source_s* pthis =
  (struct audio_source_s*)calloc(1, sizeof(struct audio_source_s));
  pthis->corrected_pts = -1;
  *audio_source_out = pthis;
}

void audio_source_free(struct audio_source_s* pthis) {
  free((void*)pthis->file_path);
  pthis->file_path = NULL;
  avformat_close_input(&pthis->format_context);
  free(pthis);
}

int audio_source_load_config(struct audio_source_s* pthis,
                             struct audio_source_config_s* config)
{
  if (pthis->file_path) {
    free((void*)pthis->file_path);
  }
  pthis->file_path = strdup(config->path);
  pthis->initial_timestamp = config->initial_timestamp;
  int ret = open_file_stream(pthis);
  return ret;
}

int audio_source_next_frame(struct audio_source_s* pthis, AVFrame** frame_out)
{
  int ret, got_frame = 0;
  AVPacket packet = { 0 };
  *frame_out = NULL;
  char tried_reopen = 0;

  /* pump packet reader until fifo is populated, or file ends */
  while (!got_frame) {
    ret = av_read_frame(pthis->format_context, &packet);
    if (AVERROR_EOF == ret && !tried_reopen) {
      ret = reopen_file_stream(pthis);
      tried_reopen = 1;
      continue;
    } else if (ret < 0) {
      printf("%s\n", av_err2str(ret));
      return ret;
    }

    AVFrame* frame = av_frame_alloc();
    got_frame = 0;
    if (packet.stream_index == pthis->stream_index &&
        packet.pts > pthis->last_pts_out)
    {
      ret = avcodec_decode_audio4(pthis->codec_context, frame,
                                  &got_frame, &packet);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error decoding audio: %s\n",
               av_err2str(ret));
      }

      if (got_frame) {
        frame->pts = av_frame_get_best_effort_timestamp(frame);

        // repair bad PTS values coming from Chrome's MediaRecorder webms.
        // TODO: File that with Chrome or otherwise figure out why these
        // timestamps are messed up
        int64_t original_pts = frame->pts;
        if (pthis->corrected_pts < 0) {
          pthis->corrected_pts = frame->pts;
        } else {
          AVStream* stream =
          pthis->format_context->streams[pthis->stream_index];
          double interval = (stream->time_base.den * frame->nb_samples) /
          pthis->codec_context->sample_rate;
          pthis->corrected_pts += interval;
          frame->pts = pthis->corrected_pts;
        }
        printf("audio source: extracted pts %lld corrected pts %lld\n",
               original_pts, frame->pts);
        if (llabs(frame->pts - original_pts) > 10) {
          printf("audio source: WARNING drifting audio PTS values\n");
        }
        pthis->last_pts_out = original_pts;
        *frame_out = frame;
      }
    } else {
      av_frame_free(&frame);
    }

    av_packet_unref(&packet);
  }

  return !got_frame;

}

const AVFormatContext* audio_source_get_format(struct audio_source_s* pthis) {
  return pthis->format_context;
}

const AVCodecContext* audio_source_get_codec(struct audio_source_s* pthis) {
  return pthis->codec_context;
}

double audio_source_get_initial_timestamp(struct audio_source_s* pthis) {
  return pthis->initial_timestamp;
}
