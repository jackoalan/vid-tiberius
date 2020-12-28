#pragma once

#include "nullptr.h"
#include <vector>

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

class ControlInterface {
  SOCKET s;

public:
  enum EventType : int64_t { E_Keyframe, E_Quality, E_Snapshot, E_Control };
  struct Event {
    EventType type;
    union {
      double quality;
      struct {
        uint32_t ctrl_id;
        int32_t ctrl_val;
      };
    };
  };

  ControlInterface() : s(-1) {}
  ~ControlInterface() { deinit(); }

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
      av_log(nullptr, AV_LOG_ERROR, "recv socket\n");
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

  void deinit() {
    if (s != -1) {
      close(s);
      s = -1;
    }
  }

  /* receiver -> sender communication */

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
    if (si_other.sin_addr.s_addr)
      return sendto(s, (sock_dptr_t)&e, sizeof(e), 0,
                    (struct sockaddr *)&si_other,
                    sizeof(si_other)) == sizeof(e);
    return false;
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

  bool send_ctrl(uint32_t id, int32_t value,
                 const struct sockaddr_in &si_other) const {
    Event e = {};
    e.type = E_Control;
    e.ctrl_id = id;
    e.ctrl_val = value;
    return send_event(e, si_other);
  }

  /* sender -> receiver communication */

  std::vector<uint8_t>::const_iterator
  recv_ctrls(std::vector<uint8_t> &ctrls) const {
    if (s == -1)
      return ctrls.begin();

    ctrls.resize(65535);
    struct sockaddr_in si_other = {};
    socklen_t slen = sizeof(si_other);
    int recv_len;
    if ((recv_len = recvfrom(s, (sock_dptr_t)ctrls.data(), ctrls.size(), 0,
                             (struct sockaddr *)&si_other, &slen)) == -1) {
      return ctrls.begin();
    }

    return ctrls.begin() + recv_len;
  }

  bool send_ctrls(const std::vector<uint8_t> &data,
                  const struct sockaddr_in &si_other) const {
    if (data.size() > 65535)
      return false;
    if (si_other.sin_addr.s_addr)
      return sendto(s, (sock_dptr_t)data.data(), data.size(), 0,
                    (struct sockaddr *)&si_other,
                    sizeof(si_other)) == data.size();
    return false;
  }
};

#ifdef __MINGW32__
#undef close
#endif
