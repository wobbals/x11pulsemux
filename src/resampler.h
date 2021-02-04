//
//  resampler.h
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

#ifndef resampler_h
#define resampler_h

#include <libavformat/avformat.h>

struct resampler_s;
struct resampler_config_s {
  enum AVSampleFormat format_in;
  enum AVSampleFormat format_out;
  int sample_rate_in;
  int sample_rate_out;
  int nb_channels_in;
  int nb_channels_out;
  uint64_t channel_layout_in;
  uint64_t channel_layout_out;
};

void resampler_alloc(struct resampler_s** resampler_out);
void resampler_free(struct resampler_s* resmapler);
int resampler_load_config(struct resampler_s* resampler,
                          struct resampler_config_s* config);
int resampler_convert(struct resampler_s* resampler, AVFrame* frame_in,
                      AVFrame** frame_out);

#endif /* resampler_h */
