//
//  pulse_audio_source.h
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

#ifndef pulse_audio_source_h
#define pulse_audio_source_h

#include <libavutil/frame.h>

/**
 * An audio stream source from pulse audio.
 */
struct pulse_s;

struct pulse_config_s {
  // notify when new data hits the queue
  void (*on_audio_data)(struct pulse_s* pulse, void* p);
  void* audio_data_cb_p;
};

void pulse_alloc(struct pulse_s** pulse_out);
void pulse_free(struct pulse_s* pulse);
void pulse_load_config(struct pulse_s* pulse, struct pulse_config_s* config);

int pulse_start(struct pulse_s* pulse);
int pulse_stop(struct pulse_s* pulse);
char pulse_is_running(struct pulse_s* pulse);

char pulse_has_next(struct pulse_s* pulse);
int pulse_get_next(struct pulse_s* pulse, AVFrame** frame_out);
int64_t pulse_get_head_ts(struct pulse_s* pthis);
double pulse_convert_pts(struct pulse_s* pulse, int64_t from_pts);

#endif /* pulse_audio_source_h */
