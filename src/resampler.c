//
//  resampler.c
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

#include "resampler.h"
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

// Workaround C++ issue with ffmpeg macro
#ifndef __clang__
#undef av_err2str
#define av_err2str(errnum) \
av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), \
AV_ERROR_MAX_STRING_SIZE, errnum)
#endif

struct resampler_s {
  struct SwrContext* swr_ctx;
  struct resampler_config_s config;
};

void resampler_alloc(struct resampler_s** resampler_out) {
  struct resampler_s* pthis = (struct resampler_s*)
  calloc(1, sizeof(struct resampler_s));
  pthis->swr_ctx = swr_alloc();
  *resampler_out = pthis;
}

void resampler_free(struct resampler_s* pthis) {
  swr_free(&pthis->swr_ctx);
  free(pthis);
}

int resampler_load_config(struct resampler_s* pthis,
                          struct resampler_config_s* config)
{
  memcpy(&pthis->config, config, sizeof(struct resampler_config_s));
  int ret;
  /* set options */
  ret = av_opt_set_int(pthis->swr_ctx, "in_channel_layout",
                       config->channel_layout_in, 0);
  if (ret) {
    printf("icl: %s\n", av_err2str(ret));
  }
  ret = av_opt_set_int(pthis->swr_ctx, "out_channel_layout",
                       config->channel_layout_out, 0);
  if (ret) {
    printf("ocl: %s\n", av_err2str(ret));
  }
  ret = av_opt_set_int(pthis->swr_ctx, "in_sample_fmt",
                       config->format_in, 0);
  if (ret) {
    printf("isf: %s\n", av_err2str(ret));
  }
  ret = av_opt_set_int(pthis->swr_ctx, "in_sample_rate",
                       config->sample_rate_in, 0);
  if (ret) {
    printf("isr: %s\n", av_err2str(ret));
  }
  ret = av_opt_set_int(pthis->swr_ctx, "out_sample_rate",
                       config->sample_rate_out, 0);
  if (ret) {
    printf("osr: %s\n", av_err2str(ret));
  }
  ret = av_opt_set_int(pthis->swr_ctx, "out_sample_fmt",
                       config->format_out, 0);
  if (ret) {
    printf("osf: %s\n", av_err2str(ret));
  }

  /* initialize the resampling context */
  ret = swr_init(pthis->swr_ctx);
  if (ret < 0) {
    printf("Failed to initialize the resampling context\n");
    return ret;
  }

  return ret;
}

int resampler_convert(struct resampler_s* pthis, AVFrame* frame_in,
                      AVFrame** frame_out)
{
  AVFrame* output = av_frame_alloc();
  output->format = pthis->config.format_out;
  output->channel_layout = pthis->config.channel_layout_out;
  output->channels = pthis->config.nb_channels_out;
  output->nb_samples = frame_in->nb_samples;
  output->sample_rate = pthis->config.sample_rate_out;
  output->pts = swr_next_pts(pthis->swr_ctx, frame_in->pts);
  int ret = av_frame_get_buffer(output, 0);
  if (ret) {
    printf("resmpler: Cannot get output buffer\n");
  }
  ret = swr_config_frame(pthis->swr_ctx, output, frame_in);
//  if (ret) {
//    printf("resampler: reconfig %s\n", av_err2str(ret));
//  }
  ret = swr_convert_frame(pthis->swr_ctx, output, frame_in);
  if (!ret) {
    *frame_out = output;
  } else {
    printf("resampler: %s\n", av_err2str(ret));
    *frame_out = NULL;
    av_frame_free(&output);
  }
  return ret;
}
