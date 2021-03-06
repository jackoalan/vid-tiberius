#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

/**
 * As of Nov 24 2020, the control_url field quadrupled in size.
 * 58.65 isn't exactly on-point for this change but excludes
 * Arch Linux's 58.64 at the time of writing.
 */
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 65, 100)
#define MAX_URL_SIZE 4096
#else
#define MAX_URL_SIZE 1024
#endif

/* clang-format off */
/**
 * Partially copied from ffmpeg's private rtsp.h
 * MAY BREAK WITH UPSTREAM CHANGES!!!
 *
 * Private data for the RTSP demuxer.
 *
 * @todo Use AVIOContext instead of URLContext
 */
typedef struct RTSPState {
  const AVClass *clazz;             /**< Class for private options. */
  URLContext *rtsp_hd; /* RTSP TCP connection handle */

  /** number of items in the 'rtsp_streams' variable */
  int nb_rtsp_streams;

  struct RTSPStream **rtsp_streams; /**< streams in this session */
} RTSPState;

/**
 * Partially copied from ffmpeg's private rtsp.h
 * MAY BREAK WITH UPSTREAM CHANGES!!!
 *
 * Describe a single stream, as identified by a single m= line block in the
 * SDP content. In the case of RDT, one RTSPStream can represent multiple
 * AVStreams. In this case, each AVStream in this set has similar content
 * (but different codec/bitrate).
 */
typedef struct RTSPStream {
  URLContext *rtp_handle;   /**< RTP stream handle (if UDP) */
  void *transport_priv; /**< RTP/RDT parse context if input, RTP AVFormatContext if output */

  /** corresponding stream index, if any. -1 if none (MPEG2TS case) */
  int stream_index;

  /** interleave IDs; copies of RTSPTransportField->interleaved_min/max
   * for the selected transport. Only used for TCP. */
  int interleaved_min, interleaved_max;

  char control_url[MAX_URL_SIZE];   /**< url for this stream (from SDP) */

  /** The following are used only in SDP, not RTSP */
  //@{
  int sdp_port;             /**< port (from SDP content) */
  struct sockaddr_storage sdp_ip; /**< IP address (from SDP content) */
  int nb_include_source_addrs; /**< Number of source-specific multicast include source IP addresses (from SDP content) */
  struct RTSPSource **include_source_addrs; /**< Source-specific multicast include source IP addresses (from SDP content) */
  int nb_exclude_source_addrs; /**< Number of source-specific multicast exclude source IP addresses (from SDP content) */
  struct RTSPSource **exclude_source_addrs; /**< Source-specific multicast exclude source IP addresses (from SDP content) */
  int sdp_ttl;              /**< IP Time-To-Live (from SDP content) */
  int sdp_payload_type;     /**< payload type */
  //@}
} RTSPStream;

typedef struct URLContext {
  const AVClass *av_class;    /**< information for av_log(). Set by url_open(). */
  const struct URLProtocol *prot;
  void *priv_data;
} URLContext;

typedef struct IPSourceFilters {
  int nb_include_addrs;
  int nb_exclude_addrs;
  struct sockaddr_storage *include_addrs;
  struct sockaddr_storage *exclude_addrs;
} IPSourceFilters;

typedef struct RTPContext {
  const AVClass *clazz;
  URLContext *rtp_hd, *rtcp_hd, *fec_hd;
  int rtp_fd, rtcp_fd;
  IPSourceFilters filters;
  int write_to_source;
  struct sockaddr_storage last_rtp_source, last_rtcp_source;
} RTPContext;
/* clang-format on */
}

#include "control_interface.h"
#include "ff_helpers.h"
#include "nullptr.h"

#define QUALITY_MIN 18
#define QUALITY_MAX 50
#define QUALITY_DEF 30

#define DISP_PIX_FMT AV_PIX_FMT_BGRA

class DecodeContext {
  AVCodecContext *video_dec_ctx;
  int video_stream_idx;
  AVFrame *net_frame;
  SwsContext *sws_ctx;
  int cached_width;
  int cached_height;

public:
  DecodeContext()
      : video_dec_ctx(nullptr), video_stream_idx(-1), net_frame(nullptr),
        sws_ctx(nullptr), cached_width(0), cached_height(0) {}
  ~DecodeContext() { deinit(); }

  bool init(AVFormatContext *fmt_dec_ctx, int wanted_stream_nb,
            void (*err_handler)(const char *, ...)) {
    deinit();

    int ret = open_codec_context(video_stream_idx, video_dec_ctx, fmt_dec_ctx,
                                 AVMEDIA_TYPE_VIDEO, wanted_stream_nb);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to open_codec_context: %s\n",
             av_err2str_cpp(ret));
      err_handler("Unable to open_codec_context: %s", av_err2str_cpp(ret));
      return false;
    }

    net_frame = av_frame_alloc();
    if (!net_frame) {
      av_log(nullptr, AV_LOG_ERROR, "net_frame av_frame_alloc\n");
      err_handler("net_frame av_frame_alloc");
      return false;
    }

    return true;
  }

  void deinit() {
    avcodec_free_context(&video_dec_ctx);
    video_stream_idx = -1;
    av_frame_free(&net_frame);
    sws_freeContext(sws_ctx);
    sws_ctx = nullptr;
    cached_width = 0;
    cached_height = 0;
  }

  bool reinit_frame() {
    if (sws_ctx) {
      sws_freeContext(sws_ctx);
      sws_ctx = nullptr;
    }

    cached_width = width();
    cached_height = height();

    sws_ctx = sws_getContext(width(), height(), pix_fmt(), width(), height(),
                             DISP_PIX_FMT, 0, nullptr, nullptr, nullptr);
    if (!sws_ctx) {
      av_log(nullptr, AV_LOG_ERROR, "sws_getContext\n");
      return false;
    }

    return true;
  }

  bool send_packet(AVPacket &pkt) {
    return avcodec_send_packet(video_dec_ctx, &pkt) >= 0;
  }

  template <class FrameDisplayer>
  bool output_frames(const FrameDisplayer &displayer) {
    while (true) {
      int ret = avcodec_receive_frame(video_dec_ctx, net_frame);
      if (ret < 0) {
        // those two return values are special and mean there is no output
        // net_frame available, but there were no errors during decoding
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
          return true;

        av_log(nullptr, AV_LOG_ERROR, "Error during decoding: %s\n",
               av_err2str_cpp(ret));
        return false;
      }

      // write the net_frame data to output file
      if (video_dec_ctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        if (width() != cached_width || height() != cached_height)
          reinit_frame();

        displayer.DisplayFrame(sws_ctx, net_frame);
      }

      av_frame_unref(net_frame);
    }
  }

  int width() const { return video_dec_ctx->width; }
  int height() const { return video_dec_ctx->height; }
  AVPixelFormat pix_fmt() const { return video_dec_ctx->pix_fmt; }
};

class NetworkGrabber {
  const char *sdp_path;
  DecodeContext dec_ctxs[2];
  AVFormatContext *fmt_dec_ctx;
  const char *cached_title;
  const char *cached_comment;
  uint16_t cached_ctrl_port;

  const char *get_metadata(const char *key, const char *&cache_var) {
    if (cache_var)
      return cache_var;

    AVDictionaryEntry *ent;
    if (fmt_dec_ctx &&
        (ent = av_dict_get(fmt_dec_ctx->metadata, key, nullptr, 0))) {
      cache_var = ent->value;
      return cache_var;
    }

    return nullptr;
  }

public:
  NetworkGrabber(const char *sdp_path)
      : sdp_path(sdp_path), fmt_dec_ctx(nullptr), cached_title(nullptr),
        cached_comment(nullptr), cached_ctrl_port(0) {}
  ~NetworkGrabber() { deinit(); }

  bool init(void (*err_handler)(const char *, ...)) {
    deinit();

    AVInputFormat *sdp_fmt = av_find_input_format("sdp");
    if (!sdp_fmt) {
      av_log(nullptr, AV_LOG_ERROR,
             "Unable to av_find_input_format(\"sdp\")\n");
      err_handler("Unable to av_find_input_format(\"sdp\")");
      return false;
    }

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,udp,rtp", 0);
    int ret = avformat_open_input(&fmt_dec_ctx, sdp_path, sdp_fmt, &opts);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avformat_open_input(%s): %s\n",
             sdp_path, av_err2str_cpp(ret));
      err_handler("Unable to open %s: %s", sdp_path, av_err2str_cpp(ret));
      return false;
    }

    if (!dec_ctxs[0].init(fmt_dec_ctx, 0, err_handler))
      return false;

    if (!dec_ctxs[1].init(fmt_dec_ctx, 1, err_handler))
      return false;

    return true;
  }

  void deinit() {
    avformat_close_input(&fmt_dec_ctx);
    dec_ctxs[0].deinit();
    dec_ctxs[1].deinit();
    cached_title = nullptr;
    cached_comment = nullptr;
  }

  DecodeContext &dec_ctx(int i) { return dec_ctxs[i]; }
  const char *title() { return get_metadata("title", cached_title); }
  const char *comment() { return get_metadata("comment", cached_comment); }

  uint16_t ctrl_port() {
    if (cached_ctrl_port)
      return cached_ctrl_port;
    if (const char *p = strstr(comment(), "c:"))
      cached_ctrl_port = strtol(p + 2, nullptr, 10);
    return cached_ctrl_port;
  }

  struct sockaddr_storage sdp_ip(int &port) {
    sockaddr_storage sdp_ip{};
    RTSPState *rt = (RTSPState *)fmt_dec_ctx->priv_data;
    if (rt->nb_rtsp_streams) {
      RTSPStream *s = rt->rtsp_streams[0];
      sdp_ip = s->sdp_ip;
      port = s->sdp_port;
    }
    return sdp_ip;
  }

  struct sockaddr_storage last_rtp_source() {
    sockaddr_storage addr{};
    RTSPState *rt = (RTSPState *)fmt_dec_ctx->priv_data;
    if (rt->nb_rtsp_streams) {
      RTSPStream *s = rt->rtsp_streams[0];
      if (s->rtp_handle) {
        RTPContext *rtp = (RTPContext *)s->rtp_handle->priv_data;
        addr = rtp->last_rtp_source;
      }
    }
    return addr;
  }

  bool read_frame(AVPacket &pkt, struct sockaddr_in &ctrl_addr) {
    int err = av_read_frame(fmt_dec_ctx, &pkt);
    if (err >= 0) {
      if (!ctrl_addr.sin_port) {
        ctrl_addr.sin_family = AF_INET;
        ctrl_addr.sin_port = htons(ctrl_port());
      }

      struct sockaddr_storage last_addr = last_rtp_source();
      struct sockaddr_in last_addr_in {};
      memcpy(&last_addr_in, &last_addr, sizeof(last_addr_in));
      if (memcmp(&ctrl_addr.sin_addr, &last_addr_in.sin_addr,
                 sizeof(last_addr_in.sin_addr)) != 0) {
        uint32_t addr_num = 0;
        memcpy(&addr_num, &last_addr_in.sin_addr, 4);
        addr_num = ntohl(addr_num);
        av_log(nullptr, AV_LOG_INFO,
               "Setting control address to: %d.%d.%d.%d:%d\n",
               (addr_num >> 24) & 0xff, (addr_num >> 16) & 0xff,
               (addr_num >> 8) & 0xff, addr_num & 0xff, ctrl_port());
        ctrl_addr.sin_addr = last_addr_in.sin_addr;
      }
      return true;
    }
    av_log(nullptr, AV_LOG_ERROR, "av_read_frame: %s\n", av_err2str_cpp(err));
    return false;
  }
};

class DisplayContext *g_dispCtx = nullptr;
void DisplayVideoFrame(SwsContext *sws_ctx, AVFrame *net_frame, int bitrate);
void DisplaySnapshotFrame(SwsContext *sws_ctx, AVFrame *net_frame);
void UpdateCtrlMenu(std::vector<uint8_t>::const_iterator begin,
                    std::vector<uint8_t>::const_iterator end);

struct VideoFrameDisplayer {
  int bitrate;
  VideoFrameDisplayer(int bitrate) : bitrate(bitrate) {}
  void DisplayFrame(SwsContext *sws_ctx, AVFrame *net_frame) const {
    DisplayVideoFrame(sws_ctx, net_frame, bitrate);
  }
};

struct SnapshotFrameDisplayer {
  void DisplayFrame(SwsContext *sws_ctx, AVFrame *net_frame) const {
    DisplaySnapshotFrame(sws_ctx, net_frame);
  }
};

enum v4l2_ctrl_type {
  V4L2_CTRL_TYPE_INTEGER = 1,
  V4L2_CTRL_TYPE_BOOLEAN = 2,
  V4L2_CTRL_TYPE_MENU = 3,
  V4L2_CTRL_TYPE_BUTTON = 4,
  V4L2_CTRL_TYPE_INTEGER64 = 5,
  V4L2_CTRL_TYPE_CTRL_CLASS = 6,
  V4L2_CTRL_TYPE_STRING = 7,
  V4L2_CTRL_TYPE_BITMASK = 8,
  V4L2_CTRL_TYPE_INTEGER_MENU = 9,

  /* Compound types are >= 0x0100 */
  V4L2_CTRL_COMPOUND_TYPES = 0x0100,
  V4L2_CTRL_TYPE_U8 = 0x0100,
  V4L2_CTRL_TYPE_U16 = 0x0101,
  V4L2_CTRL_TYPE_U32 = 0x0102,
  V4L2_CTRL_TYPE_AREA = 0x0106,
};

struct v4l2_queryctrl {
  uint32_t id;
  uint32_t type;    /* enum v4l2_ctrl_type */
  uint8_t name[32]; /* Whatever */
  int32_t minimum;  /* Note signedness */
  int32_t maximum;
  int32_t step;
  int32_t default_value;
  uint32_t flags;
  uint32_t reserved[2];
};

struct v4l2_querymenu {
  uint32_t id;
  uint32_t index;
  union {
    uint8_t name[32]; /* Whatever */
    int64_t value;
  };
  uint32_t reserved;
} __attribute__((packed));

#define V4L2_CTRL_CLASS_CAMERA 0x009a0000

class ControlReader {
  std::vector<uint8_t>::const_iterator begin, end;

  template <class T> T read() {
    T ret;
    memcpy(&ret, &*begin, sizeof(ret));
    begin += sizeof(ret);
    return ret;
  }

public:
  ControlReader(std::vector<uint8_t>::const_iterator begin,
                std::vector<uint8_t>::const_iterator end)
      : begin(begin), end(end) {}

  operator bool() const { return begin != end; }
  v4l2_queryctrl read_ctrl() { return read<v4l2_queryctrl>(); }
  v4l2_querymenu read_menu() { return read<v4l2_querymenu>(); }
};

class DisplayContext {
  NetworkGrabber net;
  AVPacket dec_pkt;
  ControlInterface ctrl;
  struct sockaddr_in ctrl_addr;
  int64_t last_time;
  int read_bytes;
  int bitrate;
  std::vector<uint8_t> last_ctrls;

  bool decode_packet() {
    if (dec_pkt.stream_index > 1)
      return false;

    DecodeContext &dec_ctx = net.dec_ctx(dec_pkt.stream_index);

    // send packet to decoder
    if (!dec_ctx.send_packet(dec_pkt)) {
      if (dec_pkt.stream_index == 0) {
        av_log(nullptr, AV_LOG_WARNING, "Requesting keyframe\n");
        ctrl.send_emit_keyframe(ctrl_addr);
      }
      return false;
    }

    if (dec_pkt.stream_index == 0) {
      // calculate bitrate
      read_bytes += dec_pkt.size;
      int64_t cur_time = av_gettime_relative();
      if (last_time != -1) {
        int64_t delta = cur_time - last_time;
        if (delta >= 500000ll) {
          bitrate = int(int64_t(read_bytes) * 8000000ll / delta);
          read_bytes = 0;
          last_time = cur_time;
        }
      } else {
        last_time = cur_time;
      }

      // get all the available frames from the decoder
      return dec_ctx.output_frames(VideoFrameDisplayer(bitrate));
    } else if (dec_pkt.stream_index == 1) {
      // get all the available frames from the decoder
      return dec_ctx.output_frames(SnapshotFrameDisplayer());
    } else {
      return false;
    }
  }

public:
  DisplayContext(const char *sdp_path)
      : net(sdp_path), dec_pkt({}), ctrl_addr({}), last_time(-1), read_bytes(0),
        bitrate(0) {
    av_init_packet(&dec_pkt);
    g_dispCtx = this;
  }
  ~DisplayContext() { deinit(); }

  bool init(void (*err_handler)(const char *, ...)) {
    deinit();

    /* Net grab */
    if (!net.init(err_handler))
      return false;

    ctrl.init(net.ctrl_port() + 1);
    send_emit_keyframe();
    send_quality(QUALITY_DEF);

    return true;
  }

  void deinit() {
    ctrl.deinit();
    net.deinit();
  }

  void do_frame() {
    std::vector<uint8_t>::const_iterator end = ctrl.recv_ctrls(last_ctrls);
    if (last_ctrls.begin() != end)
      UpdateCtrlMenu(last_ctrls.begin(), end);

    if (net.read_frame(dec_pkt, ctrl_addr)) {
      decode_packet();
      av_packet_unref(&dec_pkt);
    }
  }

  void send_emit_keyframe() const { ctrl.send_emit_keyframe(ctrl_addr); }
  void send_quality(double q) const { ctrl.send_quality(q, ctrl_addr); }
  void send_req_snapshot() const { ctrl.send_req_snapshot(ctrl_addr); }
  void send_ctrl(uint32_t id, int32_t value) const {
    ctrl.send_ctrl(id, value, ctrl_addr);
  }

  const char *title() { return net.title(); }
  const char *comment() { return net.comment(); }
  struct sockaddr_storage sdp_ip(int &port) {
    return net.sdp_ip(port);
  }
};
