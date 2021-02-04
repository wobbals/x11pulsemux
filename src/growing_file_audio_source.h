//
//  growing_file_audio_source.h
//  ichabod
//
//  Created by Charley Robinson on 6/7/17.
//

#ifndef growing_file_audio_source_h
#define growing_file_audio_source_h

/**
 * AVFrame source from a file being continuously written.
 */

#include <libavformat/avformat.h>
#include <libavutil/frame.h>

struct audio_source_s;

struct audio_source_config_s {
  const char* path;
  int64_t initial_timestamp;
};

void audio_source_alloc(struct audio_source_s** audio_source_out);
void audio_source_free(struct audio_source_s* audio_source);
int audio_source_load_config(struct audio_source_s* audio_source,
                             struct audio_source_config_s* config);
/** Caller is responsible for freeing frame_out */
int audio_source_next_frame(struct audio_source_s* audio_source,
                            AVFrame** frame_out);
const AVFormatContext* audio_source_get_format(struct audio_source_s* source);
const AVCodecContext* audio_source_get_codec(struct audio_source_s* source);
double audio_source_get_initial_timestamp(struct audio_source_s* source);

#endif /* growing_file_audio_source_h */
