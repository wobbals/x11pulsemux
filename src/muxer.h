
struct muxer_s;

struct muxer_config_s {
  const char* outfile_path;
  const char* device_name;
};

// invoke before opening the first muxer.
void muxer_initialize();

int muxer_open(struct muxer_s** muxer, struct muxer_config_s* config);
int muxer_close(struct muxer_s* muxer);
