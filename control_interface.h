#pragma once

#include "nullptr.h"

#ifdef __MINGW32__
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
typedef char *sock_dptr_t;
#define close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef void *sock_dptr_t;
#endif

#define CTRL_NET_PATH "10.20.0.20"
#define VIDEO_NET_PATH "rtp://10.10.0.20:1936"
#define SNAPSHOT_NET_PATH "rtp://10.10.0.20:1938"
//#define VIDEO_NET_PATH "rtp://127.0.0.1:1936"
//#define SNAPSHOT_NET_PATH "rtp://127.0.0.1:1938"
#define CONTROL_PORT 1940

class ControlInterface {
  SOCKET s;

  bool init(uint16_t port) {
    deinit();

    s = socket(AF_INET,
               SOCK_DGRAM
#ifndef __MINGW32__
                   | SOCK_NONBLOCK
#endif
               ,
               IPPROTO_UDP);
    if (s == -1) {
      av_log(nullptr, AV_LOG_ERROR, "socket\n");
      return false;
    }

#ifdef __MINGW32__
    u_long iMode = 1;
    if (ioctlsocket(s, FIONBIO, &iMode) != NO_ERROR) {
      av_log(nullptr, AV_LOG_ERROR, "ioctlsocket(FIONBIO, 1) failed\n");
      deinit();
      return false;
    }
#endif

    struct sockaddr_in si_me = {};
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(port);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&si_me, sizeof(si_me)) != 0) {
      av_log(nullptr, AV_LOG_ERROR, "bind\n");
      deinit();
      return false;
    }

    return true;
  }

public:
  enum EventType : int64_t { E_Keyframe, E_Quality, E_Snapshot };
  struct Event {
    EventType type;
    union {
      double quality;
    };
  };

  ControlInterface() : s(-1) {}
  ~ControlInterface() { deinit(); }

  bool init_send() { return init(0); }

  bool init_recv() { return init(CONTROL_PORT); }

  void deinit() {
    if (s != -1) {
      close(s);
      s = -1;
    }
  }

  bool recv_event(Event &e) const {
    if (s == -1)
      return false;
    struct sockaddr_in si_other = {};
    socklen_t slen = sizeof(si_other);
    int recv_len;
    if ((recv_len = recvfrom(s, (sock_dptr_t)&e, sizeof(e), 0,
                             (struct sockaddr *)&si_other, &slen)) == -1) {
      return false;
    }
    return recv_len == sizeof(e);
  }

  bool send_event(const Event &e, const struct sockaddr_in &si_other) const {
    return sendto(s, (sock_dptr_t)&e, sizeof(e), 0,
                  (struct sockaddr *)&si_other, sizeof(si_other)) == sizeof(e);
  }

  bool send_emit_keyframe(const struct sockaddr_in &si_other) const {
    Event e = {};
    e.type = E_Keyframe;
    return send_event(e, si_other);
  }

  bool send_quality(double q, const struct sockaddr_in &si_other) const {
    Event e = {};
    e.type = E_Quality;
    e.quality = q;
    return send_event(e, si_other);
  }

  bool send_req_snapshot(const struct sockaddr_in &si_other) const {
    Event e = {};
    e.type = E_Snapshot;
    return send_event(e, si_other);
  }

  static struct sockaddr_in make_addr(const char *addr_str) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    sockaddr_in si = {};
    si.sin_family = AF_INET;
    si.sin_port = htons(CONTROL_PORT);

    struct addrinfo *res = nullptr;
    if (getaddrinfo(addr_str, nullptr, &hints, &res) == 0) {
      si.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
    }
    freeaddrinfo(res);

    return si;
  }
};

#ifdef __MINGW32__
#undef close
#endif
