extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "control_interface.h"
#include "ff_helpers.h"

// ffmpeg -f video4linux2 -r 10 -video_size 432x240 -i /dev/video0 -f h264
// -vcodec libx264 -crf 30 -maxrate 128k -bufsize 1M -preset medium -tune
// zerolatency rtp://localhost:1936

#define VIDEO_SIZE "432x240"
#define SNAP_SIZE "1280x720"
#define FRAMERATE 10
#define DEFAULT_CRF 30

struct v4l2_video_data {
  AVClass *clazz;
  int fd;
};

class CameraGrabber {
  const char *cam_name;
  AVCodecContext *video_dec_ctx = nullptr;
  AVFormatContext *fmt_dec_ctx = nullptr;
  int video_stream_idx = -1;

public:
  explicit CameraGrabber(const char *cam_name) : cam_name(cam_name) {}
  ~CameraGrabber() { deinit(); }

  bool _init(const char *video_size) {
    deinit();

    AVInputFormat *v4l2_fmt = av_find_input_format("video4linux2");
    if (!v4l2_fmt) {
      av_log(nullptr, AV_LOG_ERROR,
             "Unable to av_find_input_format(\"video4linux2\")\n");
      return false;
    }

    AVDeviceInfoList *dev_list = nullptr;
    int ret =
        avdevice_list_input_sources(v4l2_fmt, nullptr, nullptr, &dev_list);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR,
             "Unable to avdevice_list_input_sources: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    const char *dev_path = nullptr;
    for (int i = 0; i < dev_list->nb_devices; i++) {
      AVDeviceInfo *dev_info = dev_list->devices[i];

      int fd = open(dev_info->device_name, O_RDONLY, 0);
      if (fd < 0) {
        int err = AVERROR(errno);
        av_log(nullptr, AV_LOG_ERROR, "Cannot open video device %s: %s\n",
               dev_info->device_name, av_err2str_cpp(err));
        return false;
      }

      struct v4l2_capability cap {};
      if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        close(fd);
        int err = AVERROR(errno);
        av_log(nullptr, AV_LOG_ERROR, "ioctl(VIDIOC_QUERYCAP): %s\n",
               av_err2str_cpp(err));
        return false;
      }

      close(fd);

      if (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE &&
          !strcmp((char *)cap.card, cam_name)) {
        dev_path = dev_info->device_name;
        break;
      }
    }

    if (!dev_path) {
      avdevice_free_list_devices(&dev_list);
      av_log(nullptr, AV_LOG_ERROR, "Unable to find camera device\n");
      return false;
    }

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "video_size", video_size, 0);
    av_dict_set(&opts, "framerate", AV_STRINGIFY(FRAMERATE), 0);

    fmt_dec_ctx = avformat_alloc_context();
    fmt_dec_ctx->flags |= AVFMT_FLAG_NONBLOCK;
    ret = avformat_open_input(&fmt_dec_ctx, dev_path, v4l2_fmt, &opts);
    av_dict_free(&opts);
    avdevice_free_list_devices(&dev_list);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avformat_open_input: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    ret = open_codec_context(video_stream_idx, video_dec_ctx, fmt_dec_ctx,
                             AVMEDIA_TYPE_VIDEO);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to open_codec_context: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    return true;
  }

  bool init(const char *video_size = VIDEO_SIZE) {
    while (!_init(video_size)) {
      struct timespec ts = {0, 500000000};
      nanosleep(&ts, nullptr);
    }
    return true;
  }

  void deinit() {
    avformat_close_input(&fmt_dec_ctx);
    avcodec_free_context(&video_dec_ctx);
    video_stream_idx = -1;
  }

  AVCodecContext *dec_ctx() const { return video_dec_ctx; }
  int width() const { return video_dec_ctx->width; }
  int height() const { return video_dec_ctx->height; }
  AVRational time_base() const { return {1, FRAMERATE}; }
  AVPixelFormat pix_fmt() const { return video_dec_ctx->pix_fmt; }

  std::vector<uint8_t> query_ctrls() const {
    std::vector<uint8_t> ret;

    if (fmt_dec_ctx) {
      v4l2_video_data *vd = (v4l2_video_data *)fmt_dec_ctx->priv_data;
      v4l2_queryctrl ctrl{};
      ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
      while (ioctl(vd->fd, VIDIOC_QUERYCTRL, &ctrl) >= 0) {
        v4l2_control ctrl_val{};
        ctrl_val.id = ctrl.id;
        if (ioctl(vd->fd, VIDIOC_G_CTRL, &ctrl_val) >= 0)
          ctrl.default_value = ctrl_val.value;
        memcpy(&*ret.insert(ret.end(), sizeof(ctrl), 0), &ctrl, sizeof(ctrl));
        if (ctrl.type == V4L2_CTRL_TYPE_MENU) {
          for (int m = ctrl.minimum; m <= ctrl.maximum; ++m) {
            v4l2_querymenu menu{};
            menu.id = ctrl.id;
            menu.index = m;
            ioctl(vd->fd, VIDIOC_QUERYMENU, &menu);
            memcpy(&*ret.insert(ret.end(), sizeof(menu), 0), &menu,
                   sizeof(menu));
          }
        }
        ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
      }
    }

    return ret;
  }

  void set_ctrl(uint32_t id, int32_t val) {
    if (fmt_dec_ctx) {
      v4l2_video_data *vd = (v4l2_video_data *)fmt_dec_ctx->priv_data;
      v4l2_control ctrl{};
      ctrl.id = id;
      ctrl.value = val;
      ioctl(vd->fd, VIDIOC_S_CTRL, &ctrl);
    }
  }

  bool read_frame(AVPacket &pkt) {
    if (!fmt_dec_ctx)
      return false;

    int ret;
    while ((ret = av_read_frame(fmt_dec_ctx, &pkt)) == AVERROR(EAGAIN)) {
      struct timespec ts = {0, 50000000};
      nanosleep(&ts, nullptr);
    }
    return ret >= 0;
  }
};

class EncodeContext {
protected:
  const char *enc_name;
  const char *net_path;
  AVPixelFormat pix_fmt;
  AVCodecContext *video_enc_ctx = nullptr;
  AVStream *video_enc_stream = nullptr;
  AVFormatContext *fmt_enc_context = nullptr;

public:
  explicit EncodeContext(const char *enc_name, const char *net_path,
                         AVPixelFormat pix_fmt)
      : enc_name(enc_name), net_path(net_path), pix_fmt(pix_fmt) {}
  ~EncodeContext() { deinit(); }

  virtual void set_options(AVDictionary *&opts) = 0;

  bool init(int width, int height, AVRational time_base) {
    deinit();

    AVCodec *codec = avcodec_find_encoder_by_name(enc_name);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR,
             "Unable to avcodec_find_encoder_by_name(%s)\n", enc_name);
      return false;
    }

    video_enc_ctx = avcodec_alloc_context3(codec);
    if (!video_enc_ctx) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avcodec_alloc_context3\n");
      return false;
    }

    video_enc_ctx->width = width;
    video_enc_ctx->height = height;
    video_enc_ctx->pix_fmt = pix_fmt;
    video_enc_ctx->time_base = time_base;
    AVDictionary *opts = nullptr;
    set_options(opts);
    int ret = avcodec_open2(video_enc_ctx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avcodec_open2: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    ret = avformat_alloc_output_context2(&fmt_enc_context, nullptr, "rtp",
                                         net_path);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR,
             "Unable to avformat_alloc_output_context2: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    ret = avio_open(&fmt_enc_context->pb, net_path, AVIO_FLAG_WRITE);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avio_open(%s): %s\n", net_path,
             av_err2str_cpp(ret));
      return false;
    }

    video_enc_stream = avformat_new_stream(fmt_enc_context, nullptr);
    if (!video_enc_stream) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avformat_new_stream\n");
      return false;
    }

    ret = avcodec_parameters_from_context(video_enc_stream->codecpar,
                                          video_enc_ctx);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR,
             "Unable to avcodec_parameters_from_context: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    av_dict_set(&opts, "keyframe_replay_params", "1", 0);
    set_options(opts);
    ret = avformat_write_header(fmt_enc_context, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "Unable to avformat_write_header: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    return true;
  }

  void deinit() {
    if (fmt_enc_context)
      av_write_trailer(fmt_enc_context);
    avformat_close_input(&fmt_enc_context);
    video_enc_stream = nullptr;
    avcodec_free_context(&video_enc_ctx);
  }

  bool encode(AVFrame *frame) {
    int ret = avcodec_send_frame(video_enc_ctx, frame);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR,
             "Error sending a sws_frame to the encoder: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    while (ret >= 0) {
      AVPacket pkt = {};

      ret = avcodec_receive_packet(video_enc_ctx, &pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_packet_unref(&pkt);
        break;
      } else if (ret < 0) {
        av_packet_unref(&pkt);
        av_log(nullptr, AV_LOG_ERROR, "Error encoding a net_frame: %s\n",
               av_err2str_cpp(ret));
        return false;
      }

      /* rescale output packet timestamp values from codec to stream timebase */
      av_packet_rescale_ts(&pkt, video_enc_ctx->time_base,
                           video_enc_stream->time_base);
      pkt.stream_index = video_enc_stream->index;

      /* Write the compressed net_frame to the media file. */
      ret = av_interleaved_write_frame(fmt_enc_context, &pkt);
      av_packet_unref(&pkt);
      if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Error while writing output packet: %s\n",
               av_err2str_cpp(ret));
        return false;
      }
    }

    return true;
  }
};

// m=video 1936 RTP/AVP 96
// a=rtpmap:96 H264/90000
#ifndef V4L2_ENCODE
class H264EncodeContext : public EncodeContext {
public:
  static constexpr AVPixelFormat pix_fmt = AV_PIX_FMT_YUV422P;
  explicit H264EncodeContext(const char *h264_path)
      : EncodeContext("libx264", h264_path, pix_fmt) {}

  void set_options(AVDictionary *&opts) override {
    av_dict_set(&opts, "crf", AV_STRINGIFY(DEFAULT_CRF), 0);
    av_dict_set(&opts, "framerate", AV_STRINGIFY(FRAMERATE), 0);
    av_dict_set(&opts, "maxrate", "128k", 0);
    av_dict_set(&opts, "bufsize", "1M", 0);
    av_dict_set(&opts, "preset", "medium", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
  }

  void set_crf(double crf) {
    av_opt_set_double(video_enc_ctx->priv_data, "crf", crf, 0);
  }
};

#else

struct V4L2m2mContext {
  char devname[PATH_MAX];
  int fd;
};

struct V4L2m2mPriv {
  AVClass *clazz;
  V4L2m2mContext *context;
};

class H264EncodeContext : public EncodeContext {
public:
  static constexpr AVPixelFormat pix_fmt = AV_PIX_FMT_YUYV422;
  explicit H264EncodeContext(const char *h264_path)
      : EncodeContext("h264_v4l2m2m", h264_path, pix_fmt) {}

  void set_options(AVDictionary *&opts) override {
    int bitrate = crf_to_bitrate(DEFAULT_CRF);
    av_dict_set_int(&opts, "b", bitrate, 0);
    av_dict_set(&opts, "framerate", AV_STRINGIFY(FRAMERATE), 0);
  }

  static void v4l2_set_ext_ctrl(V4L2m2mContext *s, unsigned int id,
                                signed int value, const char *name,
                                int log_warning) {
    struct v4l2_ext_controls ctrls = {{0}};
    struct v4l2_ext_control ctrl = {0};

    /* set ctrls */
    ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
    ctrls.controls = &ctrl;
    ctrls.count = 1;

    /* set ctrl*/
    ctrl.value = value;
    ctrl.id = id;

    if (ioctl(s->fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0)
      av_log(nullptr,
             log_warning || errno != EINVAL ? AV_LOG_WARNING : AV_LOG_DEBUG,
             "Failed to set %s: %s\n", name, strerror(errno));
    else
      av_log(nullptr, AV_LOG_DEBUG, "Encoder: %s = %d\n", name, value);
  }

  /**
   * BCM2835 driver does not expose a crf control, so we approximate the bitrate
   * instead
   */
  static int crf_to_bitrate(double crf) {
    double norm = crf / 50.0;
    return int(norm * (25.0 * 1024.0) + (1.0 - norm) * (512.0 * 1024.0));
  }

  void set_crf(double crf) {
    int bitrate = crf_to_bitrate(crf);
    if (V4L2m2mPriv *priv = (V4L2m2mPriv *)video_enc_ctx->priv_data) {
      if (V4L2m2mContext *s = priv->context) {
        v4l2_set_ext_ctrl(s, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bit rate",
                          1);
      }
    }
  }
};
#endif

// m=video 1938 RTP/AVP 26
class JPEGEncodeContext : public EncodeContext {
public:
  static constexpr AVPixelFormat pix_fmt = AV_PIX_FMT_YUVJ420P;
  explicit JPEGEncodeContext(const char *jpeg_path)
      : EncodeContext("mjpeg", jpeg_path, pix_fmt) {}

  void set_options(AVDictionary *&opts) override {
    av_dict_set(&opts, "huffman", "default", 0);
    av_dict_set(&opts, "force_duplicated_matrix", "1", 0);
  }

  void set_crf(double crf) {}
};

template <class EncodeContext> class EncoderPipeline {
  EncodeContext enc;
  AVPixelFormat cam_pix_fmt = AV_PIX_FMT_NONE;
  AVFrame *cam_frame = nullptr;
  AVFrame *sws_frame = nullptr;
  SwsContext *sws_ctx = nullptr;
  AVPictureType next_pict_type = AV_PICTURE_TYPE_NONE;

public:
  explicit EncoderPipeline(const char *net_path) : enc(net_path) {}
  bool init(CameraGrabber &cam) {
    deinit();

    if (!enc.init(cam.width(), cam.height(), cam.time_base()))
      return false;

    cam_frame = av_frame_alloc();
    if (!cam_frame) {
      av_log(nullptr, AV_LOG_ERROR, "net_frame av_frame_alloc\n");
      return false;
    }

    sws_frame = av_frame_alloc();
    if (!sws_frame) {
      av_log(nullptr, AV_LOG_ERROR, "sws_frame av_frame_alloc\n");
      return false;
    }
    sws_frame->format = EncodeContext::pix_fmt;
    sws_frame->width = cam.width();
    sws_frame->height = cam.height();

    int ret = av_frame_get_buffer(sws_frame, 0);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "av_frame_get_buffer: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    cam_pix_fmt = cam.pix_fmt();
    sws_ctx = sws_getContext(cam.width(), cam.height(), cam.pix_fmt(),
                             cam.width(), cam.height(), EncodeContext::pix_fmt,
                             0, nullptr, nullptr, nullptr);
    if (!sws_ctx) {
      av_log(nullptr, AV_LOG_ERROR, "sws_getContext\n");
      return false;
    }

    return true;
  }

  void deinit() {
    enc.deinit();
    av_frame_free(&sws_frame);
    av_frame_free(&cam_frame);
    sws_freeContext(sws_ctx);
    sws_ctx = nullptr;
    cam_pix_fmt = AV_PIX_FMT_NONE;
  }

  bool output_video_frame(int64_t pts) {
    AVFrame *use_frame =
        cam_pix_fmt == EncodeContext::pix_fmt ? cam_frame : sws_frame;
    if (use_frame == sws_frame)
      sws_scale(sws_ctx, (const uint8_t *const *)cam_frame->data,
                cam_frame->linesize, 0, cam_frame->height, sws_frame->data,
                sws_frame->linesize);

    use_frame->pts = pts;
    use_frame->pict_type = next_pict_type;
    next_pict_type = AV_PICTURE_TYPE_NONE;
    return enc.encode(use_frame);
  }

  bool decode_packet(CameraGrabber &cam, const AVPacket &dec_pkt, int64_t pts) {
    // submit the packet to the decoder
    int ret = avcodec_send_packet(cam.dec_ctx(), &dec_pkt);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR,
             "Error submitting a packet for decoding: %s\n",
             av_err2str_cpp(ret));
      return false;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
      ret = avcodec_receive_frame(cam.dec_ctx(), cam_frame);
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
      if (cam.dec_ctx()->codec->type == AVMEDIA_TYPE_VIDEO)
        output_video_frame(pts);

      av_frame_unref(cam_frame);
    }

    return true;
  }

  void set_crf(double crf) { enc.set_crf(crf); }

  void emit_keyframe() { next_pict_type = AV_PICTURE_TYPE_I; }
};

class TranscodeContext {
  CameraGrabber cam;
  EncoderPipeline<H264EncodeContext> enc;
  EncoderPipeline<JPEGEncodeContext> snap_enc;
  AVPacket dec_pkt = {};
  ControlInterface ctrl;
  sockaddr_in ctrl_addr;
  bool pending_snapshot = false;

public:
  TranscodeContext(const char *cam_name, const char *h264_path,
                   const char *jpeg_path, sockaddr_in ctrl_addr)
      : cam(cam_name), enc(h264_path), snap_enc(jpeg_path),
        ctrl_addr(ctrl_addr) {
    av_init_packet(&dec_pkt);
  }
  ~TranscodeContext() { deinit(); }

  bool init() {
    deinit();

    if (!cam.init(SNAP_SIZE))
      return false;
    if (!snap_enc.init(cam))
      return false;

    if (!cam.init())
      return false;
    if (!enc.init(cam))
      return false;

    ctrl.init(ntohs(ctrl_addr.sin_port));
    ctrl_addr.sin_port = htons(ntohs(ctrl_addr.sin_port) + 1);
    ctrl.send_ctrls(cam.query_ctrls(), ctrl_addr);

    return true;
  }

  void deinit() {
    ctrl.deinit();
    snap_enc.deinit();
    enc.deinit();
    cam.deinit();
  }

  void handle_events() {
    ControlInterface::Event ev = {};
    while (ctrl.recv_event(ev)) {
      switch (ev.type) {
      case ControlInterface::E_Keyframe:
        emit_keyframe();
        break;
      case ControlInterface::E_Quality:
        set_crf(ev.quality);
        break;
      case ControlInterface::E_Snapshot:
        pending_snapshot = true;
        break;
      case ControlInterface::E_Control:
        cam.set_ctrl(ev.ctrl_id, ev.ctrl_val);
        break;
      default:
        break;
      }
    }
  }

  int64_t pts = 0;
  bool do_frame() {
    handle_events();

    if (pending_snapshot) {
      if (!cam.init(SNAP_SIZE))
        return false;
      for (int i = 0; cam.read_frame(dec_pkt); ++i) {
        // Discard 3 frames to give camera a chance to adjust exposure
        if (i == 3) {
          snap_enc.decode_packet(cam, dec_pkt, ++pts);
          av_packet_unref(&dec_pkt);
          pending_snapshot = false;
          break;
        }
        av_packet_unref(&dec_pkt);
      }
      if (!cam.init())
        return false;
    }

    if (cam.read_frame(dec_pkt)) {
      enc.decode_packet(cam, dec_pkt, ++pts);
      av_packet_unref(&dec_pkt);
    } else {
      av_log(nullptr, AV_LOG_ERROR, "lost camera - reinitializing\n");
      if (!cam.init())
        return false;
    }

    return true;
  }

  void set_crf(double crf) { enc.set_crf(crf); }

  void emit_keyframe() {
    enc.emit_keyframe();
    ctrl.send_ctrls(cam.query_ctrls(), ctrl_addr);
  }
};

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr,
            "usage: %s <cam-name> <dest-ipv4-addr> <h264-port> <jpeg-port> "
            "<ctrl-port>\n",
            argv[0]);
    return 1;
  }

  char h264_path[32];
  snprintf(h264_path, 32, "rtp://%s:%s", argv[2], argv[3]);

  char jpeg_path[32];
  snprintf(jpeg_path, 32, "rtp://%s:%s", argv[2], argv[4]);

  sockaddr_in ctrl_addr{};
  inet_aton(argv[2], &ctrl_addr.sin_addr);
  ctrl_addr.sin_port = htons(strtoul(argv[5], nullptr, 10));

  // av_log_set_level(AV_LOG_DEBUG);

  /* Enable webcam capture */
  avdevice_register_all();

  TranscodeContext ctx(argv[1], h264_path, jpeg_path, ctrl_addr);
  if (!ctx.init())
    return 1;

  while (true) {
    if (!ctx.do_frame())
      return 1;
  }

  return 0;
}
