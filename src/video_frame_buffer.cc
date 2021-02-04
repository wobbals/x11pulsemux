//
//  video_frame_buffer.c
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

extern "C" {

#include "video_frame_buffer.h"

}

#include <queue>

struct frame_buffer_s {
  std::queue<AVFrame*> buf;
  double interval;
  double precise_tail_pts;
};

void frame_buffer_alloc(struct frame_buffer_s** frame_buffer_out,
                        double pts_interval)
{
  struct frame_buffer_s* pthis = (struct frame_buffer_s*)
  calloc(1, sizeof(struct frame_buffer_s));
  pthis->buf = std::queue<AVFrame*>();
  pthis->interval = pts_interval;
  *frame_buffer_out = pthis;
}

void frame_buffer_free(struct frame_buffer_s* pthis) {
  while (!pthis->buf.empty()) {
    AVFrame* frame = pthis->buf.front();
    av_frame_free(&frame);
    pthis->buf.pop();
  }
  free(pthis);
}

int frame_buffer_get_next(struct frame_buffer_s* pthis, AVFrame** frame_out)
{
  if (pthis->buf.size() < 2) {
    *frame_out = NULL;
    return EAGAIN;
  }
  *frame_out = pthis->buf.front();
  pthis->buf.pop();
  return 0;
}

void frame_buffer_consume(struct frame_buffer_s* pthis, AVFrame* frame)
{
  if (pthis->buf.empty()) {
    pthis->buf.push(frame);
    pthis->precise_tail_pts = frame->pts;
    return;
  }
  double next_tail_ts = pthis->precise_tail_pts + pthis->interval;
  double two_frames_late = next_tail_ts + pthis->interval;
  double half_frame_early = next_tail_ts - (pthis->interval / 2);
  AVFrame* tail = pthis->buf.back();
  // Frame is late: copy old tail if more than 1 frame late
  if (frame->pts > two_frames_late) {
    // this may need to become a manual deep copy if we're not refcounting
    AVFrame* new_tail = av_frame_clone(tail);
    new_tail->pts = next_tail_ts;
    pthis->precise_tail_pts = next_tail_ts;
    pthis->buf.push(new_tail);
    frame_buffer_consume(pthis, frame);
  } else if (frame->pts > next_tail_ts) {
    // frame is late, but not that late: mangle it and accept as is.
    frame->pts = next_tail_ts;
    pthis->precise_tail_pts = next_tail_ts;
    pthis->buf.push(frame);
  } else if (frame->pts > half_frame_early) {
    // frame is early, but not that early. mangle it and accept as is.
    frame->pts = next_tail_ts;
    pthis->precise_tail_pts = next_tail_ts;
    pthis->buf.push(frame);
  } else {
    // frame is too early to consider. toss it out with yesterday's garbage.
    av_frame_free(&frame);
  }
}

char frame_buffer_has_next(struct frame_buffer_s* pthis) {
  return pthis->buf.size() > 1;
}
