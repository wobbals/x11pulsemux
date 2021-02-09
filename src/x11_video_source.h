//
//  x11_video_source.h
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

#ifndef x11_video_source_h
#define x11_video_source_h

#include <libavutil/frame.h>

struct x11_grab_config_s {
  const char* device_name;
  int width;
  int height;
};

struct x11_s;

void x11_alloc(struct x11_s** x11_out);
void x11_free(struct x11_s* x11);

int x11_start(struct x11_s* x11, struct x11_grab_config_s* config);
int x11_stop(struct x11_s* x11);

char x11_has_next(struct x11_s* x11);
int x11_get_next(struct x11_s* x11, AVFrame** frame_out);
int64_t x11_get_head_ts(struct x11_s* pthis);
double x11_convert_pts(struct x11_s* pthis, int64_t pts);

#endif /* x11_video_source_h */
