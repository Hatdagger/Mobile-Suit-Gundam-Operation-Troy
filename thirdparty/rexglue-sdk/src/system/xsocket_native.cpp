/**
 * @file        system/xsocket_native.cpp
 * @brief       Xbox 360 socket calls on the PC's own sockets (see xsocket_native.h)
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#include <rex/system/xsocket_native.h>

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <mstcpip.h>  // SIO_UDP_CONNRESET
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace rex::system::sock {

namespace {

thread_local uint32_t t_last_error = 0;

#if defined(_WIN32)
using NativeSocket = SOCKET;
NativeSocket N(Handle h) {
  return static_cast<SOCKET>(h);
}
void CaptureError() {
  t_last_error = static_cast<uint32_t>(WSAGetLastError());
}
#else
using NativeSocket = int;
NativeSocket N(Handle h) {
  return static_cast<int>(h);
}
void CaptureError() {
  t_last_error = ErrnoToWsa(errno);
}
#endif

// Returns ret, saving the error when it is negative.
int Check(int ret) {
  if (ret < 0) {
    CaptureError();
    return -1;
  }
  return ret;
}

int Fail(uint32_t code) {
  t_last_error = code;
  return -1;
}

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t LoadBe16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
void StoreBe16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

sockaddr_in ToNative(Endpoint e) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(e.port);
  a.sin_addr.s_addr = htonl(e.ip);
  return a;
}

Endpoint FromNative(const sockaddr_in& a) {
  return Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
}

// Xbox recv/send flags: MSG_OOB 1, MSG_PEEK 2 (same on Windows and POSIX).
int RecvFlags(uint32_t flags) {
  return static_cast<int>(flags & 0x3);
}
int SendFlags(uint32_t flags) {
  int native = static_cast<int>(flags & 0x1);
#if defined(MSG_NOSIGNAL)
  native |= MSG_NOSIGNAL;  // a closed TCP peer must not kill the program
#endif
  return native;
}

enum class OptKind {
  kInt,        // 4-byte integer / bool
  kLinger,     // Xbox: two big-endian u16
  kTimeout,    // Xbox: milliseconds (u32)
  kDontLinger, // ~SO_LINGER
  kExclusive,  // ~SO_REUSEADDR (SO_EXCLUSIVEADDRUSE)
  kError,      // SO_ERROR (read only)
};

struct NativeOpt {
  int level = 0;
  int name = 0;
  OptKind kind = OptKind::kInt;
  bool read_only = false;
};

bool Translate(uint32_t x_level, uint32_t x_opt, NativeOpt* out) {
  if (x_level == kXIpprotoTcp) {
    if (x_opt == 0x0001) {
      *out = {IPPROTO_TCP, TCP_NODELAY, OptKind::kInt, false};
      return true;
    }
    return false;
  }
  if (x_level != kXSolSocket) {
    return false;
  }
  switch (x_opt) {
    case 0x0002:
      *out = {SOL_SOCKET, SO_ACCEPTCONN, OptKind::kInt, true};
      return true;
    case kXSoReuseAddr:
      *out = {SOL_SOCKET, SO_REUSEADDR, OptKind::kInt, false};
      return true;
    case 0x0008:
      *out = {SOL_SOCKET, SO_KEEPALIVE, OptKind::kInt, false};
      return true;
    case kXSoBroadcast:
      *out = {SOL_SOCKET, SO_BROADCAST, OptKind::kInt, false};
      return true;
    case kXSoLinger:
      *out = {SOL_SOCKET, SO_LINGER, OptKind::kLinger, false};
      return true;
    case 0x1001:
      *out = {SOL_SOCKET, SO_SNDBUF, OptKind::kInt, false};
      return true;
    case 0x1002:
      *out = {SOL_SOCKET, SO_RCVBUF, OptKind::kInt, false};
      return true;
    case 0x1005:
      *out = {SOL_SOCKET, SO_SNDTIMEO, OptKind::kTimeout, false};
      return true;
    case 0x1006:
      *out = {SOL_SOCKET, SO_RCVTIMEO, OptKind::kTimeout, false};
      return true;
    case 0x1007:
      *out = {SOL_SOCKET, SO_ERROR, OptKind::kError, true};
      return true;
    case 0x1008:
      *out = {SOL_SOCKET, SO_TYPE, OptKind::kInt, true};
      return true;
    case ~kXSoLinger:
      *out = {SOL_SOCKET, SO_LINGER, OptKind::kDontLinger, false};
      return true;
    case ~kXSoReuseAddr:
#if defined(_WIN32)
      *out = {SOL_SOCKET, SO_EXCLUSIVEADDRUSE, OptKind::kExclusive, false};
#else
      *out = {SOL_SOCKET, 0, OptKind::kExclusive, false};
#endif
      return true;
    default:
      return false;
  }
}

int NativeSet(Handle h, int level, int name, const void* value, int len) {
  return Check(setsockopt(N(h), level, name, static_cast<const char*>(value),
                          static_cast<socklen_t>(len)));
}

int NativeGet(Handle h, int level, int name, void* value, socklen_t* len) {
  return Check(getsockopt(N(h), level, name, static_cast<char*>(value), len));
}

int NativeGetInt(Handle h, int level, int name, int* value) {
  *value = 0;
  socklen_t len = sizeof(*value);
  return NativeGet(h, level, name, value, &len);
}

}  // namespace

uint32_t LastError() {
  return t_last_error;
}

void SetError(uint32_t code) {
  t_last_error = code;
}

uint32_t ErrnoToWsa(int err) {
#if defined(_WIN32)
  return err ? kWsaEINVAL : 0;
#else
  switch (err) {
    case 0:
      return 0;
    case EINTR:
      return kWsaEINTR;
    case EBADF:
      return 10009;
    case EACCES:
      return 10013;
    case EFAULT:
      return kWsaEFAULT;
    case EINVAL:
      return kWsaEINVAL;
    case EMFILE:
      return 10024;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
      return kWsaEWOULDBLOCK;
    case EINPROGRESS:
      // A non-blocking connect: Winsock reports WSAEWOULDBLOCK here.
      return kWsaEWOULDBLOCK;
    case EALREADY:
      return 10037;
    case ENOTSOCK:
      return kWsaENOTSOCK;
    case EDESTADDRREQ:
      return 10039;
    case EMSGSIZE:
      return kWsaEMSGSIZE;
    case EPROTOTYPE:
      return 10041;
    case ENOPROTOOPT:
      return kWsaENOPROTOOPT;
    case EPROTONOSUPPORT:
      return 10043;
    case EOPNOTSUPP:
      return 10045;
    case EAFNOSUPPORT:
      return 10047;
    case EADDRINUSE:
      return kWsaEADDRINUSE;
    case EADDRNOTAVAIL:
      return kWsaEADDRNOTAVAIL;
    case ENETDOWN:
      return 10050;
    case ENETUNREACH:
      return 10051;
    case ENETRESET:
      return 10052;
    case ECONNABORTED:
      return 10053;
    case ECONNRESET:
    case EPIPE:
      return kWsaECONNRESET;
    case ENOBUFS:
      return 10055;
    case EISCONN:
      return 10056;
    case ENOTCONN:
      return kWsaENOTCONN;
    case ETIMEDOUT:
      return 10060;
    case ECONNREFUSED:
      return 10061;
    case EHOSTUNREACH:
      return 10065;
    default:
      return kWsaEINVAL;
  }
#endif
}

void EnsureStarted() {
#if defined(_WIN32)
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

Handle Open(uint32_t af, uint32_t type, uint32_t protocol) {
  EnsureStarted();
  if (protocol == 254) {
    protocol = IPPROTO_UDP;  // VDP: game data over UDP (sent as is, like Xenia)
  }
  NativeSocket s = socket(static_cast<int>(af ? af : AF_INET), static_cast<int>(type),
                          static_cast<int>(protocol));
#if defined(_WIN32)
  if (s == INVALID_SOCKET) {
    CaptureError();
    return kInvalidHandle;
  }
  if (type == kXSockDgram) {
    // Windows reports WSAECONNRESET on a UDP socket after sending to a port
    // nobody listens on (a player who just left). The Xbox never does, and the
    // game may treat it as a dead socket, so turn it off.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr,
             nullptr);
  }
#else
  if (s < 0) {
    CaptureError();
    return kInvalidHandle;
  }
#if defined(SO_NOSIGPIPE)
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
  return static_cast<Handle>(s);
}

int Close(Handle h) {
#if defined(_WIN32)
  return Check(closesocket(N(h)));
#else
  return Check(close(N(h)));
#endif
}

int Bind(Handle h, Endpoint local) {
  sockaddr_in a = ToNative(local);
  return Check(bind(N(h), reinterpret_cast<sockaddr*>(&a), sizeof(a)));
}

int Connect(Handle h, Endpoint remote) {
  sockaddr_in a = ToNative(remote);
  return Check(connect(N(h), reinterpret_cast<sockaddr*>(&a), sizeof(a)));
}

int Listen(Handle h, int backlog) {
  return Check(listen(N(h), backlog));
}

Handle Accept(Handle h, Endpoint* remote) {
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  NativeSocket s = accept(N(h), reinterpret_cast<sockaddr*>(&a), &len);
#if defined(_WIN32)
  if (s == INVALID_SOCKET) {
#else
  if (s < 0) {
#endif
    CaptureError();
    return kInvalidHandle;
  }
  if (remote) {
    *remote = FromNative(a);
  }
  return static_cast<Handle>(s);
}

int Shutdown(Handle h, int how) {
  return Check(shutdown(N(h), how));
}

int GetSockName(Handle h, Endpoint* out) {
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  if (Check(getsockname(N(h), reinterpret_cast<sockaddr*>(&a), &len)) < 0) {
    return -1;
  }
  *out = FromNative(a);
  return 0;
}

int GetPeerName(Handle h, Endpoint* out) {
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  if (Check(getpeername(N(h), reinterpret_cast<sockaddr*>(&a), &len)) < 0) {
    return -1;
  }
  *out = FromNative(a);
  return 0;
}

int Send(Handle h, const void* buf, uint32_t len, uint32_t flags) {
  return Check(static_cast<int>(
      send(N(h), static_cast<const char*>(buf), static_cast<int>(len), SendFlags(flags))));
}

int Recv(Handle h, void* buf, uint32_t len, uint32_t flags) {
  return Check(static_cast<int>(
      recv(N(h), static_cast<char*>(buf), static_cast<int>(len), RecvFlags(flags))));
}

int SendTo(Handle h, const void* buf, uint32_t len, uint32_t flags, const Endpoint* to) {
  if (!to) {
    return Send(h, buf, len, flags);
  }
  sockaddr_in a = ToNative(*to);
  return Check(static_cast<int>(sendto(N(h), static_cast<const char*>(buf),
                                       static_cast<int>(len), SendFlags(flags),
                                       reinterpret_cast<sockaddr*>(&a), sizeof(a))));
}

int RecvFrom(Handle h, void* buf, uint32_t len, uint32_t flags, Endpoint* from) {
  sockaddr_in a{};
  socklen_t alen = sizeof(a);
  int ret = static_cast<int>(recvfrom(N(h), static_cast<char*>(buf), static_cast<int>(len),
                                      RecvFlags(flags), reinterpret_cast<sockaddr*>(&a), &alen));
  if (ret < 0) {
    CaptureError();
#if defined(_WIN32)
    // A datagram bigger than the buffer: Winsock fills the buffer and fails
    // with WSAEMSGSIZE, like the Xbox. Nothing to change.
#endif
    return -1;
  }
  if (from) {
    *from = FromNative(a);
  }
  return ret;
}

int SetOption(Handle h, uint32_t x_level, uint32_t x_optname, const uint8_t* guest_value,
              uint32_t len) {
  if (x_level == kXSolSocket &&
      (x_optname == kXSoMarkInsecure || x_optname == kXSoPrivate ||
       x_optname == kXSoGrantInsecure)) {
    return 0;  // Xbox network security options: nothing to do on the PC
  }
  NativeOpt opt;
  if (!Translate(x_level, x_optname, &opt)) {
    return Fail(x_level == kXSolSocket || x_level == kXIpprotoTcp ? kWsaENOPROTOOPT : kWsaEINVAL);
  }
  if (opt.read_only) {
    return Fail(kWsaENOPROTOOPT);
  }
  if (!guest_value || len == 0) {
    return Fail(kWsaEFAULT);
  }
  // A value shorter than 4 bytes is read as its first byte (Xenia does the same).
  const uint32_t value = len >= 4 ? LoadBe32(guest_value) : guest_value[0];

  switch (opt.kind) {
    case OptKind::kInt:
    case OptKind::kError: {
      int v = static_cast<int>(value);
      return NativeSet(h, opt.level, opt.name, &v, sizeof(v)) < 0 ? -1 : 0;
    }
    case OptKind::kLinger: {
      if (len < 4) {
        return Fail(kWsaEFAULT);
      }
      linger l{};
      l.l_onoff = LoadBe16(guest_value);
      l.l_linger = LoadBe16(guest_value + 2);
      return NativeSet(h, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0 ? -1 : 0;
    }
    case OptKind::kDontLinger: {
      linger l{};
      l.l_onoff = value ? 0 : 1;
      return NativeSet(h, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0 ? -1 : 0;
    }
    case OptKind::kTimeout: {
#if defined(_WIN32)
      DWORD ms = value;
      return NativeSet(h, opt.level, opt.name, &ms, sizeof(ms)) < 0 ? -1 : 0;
#else
      timeval tv{};
      tv.tv_sec = static_cast<decltype(tv.tv_sec)>(value / 1000);
      tv.tv_usec = static_cast<decltype(tv.tv_usec)>((value % 1000) * 1000);
      return NativeSet(h, opt.level, opt.name, &tv, sizeof(tv)) < 0 ? -1 : 0;
#endif
    }
    case OptKind::kExclusive: {
#if defined(_WIN32)
      int v = static_cast<int>(value);
      return NativeSet(h, opt.level, opt.name, &v, sizeof(v)) < 0 ? -1 : 0;
#else
      return 0;  // POSIX has no exclusive-address option; its default already refuses sharing
#endif
    }
  }
  return Fail(kWsaEINVAL);
}

int GetOption(Handle h, uint32_t x_level, uint32_t x_optname, uint8_t* guest_value,
              uint32_t* len) {
  if (!guest_value || !len || *len == 0) {
    return Fail(kWsaEFAULT);
  }
  NativeOpt opt;
  if (!Translate(x_level, x_optname, &opt)) {
    return Fail(x_level == kXSolSocket || x_level == kXIpprotoTcp ? kWsaENOPROTOOPT : kWsaEINVAL);
  }

  uint32_t value = 0;
  switch (opt.kind) {
    case OptKind::kInt: {
      int v = 0;
      if (NativeGetInt(h, opt.level, opt.name, &v) < 0) {
        return -1;
      }
      value = v ? static_cast<uint32_t>(v) : 0;
      break;
    }
    case OptKind::kError: {
      int v = 0;
      if (NativeGetInt(h, opt.level, opt.name, &v) < 0) {
        return -1;
      }
#if defined(_WIN32)
      value = static_cast<uint32_t>(v);  // already a Winsock code
#else
      value = ErrnoToWsa(v);
#endif
      break;
    }
    case OptKind::kLinger:
    case OptKind::kDontLinger: {
      linger l{};
      socklen_t l_len = sizeof(l);
      if (NativeGet(h, SOL_SOCKET, SO_LINGER, &l, &l_len) < 0) {
        return -1;
      }
      if (opt.kind == OptKind::kDontLinger) {
        value = l.l_onoff ? 0 : 1;
        break;
      }
      if (*len < 4) {
        return Fail(kWsaEFAULT);
      }
      StoreBe16(guest_value, static_cast<uint16_t>(l.l_onoff));
      StoreBe16(guest_value + 2, static_cast<uint16_t>(l.l_linger));
      *len = 4;
      return 0;
    }
    case OptKind::kTimeout: {
#if defined(_WIN32)
      DWORD ms = 0;
      socklen_t t_len = sizeof(ms);
      if (NativeGet(h, opt.level, opt.name, &ms, &t_len) < 0) {
        return -1;
      }
      value = ms;
#else
      timeval tv{};
      socklen_t t_len = sizeof(tv);
      if (NativeGet(h, opt.level, opt.name, &tv, &t_len) < 0) {
        return -1;
      }
      value = static_cast<uint32_t>(tv.tv_sec * 1000 + tv.tv_usec / 1000);
#endif
      break;
    }
    case OptKind::kExclusive: {
#if defined(_WIN32)
      int v = 0;
      if (NativeGetInt(h, opt.level, opt.name, &v) < 0) {
        return -1;
      }
      value = v ? 1 : 0;
#else
      value = 0;
#endif
      break;
    }
  }

  if (*len < 4) {
    // A short buffer gets the low byte (Xenia does the same).
    std::memset(guest_value, 0, *len);
    guest_value[0] = static_cast<uint8_t>(value);
    *len = 1;
    return 0;
  }
  StoreBe32(guest_value, value);
  *len = 4;
  return 0;
}

int IoControl(Handle h, uint32_t x_cmd, uint8_t* guest_arg) {
  if (!guest_arg) {
    return Fail(kWsaEFAULT);
  }
  if (x_cmd == kXFionbio) {
    const bool non_blocking = LoadBe32(guest_arg) != 0;
#if defined(_WIN32)
    u_long mode = non_blocking ? 1 : 0;
    return Check(ioctlsocket(N(h), FIONBIO, &mode)) < 0 ? -1 : 0;
#else
    int flags = fcntl(N(h), F_GETFL, 0);
    if (flags < 0) {
      return Check(-1);
    }
    flags = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return Check(fcntl(N(h), F_SETFL, flags)) < 0 ? -1 : 0;
#endif
  }
  if (x_cmd == kXFionread) {
#if defined(_WIN32)
    u_long available = 0;
    if (Check(ioctlsocket(N(h), FIONREAD, &available)) < 0) {
      return -1;
    }
#else
    int available = 0;
    if (Check(ioctl(N(h), FIONREAD, &available)) < 0) {
      return -1;
    }
#endif
    StoreBe32(guest_arg, static_cast<uint32_t>(available));
    return 0;
  }
  return Fail(kWsaEINVAL);
}

int NativeFd(Handle h) {
  return static_cast<int>(h);
}

}  // namespace rex::system::sock
