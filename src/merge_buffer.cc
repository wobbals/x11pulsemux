//
//  merge_buffer.c
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

extern "C" {

#include "merge_buffer.h"

}

#include <queue>

struct merge_buffer_s {
  std::queue<AVFrame*> buf;
  int64_t first_timestamp;
};

void merge_buffer_alloc(struct merge_buffer_s** merge_buffer_out,
                        double pts_interval)
{
  struct merge_buffer_s* pthis = (struct merge_buffer_s*)
  calloc(1, sizeof(struct merge_buffer_s));
  pthis->buf = std::queue<AVFrame*>();
  pthis->first_timestamp = -1;
  *merge_buffer_out = pthis;
}

void merge_buffer_free(struct merge_buffer_s* pthis) {
  while (!pthis->buf.empty()) {
    AVFrame* frame = pthis->buf.front();
    av_frame_free(&frame);
    pthis->buf.pop();
  }
  free(pthis);
}

int merge_buffer_get_next(struct merge_buffer_s* pthis, AVFrame** frame_out)
{
  if (pthis->buf.size() < 2) {
    *frame_out = NULL;
    return EAGAIN;
  }
  *frame_out = pthis->buf.front();
  pthis->buf.pop();
  return 0;
}

void merge_buffer_consume(struct merge_buffer_s* pthis, AVFrame* frame)
{
  if (pthis->first_timestamp < 0) {
    pthis->first_timestamp = frame->pts;
  }
  pthis->buf.push(frame);
}

char merge_buffer_has_next(struct merge_buffer_s* pthis) {
  return pthis->buf.size() > 1;
}
