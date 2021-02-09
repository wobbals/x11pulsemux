
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <signal.h>
#include <uv.h>
#include "pulse_audio_source.h"
#include "x11_video_source.h"
#include "file_writer.h"
#include "muxer.h"

struct muxer_s {
  char* outfile_path;
  char* device_name;
  uv_thread_t worker_thread;
  volatile sig_atomic_t interrupted;
  struct pulse_s* pulse;
  struct x11_s* x11grab;
  struct file_writer_t* file_writer;
  struct archive_mixer_s* mixer;

  char audio_up;
  char video_up;
};

int setup_outputs(struct muxer_s* pthis, AVFrame* first_video_frame)
{
  int ret, width, height;
  width = first_video_frame->width;
  height = first_video_frame->height;
  ret = file_writer_alloc(&pthis->file_writer);
  if (ret) {
    printf("file_writer_alloc failed with %d\n", ret);
    return ret;
  }
  ret = file_writer_open(pthis->file_writer, pthis->outfile_path, width, height);
  if (ret) {
    printf("file_writer_open failed with %d\n", ret);
    return ret;
  }
  // late start (see also muxer_open)
//  ret = pulse_start(pthis->pulse);
  if (ret) {
    printf("failed to open pulse audio! ichabod will be silent.\n");
  }
  return ret;
}

void muxer_main(void* p) {
  int ret;
  struct muxer_s* pthis = (struct muxer_s*)p;
  printf("muxer main\n");
  int64_t first_pts = -1;
  while (!pthis->interrupted) {
    while (
      !pthis->interrupted && x11_has_next(pthis->x11grab)
      && x11_get_head_ts(pthis->x11grab) < pulse_get_head_ts(pthis->pulse)
    ) {
      pthis->video_up = 1;
      AVFrame* frame = NULL;
      ret = x11_get_next(pthis->x11grab, &frame);
      if (ret) {
        printf("muxer_main: x11_get_next failed with %d\n", ret);
        continue;
      }
      if (!pthis->file_writer) {
        setup_outputs(pthis, frame);
      }
      if (!pthis->audio_up) {
        printf("muxer_main: skip x11 frame (wait for audio)\n");
        av_frame_free(&frame);
        continue;
      }
      if (first_pts < 0) {
        first_pts = frame->pts;
      }
      int64_t adjusted_pts = frame->pts;
      adjusted_pts -= first_pts;
      double timestamp = x11_convert_pts(pthis->x11grab, adjusted_pts);
      ret = file_writer_push_video_frame(pthis->file_writer, frame, timestamp);
      if (ret) {
        printf("muxer_main: file_writer_push_video_frame failed with %d\n",
               ret);
      }
    }
    
    while (
      !pthis->interrupted && pulse_has_next(pthis->pulse)
      && pulse_get_head_ts(pthis->pulse) < x11_get_head_ts(pthis->x11grab)
    ) {
      pthis->audio_up = 1;
      AVFrame* frame = NULL;
      ret = pulse_get_next(pthis->pulse, &frame);
      if (ret) {
        continue;
      }
      if (!pthis->video_up) {
        printf("muxer_main: skip pulse frame (wait for video)\n");
        av_frame_free(&frame);
        continue;
      }
      if (first_pts < 0) {
        first_pts = frame->pts;
      }
      int64_t adjusted_pts = frame->pts;
      adjusted_pts -= first_pts;
      double timestamp = x11_convert_pts(pthis->x11grab, adjusted_pts);
      ret = file_writer_push_audio_frame(pthis->file_writer, frame, timestamp);
      if (ret && ret != AVERROR(EAGAIN)) {
        printf("muxer_main: file_writer_push_audio_frame failed with %d\n",
               ret);
      }
      av_frame_free(&frame);
    }
  }
  printf("muxer main: exit loop\n");
}

void muxer_initialize() {
  avdevice_register_all();
}

int muxer_open(struct muxer_s** muxer, struct muxer_config_s* config) {
  struct muxer_s* pthis = (struct muxer_s*)calloc(1, sizeof(struct muxer_s));
  pthis->outfile_path = calloc(strlen(config->outfile_path) + 1, 1);
  pthis->device_name = calloc(strlen(config->device_name) + 1, 1);
  strcpy(pthis->outfile_path, config->outfile_path);
  strcat(pthis->device_name, config->device_name);
  int ret;
  x11_alloc(&pthis->x11grab);
  struct x11_grab_config_s x11_config = { 0 };
  x11_config.device_name = config->device_name;
  // oops wire this up to the cli
  x11_config.width = 2560;
  x11_config.height = 1440;
  ret = x11_start(pthis->x11grab, &x11_config);
  if (ret) {
    printf("x11_start failed with %d\n", ret);
    return ret;
  }
  
  pulse_alloc(&pthis->pulse);
  ret = pulse_start(pthis->pulse);
  if (ret) {
    printf("pulse_start failed with %d\n", ret);
    return ret;
  }
  pthis->interrupted = 0;
  ret = uv_thread_create(&pthis->worker_thread, muxer_main, pthis);
  if (!ret) {
    *muxer = pthis;
  }
  return ret;
}

int muxer_close(struct muxer_s* pthis) {
  int ret;
  printf("muxer_close\n");
  pthis->interrupted = 1;
  ret = uv_thread_join(&pthis->worker_thread);
  if (ret) {
    printf("muxer_close: uv_thread_join failed with %d\n", ret);
  }
  ret = file_writer_close(pthis->file_writer);
  if (ret) {
    printf("muxer_close: file_writer_close failed with %d\n", ret);
  }
  ret = pulse_stop(pthis->pulse);
  if (ret) {
    printf("muxer_close: pulse_stop failed with %d\n", ret);
    return ret;
  }
  pulse_free(pthis->pulse);

  ret = x11_stop(pthis->x11grab);
  if (ret) {
    printf("muxer_close: x11_stop failed with %d\n", ret);
    return ret;
  }
  x11_free(pthis->x11grab);

  file_writer_free(pthis->file_writer);
  
  free(pthis->outfile_path);
  free(pthis);
  return ret;
}
