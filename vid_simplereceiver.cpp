#include "vid_receiver_common.h"

void DisplayVideoFrame(SwsContext *sws_ctx, AVFrame *net_frame, int bitrate) {
  printf("DisplayVideoFrame %d.%01d kbits/s\n", bitrate / 1000,
         (bitrate % 1000) / 100);
}
void DisplaySnapshotFrame(SwsContext *sws_ctx, AVFrame *net_frame) {
  printf("DisplaySnapshotFrame\n");
}

static volatile bool g_running = true;

static void MessageErrorToUser(const char *text, ...) {}

int main() {
  // av_log_set_level(AV_LOG_FATAL);
  DisplayContext ctx("cam.sdp", ControlInterface::make_addr(CTRL_NET_PATH));
  if (!ctx.init(MessageErrorToUser)) {
    g_running = false;
    return 1;
  }

  while (g_running)
    ctx.do_frame();

  return 0;
}
