#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <queue>

#include <rex/math.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, rex::countof(sa_data));
    return *this;
  }

  uint16_t address_family;
  char sa_data[14];
};

struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;

  // Always big-endian!
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  uint16_t sin_family;
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

// Socket calls follow Winsock: 0 or a count on success, -1 on failure, and then
// sock::LastError() (rex/system/xsocket_native.h) holds the Xbox error code.
class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  // Called when a game socket binds a port and when that socket closes, so the
  // online layer can open the port on the router (UPnP). protocol: "UDP" / "TCP".
  using PortHook = void (*)(uint16_t port, const char* protocol);
  static void SetPortHooks(PortHook on_bind, PortHook on_close);

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }
  Type type() const { return type_; }
  bool secure() const { return secure_; }
  bool broadcast() const { return broadcast_socket_; }

  // X_STATUS for NetDll_socket (the caller turns it into an error code).
  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  int Close();

  int SetOption(uint32_t level, uint32_t optname, const uint8_t* optval_ptr, uint32_t optlen);
  int GetOption(uint32_t level, uint32_t optname, uint8_t* optval_ptr, uint32_t* optlen);
  int IOControl(uint32_t cmd, uint8_t* arg_ptr);

  // Addresses are the game's (big-endian fields).
  int Connect(const XSOCKADDR_IN* name);
  int Bind(const XSOCKADDR_IN* name);
  int Listen(int backlog);
  object_ref<XSocket> Accept(XSOCKADDR_IN* name);
  int Shutdown(int how);
  int GetSockName(XSOCKADDR_IN* name);
  int GetPeerName(XSOCKADDR_IN* name);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  // from / to may be null.
  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from);
  int SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags, const XSOCKADDR_IN* to);

  struct packet {
    // These values are in network byte order.
    rex::be<uint16_t> src_port;
    rex::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  void NoteBound();

  uint64_t native_handle_ = ~uint64_t(0);

  AddressFamily af_ = X_AF_INET;  // Address family
  Type type_ = X_SOCK_DGRAM;      // Type (DGRAM/Stream/etc)
  Protocol proto_ = X_IPPROTO_UDP;  // Protocol (TCP/UDP/etc)
  bool secure_ = true;            // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;
  bool port_reported_ = false;  // on_bind hook called for bound_port_

  bool broadcast_socket_ = false;

  std::unique_ptr<rex::thread::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;
};

}  // namespace rex::system
