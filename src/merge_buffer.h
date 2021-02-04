//
//  merge_buffer.h
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

#ifndef generic_merge_buffer_h
#define generic_merge_buffer_h

#include <libavutil/frame.h>

/*
 * Wallclock merge buffer takes frames from multiple sources and emits them in
 * Order and with timestamps relative to each other.
 */
struct merge_buffer_s;

void merge_buffer_alloc(struct merge_buffer_s** merge_buffer_out,
                        double pts_interval);
void merge_buffer_free(struct merge_buffer_s* merge_buffer);
char merge_buffer_has_next(struct merge_buffer_s* merge_buffer);
int merge_buffer_get_next(struct merge_buffer_s* merge_buffer,
                          AVFrame** frame_out);
void merge_buffer_consume(struct merge_buffer_s* merge_buffer,
                          AVFrame* frame);

#endif /* generic_merge_buffer_h */
