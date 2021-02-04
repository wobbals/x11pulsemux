//
//  video_frame_buffer.h
//  ichabod
//
//  Created by Charley Robinson on 6/15/17.
//

#ifndef video_frame_buffer_h
#define video_frame_buffer_h

#include <libavutil/frame.h>

/**
 * Constant rate frame buffer guarantees configured PTS interval by duplicating
 * frames as needed. Best for decoded video, but doesn't care much either way.
 */
struct frame_buffer_s;

void frame_buffer_alloc(struct frame_buffer_s** frame_buffer_out,
                        double pts_interval);
void frame_buffer_free(struct frame_buffer_s* frame_buffer);
char frame_buffer_has_next(struct frame_buffer_s* frame_buffer);
int frame_buffer_get_next(struct frame_buffer_s* frame_buffer,
                          AVFrame** frame_out);
void frame_buffer_consume(struct frame_buffer_s* frame_buffer,
                          AVFrame* frame);

#endif /* video_frame_buffer_h */
