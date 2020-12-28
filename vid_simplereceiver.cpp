#include "vid_receiver_common.h"

void DisplayVideoFrame(SwsContext *sws_ctx, AVFrame *net_frame, int bitrate) {
  printf("DisplayVideoFrame %d.%01d kbits/s\n", bitrate / 1000,
         (bitrate % 1000) / 100);
}
void DisplaySnapshotFrame(SwsContext *sws_ctx, AVFrame *net_frame) {
  printf("DisplaySnapshotFrame\n");
}

void UpdateCtrlMenu(std::vector<uint8_t>::const_iterator begin,
                    std::vector<uint8_t>::const_iterator end) {
  printf("UpdateCtrlMenu\n");
  ControlReader reader(begin, end);
  while (reader) {
    v4l2_queryctrl ctrl = reader.read_ctrl();
    printf("ctrl: %s\n", ctrl.name);
    if (ctrl.type == V4L2_CTRL_TYPE_MENU) {
      for (int m = ctrl.minimum; m <= ctrl.maximum; ++m) {
        v4l2_querymenu menu = reader.read_menu();
        printf("  menu: %s\n", menu.name);
      }
    }
  }
}

static volatile bool g_running = true;

static void MessageErrorToUser(const char *text, ...) {}

int main() {
  av_log_set_level(AV_LOG_INFO);

  DisplayContext ctx("cam.sdp");
  if (!ctx.init(MessageErrorToUser)) {
    g_running = false;
    return 1;
  }

  while (g_running)
    ctx.do_frame();

  return 0;
}
