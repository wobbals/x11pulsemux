//
//  archive_mixer.c
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

extern "C" {

#include <assert.h>
#include <uv.h>
#include "archive_mixer.h"
#include "growing_file_audio_source.h"
#include "video_frame_buffer.h"
#include "audio_frame_converter.h"
#include "pulse_audio_source.h"
}

#include <map>
#include <string>

struct archive_mixer_s {
  double first_video_ts;
  double first_audio_ts;
  double min_buffer_time;
  struct pulse_s* pulse_audio;
  struct frame_converter_s* audio_frame_converter;
  struct frame_buffer_s* video_buffer;
  uv_mutex_t queue_lock;
  std::map<int64_t, AVFrame*> video_frame_queue;
  std::map<int64_t, AVFrame*> audio_frame_queue;

  AVFormatContext* format_out;
  AVCodecContext* audio_ctx_out;
  AVStream* audio_stream_out;
  AVCodecContext* video_ctx_out;
  AVStream* video_stream_out;

  size_t audio_size_estimated;
  size_t video_size_estimated;

};

#pragma mark - Private Utilities

static void audio_frame_queue_push_safe
(struct archive_mixer_s* pthis, AVFrame* frame)
{
  uv_mutex_lock(&pthis->queue_lock);
  pthis->audio_frame_queue[frame->pts] = frame;
  pthis->audio_size_estimated = pthis->audio_frame_queue.size();
  uv_mutex_unlock(&pthis->queue_lock);
}

static void video_frame_queue_push_safe
(struct archive_mixer_s* pthis, AVFrame* frame)
{
  uv_mutex_lock(&pthis->queue_lock);
  pthis->video_frame_queue[frame->pts] = frame;
  pthis->video_size_estimated = pthis->video_frame_queue.size();
  uv_mutex_unlock(&pthis->queue_lock);
}

// mergesort-style management of two queues at once. there's probably a cleaner
// way to do this, but I haven't thought of it yet.
static int frame_queue_pop_safe(struct archive_mixer_s* pthis,
                                AVFrame** frame,
                                enum AVMediaType* media_type)
{
  int64_t ahead_pts = -1;
  int64_t vhead_pts = -1;
  int64_t atail_pts = -1;
  int64_t vtail_pts = -1;
  *media_type = AVMEDIA_TYPE_UNKNOWN;
  AVFrame* audio_head = NULL;
  AVFrame* video_head = NULL;
  AVFrame* ret = NULL;
  uv_mutex_lock(&pthis->queue_lock);
  if (!pthis->audio_frame_queue.empty()) {
    auto audio_it = pthis->audio_frame_queue.begin();
    audio_head = audio_it->second;
    ahead_pts = audio_it->second->pts;
    auto audio_tail_it = pthis->audio_frame_queue.rbegin();
    atail_pts = audio_tail_it->second->pts;
  }
  if (!pthis->video_frame_queue.empty()) {
    auto video_it = pthis->video_frame_queue.begin();
    video_head = video_it->second;
    vhead_pts = video_it->second->pts;
    auto video_tail_it = pthis->video_frame_queue.rbegin();
    vtail_pts = video_tail_it->second->pts;
  }
  if (audio_head && video_head) {
    // articulate this comparison better: pts are presented in different units
    // so we need to rescale here before making a fair comparison.
    ret = (audio_head->pts < video_head->pts * 48) ? audio_head : video_head;
  }
  else if (audio_head) {
    ret = audio_head;
  } else if (video_head) {
    ret = video_head;
  }
  if (ret && audio_head == ret) {
    pthis->audio_frame_queue.erase(audio_head->pts);
    *media_type = AVMEDIA_TYPE_AUDIO;
  }
  if (ret && video_head == ret) {
    pthis->video_frame_queue.erase(video_head->pts);
    *media_type = AVMEDIA_TYPE_VIDEO;
  }
  pthis->video_size_estimated = pthis->video_frame_queue.size();
  pthis->audio_size_estimated = pthis->audio_frame_queue.size();
  uv_mutex_unlock(&pthis->queue_lock);
  printf("mixer: %zu audio %zu video frames in queue\n",
         pthis->audio_size_estimated, pthis->video_size_estimated);
  printf("mixer: audio head %lld tail %lld video head %lld tail %lld\n",
         ahead_pts, atail_pts, vhead_pts, vtail_pts);
  *frame = ret;
  return (NULL == ret);
}

static void setup_audio(struct archive_mixer_s* pthis, AVFrame* frame) {
  assert(pthis->first_audio_ts >= 0);

  pthis->first_audio_ts =
  (double)frame->pts / pulse_get_time_base(pthis->pulse_audio).den;

  struct frame_converter_config_s converter_config;
  converter_config.num_channels = pthis->audio_ctx_out->channels;
  converter_config.output_format = pthis->audio_ctx_out->sample_fmt;
  converter_config.sample_rate = pthis->audio_ctx_out->sample_rate;
  converter_config.samples_per_frame = pthis->audio_ctx_out->frame_size;
  converter_config.channel_layout = pthis->audio_ctx_out->channel_layout;
  converter_config.ts_offset = pthis->first_video_ts - pthis->first_audio_ts;
  frame_converter_create(&pthis->audio_frame_converter, &converter_config);
}

#pragma mark - Public API

int archive_mixer_create(struct archive_mixer_s** mixer_out,
                         struct archive_mixer_config_s* config)
{
  struct archive_mixer_s* pthis = (struct archive_mixer_s*)
  calloc(1, sizeof(struct archive_mixer_s));
  pthis->audio_frame_queue = std::map<int64_t, AVFrame*>();
  pthis->video_frame_queue = std::map<int64_t, AVFrame*>();
  pthis->first_video_ts = config->initial_timestamp;
  pthis->min_buffer_time = config->min_buffer_time;
  pthis->format_out = config->format_out;
  pthis->audio_ctx_out = config->audio_ctx_out;
  pthis->video_ctx_out = config->video_ctx_out;
  pthis->audio_stream_out = config->audio_stream_out;
  pthis->video_stream_out = config->video_stream_out;
  pthis->pulse_audio = config->pulse_audio;

  double pts_interval =
  (double)config->video_ctx_out->time_base.den / config->video_fps_out;
  frame_buffer_alloc(&pthis->video_buffer, pts_interval);
  uv_mutex_init(&pthis->queue_lock);
  *mixer_out = pthis;
  return 0;
}
void archive_mixer_free(struct archive_mixer_s* pthis) {
  uv_mutex_destroy(&pthis->queue_lock);
  free(pthis);
}

void archive_mixer_drain_audio(struct archive_mixer_s* pthis) {
  int ret;
  AVFrame* frame = NULL;
  while (pulse_has_next(pthis->pulse_audio)) {
    ret = pulse_get_next(pthis->pulse_audio, &frame);
    if (ret) {
      continue;
    }
    // this is the last time we'll see the original timestamp from pulse.
    // use it to synchronize with video sources later.
    if (!pthis->first_audio_ts) {
      setup_audio(pthis, frame);
    }
    double frame_ts = pulse_convert_frame_pts(pthis->pulse_audio, frame->pts);
    frame_converter_consume(pthis->audio_frame_converter, frame, frame_ts);
    av_frame_free(&frame);
  }

  if (pthis->audio_frame_converter) {
    // finally, pull from frame converter into audio queue
    frame = NULL;
    ret = frame_converter_get_next(pthis->audio_frame_converter, &frame);
    while (!ret) {
      if (frame) {
        audio_frame_queue_push_safe(pthis, frame);
      }
      ret = frame_converter_get_next(pthis->audio_frame_converter, &frame);
    }
  }
}

void archive_mixer_consume_video(struct archive_mixer_s* pthis,
                                 AVFrame* frame, double timestamp)
{
  frame->pts = (timestamp - (1000 * pthis->first_video_ts));
  frame_buffer_consume(pthis->video_buffer, frame);
  while (frame_buffer_has_next(pthis->video_buffer)) {
    int ret = frame_buffer_get_next(pthis->video_buffer, &frame);
    if (!ret && frame) {
      video_frame_queue_push_safe(pthis, frame);
    }
  }
}

char archive_mixer_has_next(struct archive_mixer_s* pthis) {
  char ret = 0;
  uv_mutex_lock(&pthis->queue_lock);
  // Pop any and all data in the mixer.
  ret = !pthis->audio_frame_queue.empty() || !pthis->video_frame_queue.empty();
  // Don't pop until both queues are populated
  //ret = !pthis->audio_frame_queue.empty() && !pthis->video_frame_queue.empty();
  
  // EXPERIMENT: buffer a bunch of data to try and relieve interleaving pressure
  // on the RTMP output.
  //ret = pthis->audio_frame_queue.size() > 100 && pthis->video_frame_queue.size() > 60;
  uv_mutex_unlock(&pthis->queue_lock);
  return ret;
}

int archive_mixer_get_next(struct archive_mixer_s* pthis, AVFrame** frame_out,
                           enum AVMediaType* media_type)
{
  int ret = 0;
  ret = frame_queue_pop_safe(pthis, frame_out, media_type);
  return ret;
}

size_t archive_mixer_get_size(struct archive_mixer_s* pthis) {
  return pthis->video_size_estimated + pthis->audio_size_estimated;
}
