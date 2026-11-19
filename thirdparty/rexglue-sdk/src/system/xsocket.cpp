/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstring>

#include <rex/kernel/xam/module.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
#include <rex/system/xsocket_native.h>

namespace rex::system {

namespace {

std::atomic<XSocket::PortHook> g_on_bind{nullptr};
std::atomic<XSocket::PortHook> g_on_close{nullptr};

sock::Endpoint FromGuest(const XSOCKADDR_IN* a) {
  return sock::Endpoint{static_cast<uint32_t>(a->sin_addr), static_cast<uint16_t>(a->sin_port)};
}

void ToGuest(const sock::Endpoint& e, XSOCKADDR_IN* a) {
  a->sin_family = 2;  // AF_INET
  a->sin_port = e.port;
  a->sin_addr = e.ip;
  std::memset(a->x_sin_zero, 0, sizeof(a->x_sin_zero));
}

}  // namespace

void XSocket::SetPortHooks(PortHook on_bind, PortHook on_close) {
  g_on_bind.store(on_bind);
  g_on_close.store(on_close);
}

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  native_handle_ = sock::Open(af, type, proto);
  if (native_handle_ == sock::kInvalidHandle) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

int XSocket::Close() {
  if (native_handle_ == sock::kInvalidHandle) {
    return 0;
  }
  if (port_reported_) {
    port_reported_ = false;
    if (auto hook = g_on_close.load()) {
      hook(bound_port_, type_ == X_SOCK_STREAM ? "TCP" : "UDP");
    }
  }
  int ret = sock::Close(native_handle_);
  native_handle_ = sock::kInvalidHandle;
  return ret;
}

int XSocket::SetOption(uint32_t level, uint32_t optname, const uint8_t* optval_ptr,
                       uint32_t optlen) {
  if (level == sock::kXSolSocket &&
      (optname == sock::kXSoMarkInsecure || optname == sock::kXSoPrivate ||
       optname == sock::kXSoGrantInsecure)) {
    // Disable socket encryption
    secure_ = false;
    return 0;
  }
  // SO_LINGER and SO_DONTLINGER only apply to stream sockets.
  if (level == sock::kXSolSocket &&
      (optname == sock::kXSoLinger || optname == ~sock::kXSoLinger) && type_ == X_SOCK_DGRAM) {
    sock::SetError(sock::kWsaENOPROTOOPT);
    return -1;
  }

  int ret = sock::SetOption(native_handle_, level, optname, optval_ptr, optlen);
  if (ret == 0 && level == sock::kXSolSocket && optname == sock::kXSoBroadcast) {
    broadcast_socket_ = true;
  }
  return ret;
}

int XSocket::GetOption(uint32_t level, uint32_t optname, uint8_t* optval_ptr, uint32_t* optlen) {
  if (level == sock::kXSolSocket &&
      (optname == sock::kXSoLinger || optname == ~sock::kXSoLinger) && type_ == X_SOCK_DGRAM) {
    sock::SetError(sock::kWsaENOPROTOOPT);
    return -1;
  }
  return sock::GetOption(native_handle_, level, optname, optval_ptr, optlen);
}

int XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  return sock::IoControl(native_handle_, cmd, arg_ptr);
}

int XSocket::Connect(const XSOCKADDR_IN* name) {
  int ret = sock::Connect(native_handle_, FromGuest(name));
  // connect() binds a random local port; remember it, but don't open it on the
  // router (see NoteBound).
  if (!bound_port_) {
    const uint32_t error = sock::LastError();
    sock::Endpoint local;
    if (sock::GetSockName(native_handle_, &local) == 0 && local.port) {
      bound_port_ = local.port;
      bound_ = true;
    }
    sock::SetError(error);
  }
  return ret;
}

int XSocket::Bind(const XSOCKADDR_IN* name) {
  sock::Endpoint local = FromGuest(name);
  int ret = sock::Bind(native_handle_, local);
  if (ret < 0) {
    return ret;
  }
  bound_ = true;
  bound_port_ = local.port;
  NoteBound();
  return 0;
}

void XSocket::NoteBound() {
  if (!bound_port_) {
    sock::Endpoint name;
    if (sock::GetSockName(native_handle_, &name) == 0) {
      bound_port_ = name.port;
    }
  }
  bound_ = true;
  // Only explicitly chosen ports are worth opening on the router: Xbox Live
  // games bind fixed ports (often 1000) and tell other players about them.
  // Random ports from connect() / sendto() are answered through the NAT anyway.
  if (bound_port_ && !port_reported_) {
    if (auto hook = g_on_bind.load()) {
      port_reported_ = true;
      hook(bound_port_, type_ == X_SOCK_STREAM ? "TCP" : "UDP");
    }
  }
}

int XSocket::Listen(int backlog) {
  return sock::Listen(native_handle_, backlog);
}

object_ref<XSocket> XSocket::Accept(XSOCKADDR_IN* name) {
  sock::Endpoint remote;
  sock::Handle h = sock::Accept(native_handle_, &remote);
  if (h == sock::kInvalidHandle) {
    return nullptr;
  }
  if (name) {
    ToGuest(remote, name);
  }

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, h));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;
  socket->secure_ = secure_;
  socket->bound_ = true;
  socket->bound_port_ = bound_port_;
  return socket;
}

int XSocket::Shutdown(int how) {
  return sock::Shutdown(native_handle_, how);
}

int XSocket::GetSockName(XSOCKADDR_IN* name) {
  sock::Endpoint e;
  if (sock::GetSockName(native_handle_, &e) < 0) {
    return -1;
  }
  ToGuest(e, name);
  return 0;
}

int XSocket::GetPeerName(XSOCKADDR_IN* name) {
  sock::Endpoint e;
  if (sock::GetPeerName(native_handle_, &e) < 0) {
    return -1;
  }
  ToGuest(e, name);
  return 0;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return sock::Recv(native_handle_, buf, buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from) {
  sock::Endpoint e;
  int ret = sock::RecvFrom(native_handle_, buf, buf_len, flags, &e);
  if (ret >= 0 && from) {
    ToGuest(e, from);
  }
  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return sock::Send(native_handle_, buf, buf_len, flags);
}

int XSocket::SendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    const XSOCKADDR_IN* to) {
  // Datagram sockets can't send to address 0 or port 0 (Xenia Netplay does the same).
  if (to && type_ == X_SOCK_DGRAM && (!to->sin_addr || !to->sin_port)) {
    sock::SetError(sock::kWsaEADDRNOTAVAIL);
    return -1;
  }
  if (!to) {
    return sock::Send(native_handle_, buf, buf_len, flags);
  }
  sock::Endpoint target = FromGuest(to);
  int ret = sock::SendTo(native_handle_, buf, buf_len, flags, &target);
  if (!bound_port_) {
    const uint32_t error = sock::LastError();
    sock::Endpoint name;
    if (sock::GetSockName(native_handle_, &name) == 0) {
      bound_port_ = name.port;  // implicit bind; not reported (see NoteBound)
      bound_ = true;
    }
    sock::SetError(error);
  }
  return ret;
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
