//
//  main.c
//

#include <unistd.h>
#include <ctype.h>
#include <getopt.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "muxer.h"

void usage() {
  printf("usage: x11pulsemux [-d X11DEVICE] -o OUTFILE_PATH\n");
}

volatile char interrupted = 0;

void handle_interrupt(int signal) {
  printf("SIGINT\n");
  interrupted = 1;
}

int main(int argc, char **argv)
{
  int c;
  char* outfile_path = NULL;
  char* device_name = ":0.0";

  static struct option long_options[] =
  {
    {"output", required_argument,       0, 'o'},
    {"device", optional_argument,       0, 'd'},
    {0, 0, 0, 0}
  };
  /* getopt_long stores the option index here. */
  int option_index = 0;

  while ((c = getopt_long(argc, argv, "o:d:",
                          long_options, &option_index)) != -1)
  {
    switch (c)
    {
      case 'o':
        outfile_path = optarg;
        break;
      case 'd':
        device_name = optarg;
        break;
      case '?':
        if (isprint (optopt))
          fprintf(stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf(stderr,
                  "Unknown option character `\\x%x'.\n",
                  optopt);
        return 1;
      default:
        abort();
    }
  }

  if (!outfile_path) {
    usage();
    return -1;
  }

  // pass process args to keyframe reader
  struct muxer_config_s config = { 0 };
  config.outfile_path = outfile_path;
  config.device_name = device_name;
  struct muxer_s* muxer = NULL;
  muxer_initialize();
  muxer_open(&muxer, &config);
  if (!muxer) {
    fprintf(stderr, "Unable to open muxer\n");
    return -1;
  }
  signal(SIGINT, handle_interrupt);
  while (!interrupted) {
    usleep(10000);
  }
  fprintf(stderr, "interrupted. closing...\n");
  muxer_close(muxer);
  fprintf(stderr, "muxer closed. exit.\n");
  return 0;
}
