//
//  file_writer.h
//  x11pulsemux
//
//  Created by Charley Robinson on 2/4/21.
//

#ifndef file_writer_h
#define file_writer_h

#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <uv.h>

struct file_writer_t {
  int out_width;
  int out_height;
  
  /* stream filtering */
  AVFilterContext *audio_buffersink_ctx;
  AVFilterContext *audio_buffersrc_ctx;
  AVFilterContext *video_buffersink_ctx;
  AVFilterContext *video_buffersrc_ctx;
  AVFilterGraph *video_filter_graph;
  AVFilterGraph *audio_filter_graph;
  
  /* container codec configuration */
  AVCodec* video_codec_out;
  AVCodec* audio_codec_out;
  AVCodecContext* video_ctx_out;
  AVCodecContext* audio_ctx_out;
  AVFormatContext* format_ctx_out;
  AVStream* video_stream;
  AVStream* audio_stream;
  int64_t video_frame_ct;
  int64_t audio_frame_ct;
  
  uv_mutex_t write_lock;
};

int file_writer_alloc(struct file_writer_t** writer);
void file_writer_free(struct file_writer_t* writer);

int file_writer_open(struct file_writer_t* writer,
                     const char* filename,
                     int out_width, int out_height);
int file_writer_push_audio_frame(struct file_writer_t* file_writer,
                                 AVFrame* frame, double timestamp);
int file_writer_push_video_frame(struct file_writer_t* file_writer,
                                 AVFrame* frame, double timestamp);
int file_writer_close(struct file_writer_t* writer);

#endif /* file_writer_h */
