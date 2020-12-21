#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

static int open_codec_context(int &stream_idx, AVCodecContext *&dec_ctx,
                              AVFormatContext *fmt_ctx, enum AVMediaType type,
                              int wanted_stream_nb = -1) {
  int ret, stream_index;
  AVStream *st;

  ret = av_find_best_stream(fmt_ctx, type, wanted_stream_nb, -1, nullptr, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not find %s stream in input file\n",
            av_get_media_type_string(type));
    return ret;
  } else {
    stream_index = ret;
    st = fmt_ctx->streams[stream_index];

    /* find decoder for the stream */
    AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
      fprintf(stderr, "Failed to find %s codec\n",
              av_get_media_type_string(type));
      return AVERROR(EINVAL);
    }

    /* Allocate a codec context for the decoder */
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
      fprintf(stderr, "Failed to allocate the %s codec context\n",
              av_get_media_type_string(type));
      return AVERROR(ENOMEM);
    }

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(dec_ctx, st->codecpar)) < 0) {
      fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
              av_get_media_type_string(type));
      return ret;
    }

    /* Init the decoders */
    if ((ret = avcodec_open2(dec_ctx, dec, nullptr)) < 0) {
      fprintf(stderr, "Failed to open %s codec\n",
              av_get_media_type_string(type));
      return ret;
    }
    stream_idx = stream_index;
  }

  return 0;
}

class _av_err2str_cpp {
  char _buf[AV_ERROR_MAX_STRING_SIZE];

public:
  _av_err2str_cpp(int errnum) {
    av_make_error_string(_buf, AV_ERROR_MAX_STRING_SIZE, errnum);
  }
  operator const char *() const { return _buf; }
};

#define av_err2str_cpp(errnum) (const char *)_av_err2str_cpp(errnum)
