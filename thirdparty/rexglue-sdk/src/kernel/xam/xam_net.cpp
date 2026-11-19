/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/live/live_service.h>
#include <rex/kernel/xam/live/xnet.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>                    // getaddrinfo
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// XNADDR, XNDNS, XNQOS and friends: rex/kernel/xam/live/xnet.h

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetStartupEx_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params, u32 version) {
  return NetDll_XNetStartup_entry(caller, params);
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  if (!buffer_ptr || !length) {
    return 0;
  }
  if (live::LiveService::Get().mode() == live::NetworkMode::kOffline) {
    // Offline: constant values. This makes replicating things easier.
    std::memset(buffer_ptr, 0xBB, length);
    return 0;
  }
  // Online, every console must make different keys and nonces (Xenia Canary Netplay).
  thread_local std::mt19937_64 gen(std::random_device{}());
  std::uniform_int_distribution<uint32_t> dist(0, 255);
  auto* bytes = static_cast<uint8_t*>(buffer_ptr);
  for (uint32_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<uint8_t>(dist(gen));
  }
  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  if (overlapped_ptr) {
    // auto evt = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

// ---------------------------------------------------------------------------
// XNet addresses, QoS, keys and DNS.
// Online and LAN behaviour follows Xenia Canary Netplay
// (src/xenia/kernel/xam/xam_net.cc, netplay_canary_experimental 6342e12).
// In offline mode (network_mode = offline) the SDK's earlier answers are kept,
// so single-player behaves as before.
// ---------------------------------------------------------------------------

namespace {

live::LiveService& Live() {
  return live::LiveService::Get();
}

bool NetworkOff() {
  return Live().mode() == live::NetworkMode::kOffline;
}

// The IP numbers XNet functions receive are host-order values; in_addr wants network order.
uint32_t ToNetworkOrder(uint32_t ip) {
  return rex::byte_swap(ip);
}

void StoreMac(uint8_t* out, uint64_t mac) {
  for (int i = 5; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(mac & 0xFF);
    mac >>= 8;
  }
}

uint64_t LoadMac(const uint8_t* in) {
  uint64_t mac = 0;
  for (int i = 0; i < 6; ++i) {
    mac = (mac << 8) | in[i];
  }
  return mac;
}

// XLiveAPI::IpGetConsoleXnAddr: this console's XNADDR.
void FillOwnXnAddr(XNADDR* addr) {
  std::memset(addr, 0, sizeof(XNADDR));
  auto& live = Live();
  if (live.SignedInToLive()) {
    addr->ina.s_addr = live.online_ip();
    addr->inaOnline.s_addr = live.online_ip();
    addr->wPortOnline = live.player_port();
  } else if (live.mode() != live::NetworkMode::kOffline) {
    addr->ina.s_addr = live.local_ip();
  } else {
    addr->ina.s_addr = htonl(INADDR_LOOPBACK);  // offline: as the SDK did before
  }
  addr->abOnline[kXnAddrPlatformTypeOffset] = kPlatformXbox360;
  // A real, per-PC MAC. RakNet-based games need it non-zero (see 58411436).
  StoreMac(addr->abEnet, live.identity().mac_address);
}

// Background lookups (DNS, QoS) the game can cancel by releasing the result.
std::mutex lookup_mutex;
std::map<uint32_t, std::shared_ptr<std::atomic<bool>>> lookups;  // guest address -> cancelled

std::shared_ptr<std::atomic<bool>> TrackLookup(uint32_t guest_address) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard lock(lookup_mutex);
  lookups[guest_address] = cancelled;
  return cancelled;
}

// Returns false if the lookup was never tracked (already released).
bool ReleaseLookup(uint32_t guest_address) {
  std::lock_guard lock(lookup_mutex);
  const auto it = lookups.find(guest_address);
  if (it == lookups.end()) {
    return false;
  }
  it->second->store(true);
  lookups.erase(it);
  return true;
}

std::atomic<bool> explicit_live_key{false};

}  // namespace

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  if (!addr_ptr) {
    return XNET_GET_XNADDR_NONE;
  }
  FillOwnXnAddr(addr_ptr);

  if (NetworkOff()) {
    return XNET_GET_XNADDR_STATIC;  // as before
  }
  uint32_t status = XNET_GET_XNADDR_STATIC | XNET_GET_XNADDR_GATEWAY | XNET_GET_XNADDR_DNS;
  if (Live().SignedInToLive()) {
    status |= XNET_GET_XNADDR_ONLINE;
  }
  return status;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetGetXnAddrPlatform_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                       mapped_u32 platform_type) {
  // 58411457 filters session search based on platform type
  if (!addr_ptr || !platform_type) {
    return X_WSAEINVAL;
  }
  *platform_type = addr_ptr->abOnline[kXnAddrPlatformTypeOffset];
  return 0;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr,
                                       mapped_u64 id_ptr) {
  if (NetworkOff()) {
    // Tell the caller we're not signed in to live (non-zero ret)
    return 1;
  }
  if (!id_ptr) {
    return X_WSAEINVAL;
  }
  *id_ptr = 0;
  if (!addr_ptr || !addr_ptr->inaOnline.s_addr || !addr_ptr->wPortOnline) {
    return X_WSAEINVAL;
  }
  *id_ptr = live::MachineIdFromMac(LoadMac(addr_ptr->abEnet));
  return 0;
}

u32 NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                    u32 string_size) {
  if (!string_out || !string_size) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const std::string text = live::Ipv4ToString(ToNetworkOrder(in_addr));
  rex::string::copy_truncating(string_out, text, string_size);
  return X_ERROR_SUCCESS;
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    ppc_ptr_t<XIN_ADDR> in_addr) {
  if (NetworkOff()) {
    return 1;  // as before
  }
  if (!xn_addr || !in_addr) {
    return X_WSAEINVAL;
  }
  in_addr->s_addr = 0;

  // Our own address: talk to ourselves (494707E4, 4E4D07D1).
  if (LoadMac(xn_addr->abEnet) == Live().identity().mac_address) {
    in_addr->s_addr = htonl(INADDR_LOOPBACK);
    return X_ERROR_SUCCESS;
  }
  if (Live().mode() == live::NetworkMode::kLan) {
    in_addr->s_addr = xn_addr->ina.s_addr;
  } else {
    in_addr->s_addr = xn_addr->inaOnline.s_addr;
  }
  return X_ERROR_SUCCESS;
}

// Does the reverse of the above.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    ppc_ptr_t<XNKID> xid_ptr) {
  if (NetworkOff()) {
    return 1;  // as before
  }
  if (xn_addr) {
    std::memset(xn_addr, 0, sizeof(XNADDR));
  }
  if (xid_ptr) {
    std::memset(xid_ptr, 0, sizeof(XNKID));
  }
  if (!xn_addr) {
    return X_WSAEINVAL;
  }

  const uint32_t own_local_ip = Live().local_ip();
  if (in_addr == kIpLoopback || in_addr == kIpBroadcast ||
      (own_local_ip && ToNetworkOrder(in_addr) == own_local_ip)) {
    FillOwnXnAddr(xn_addr);
    return X_ERROR_SUCCESS;
  }

  const uint32_t ip = ToNetworkOrder(in_addr);
  xn_addr->ina.s_addr = ip;
  xn_addr->inaOnline.s_addr = ip;
  xn_addr->wPortOnline = Live().player_port();
  xn_addr->abOnline[kXnAddrPlatformTypeOffset] = kPlatformXbox360;

  // The server knows the MAC (and session) of the player at that address.
  const auto remote = Live().LookupRemote(ip);
  StoreMac(xn_addr->abEnet, remote.mac_address);
  if (xid_ptr && remote.session_id) {
    xid_ptr->set_u64(remote.session_id);
  }
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 addr) {
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 addr) {
  // 43430806, 43430821 and 5841124E fail to connect without sleep.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 addr) {
  return XNET_CONNECT_STATUS_CONNECTED;
}

u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 server_addr, u32 service_id,
                                    ppc_ptr_t<XIN_ADDR> pina) {
  if (!Live().Connected()) {
    return X_WSANOTINITIALISED;
  }
  if (!server_addr || !service_id || !pina) {
    return X_WSAEINVAL;
  }
  pina->s_addr = ToNetworkOrder(server_addr);
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetInAddrToServer_entry(u32 caller, u32 server_addr, ppc_ptr_t<XIN_ADDR> pina) {
  if (!server_addr || !pina) {
    return X_WSAEINVAL;
  }
  pina->s_addr = ToNetworkOrder(server_addr);
  return X_ERROR_SUCCESS;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

u32 NetDll_XNetGetBroadcastVersionStatus_entry(u32 caller, u32 reset) {
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  if (NetworkOff()) {
    return 0;  // no link, as before
  }
  return XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX;
}

u32 NetDll_XNetCreateKey_entry(u32 caller, ppc_ptr_t<XNKID> session_key,
                               ppc_ptr_t<XNKEY> exchange_key) {
  if (session_key) {
    session_key->set_u64(GenerateSessionId(XNKID_SYSTEM_LINK));
  }
  if (exchange_key) {
    GenerateIdentityExchangeKey(exchange_key);
  }
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, ppc_ptr_t<XNKID> session_key,
                                 ppc_ptr_t<XNKEY> exchange_key) {
  if (session_key && IsOnlinePeerSession(session_key->as_u64())) {
    explicit_live_key = true;
  }
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, ppc_ptr_t<XNKID> session_key) {
  explicit_live_key = false;
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  if (!pdns) {
    return X_WSAEINVAL;
  }
  const uint32_t dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
  auto* dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
  std::memset(dns, 0, sizeof(XNDNS));
  *pdns = dns_guest;

  if (NetworkOff() || !host) {
    dns->status = 1;  // non-zero = error, as before
    if (event_handle) {
      xboxkrnl::xeNtSetEvent(event_handle, nullptr);
    }
    return 0;
  }

  dns->status = X_WSAEINPROGRESS;
  auto cancelled = TrackLookup(dns_guest);
  std::thread([name = std::string(host.value()), dns, event_handle, cancelled] {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* info = nullptr;
    const int result = getaddrinfo(name.c_str(), nullptr, &hints, &info);
    // Holding the lock: the game cannot release (free) the result while we write it.
    std::unique_lock lock(lookup_mutex);
    if (cancelled->load()) {
      lock.unlock();
      if (info) {
        freeaddrinfo(info);
      }
      return;
    }
    if (result != 0 || !info) {
      REXKRNL_INFO("XNetDnsLookup: {} not found ({})", name, result);
      dns->status = result == EAI_AGAIN ? X_WSATRY_AGAIN : X_WSAHOST_NOT_FOUND;
    } else {
      uint32_t count = 0;
      for (addrinfo* i = info; i && count < 8; i = i->ai_next) {
        if (i->ai_addr && i->ai_addr->sa_family == AF_INET) {
          dns->aina[count++].s_addr = reinterpret_cast<sockaddr_in*>(i->ai_addr)->sin_addr.s_addr;
        }
      }
      dns->cina = count;
      dns->status = count ? 0 : static_cast<int32_t>(X_WSAHOST_NOT_FOUND);
    }
    lock.unlock();
    if (info) {
      freeaddrinfo(info);
    }
    if (event_handle) {
      xboxkrnl::xeNtSetEvent(event_handle, nullptr);
    }
  }).detach();
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  ReleaseLookup(dns.guest_address());
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (!pqos) {
    return X_WSAEINVAL;
  }
  auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
  auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, sizeof(XNQOS));
  *pqos = qos_guest;

  if (Live().SignedInToLive()) {
    // XNetQosServiceLookup in Xenia Canary Netplay: a good connection to Live.
    qos->info[0].probes_xmit = 8;
    qos->info[0].probes_recv = 8;
    qos->info[0].rtt_min_in_msecs = 10;
    qos->info[0].rtt_med_in_msecs = 10;
    // 4541080F and 584109B7 expect high bit/sec
    qos->info[0].up_bits_per_sec = 5u * 1024 * 1024;
    qos->info[0].down_bits_per_sec = 5u * 1024 * 1024;
    qos->info[0].flags =
        XNET_XNQOSINFO_COMPLETE | XNET_XNQOSINFO_PARTIAL_COMPLETE | XNET_XNQOSINFO_TARGET_CONTACTED;
    qos->count = 1;
  }
  qos->count_pending = 0;
  if (event_handle) {
    xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  ReleaseLookup(qos.guest_address());
  const uint32_t count = qos->count;
  for (uint32_t i = 0; i < count; i++) {
    const XNQOSINFO& info = qos->info[i];
    if (info.data_ptr && (info.flags & XNET_XNQOSINFO_DATA_RECEIVED)) {
      REX_KERNEL_MEMORY()->SystemHeapFree(info.data_ptr);
    }
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

// The host publishes a small blob for players looking at its session.
u32 NetDll_XNetQosListen_entry(u32 caller, ppc_ptr_t<XNKID> session_id, mapped_u8 data,
                               u32 data_size, u32 bits_per_second, u32 flags) {
  if (!session_id) {
    return X_ERROR_SUCCESS;
  }
  const uint64_t id = session_id->as_u64();
  if (flags & XNET_QOS_LISTEN_RELEASE) {
    Live().ForgetQos(id);
  }
  if ((flags & XNET_QOS_LISTEN_SET_DATA) && data && data_size) {
    const uint8_t* bytes = data;
    Live().PublishQos(id, std::vector<uint8_t>(bytes, bytes + data_size));
  }
  return X_ERROR_SUCCESS;
}

// Asks about remote consoles: for each session, the host's QoS blob from the server.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 num_remote_consoles, mapped_u32 remote_addresses,
                               mapped_u32 session_ids, mapped_u32 remote_keys, u32 num_gateways,
                               mapped_u32 gateways, mapped_u32 service_ids, u32 probes_count,
                               u32 bits_per_second, u32 flags, u32 event_handle,
                               mapped_u32 pqos) {
  if (!pqos) {
    return X_WSAEACCES;
  }

  std::vector<uint64_t> ids;
  if (session_ids) {
    for (uint32_t i = 0; i < num_remote_consoles; ++i) {
      const uint32_t ptr = session_ids[i];
      const auto* xnkid = REX_KERNEL_MEMORY()->TranslateVirtual<const XNKID*>(ptr);
      ids.push_back(xnkid ? xnkid->as_u64() : 0);
    }
  }

  const uint32_t count = num_remote_consoles + num_gateways;
  const uint32_t size = sizeof(XNQOS) + sizeof(XNQOSINFO) * (count ? count - 1 : 0);
  const uint32_t qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  auto* qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, size);
  // 415707D1 expects count_pending != 0 on return (asynchronous lookup).
  qos->count_pending = count;
  *pqos = qos_guest;

  auto cancelled = TrackLookup(qos_guest);
  auto api = Live().Connected() ? Live().api() : nullptr;
  auto work = [qos, count, ids, probes_count, event_handle, cancelled, api] {
    for (uint32_t i = 0; i < count && !cancelled->load(); ++i) {
      std::optional<std::vector<uint8_t>> blob;
      if (api && i < ids.size() && ids[i]) {
        blob = api->GetQos(ids[i]);
      }
      // Holding the lock: the game cannot release (free) the result while we write it.
      std::lock_guard lock(lookup_mutex);
      if (cancelled->load()) {
        return;
      }
      XNQOSINFO& info = qos->info[i];
      {
        if (blob && !blob->empty()) {
          const uint16_t length = static_cast<uint16_t>(std::min<size_t>(blob->size(), 0xFFFF));
          const uint32_t data_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(length);
          if (data_guest) {
            std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(data_guest), blob->data(),
                        length);
            info.data_ptr = data_guest;
            info.data_len = length;
            info.flags |= XNET_XNQOSINFO_DATA_RECEIVED;
          }
        }
      }
      // 415607DD and 415607D4 expect the probe counts, otherwise they keep asking.
      info.probes_xmit = static_cast<uint16_t>(probes_count);
      info.probes_recv = static_cast<uint16_t>(probes_count);
      info.rtt_min_in_msecs = 10;
      info.rtt_med_in_msecs = 10;
      info.up_bits_per_sec = 5u * 1024 * 1024;
      info.down_bits_per_sec = 5u * 1024 * 1024;
      info.flags |= XNET_XNQOSINFO_COMPLETE | XNET_XNQOSINFO_TARGET_CONTACTED;
      const uint32_t pending = qos->count_pending;
      qos->count_pending = pending ? pending - 1 : 0;
      qos->count = qos->count + 1;
    }
    if (event_handle && !cancelled->load()) {
      xboxkrnl::xeNtSetEvent(event_handle, nullptr);
    }
  };

  // 5345081A expects results at once when no probes are asked for.
  if (probes_count) {
    std::thread(work).detach();
  } else {
    work();
  }
  return X_ERROR_SUCCESS;
}

u32 NetDll_XNetQosGetListenStats_entry(u32 caller, ppc_ptr_t<XNKID> xnkid,
                                       ppc_ptr_t<XNQOSLISTENSTATS> stats) {
  if (stats) {
    stats->requests_received_count = 1;
    stats->probes_received_count = 1;
    stats->slots_full_discards_count = 1;
    stats->data_replies_sent_count = 1;
    stats->data_reply_bytes_sent = 1;
    stats->probe_replies_sent_count = 1;
  }
  return X_ERROR_SUCCESS;
}

u32 XNetLogonGetMachineID_entry(mapped_u64 machine_id_ptr) {
  if (machine_id_ptr) {
    *machine_id_ptr = Live().machine_id();
  }
  return X_STATUS_SUCCESS;
}

u32 NetDll_XnpLogonGetStatus_entry(u32 caller, mapped_void security_gateway, mapped_u32 reason) {
  if (security_gateway) {
    security_gateway.Zero(20);  // SGADDR
  }
  return X_STATUS_SUCCESS;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_XNetStartupEx, rex::kernel::xam::NetDll_XNetStartupEx_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT(__imp__NetDll_XNetGetBroadcastVersionStatus, rex::kernel::xam::NetDll_XNetGetBroadcastVersionStatus_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetGetXnAddrPlatform, rex::kernel::xam::NetDll_XNetGetXnAddrPlatform_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToServer, rex::kernel::xam::NetDll_XNetInAddrToServer_entry)
REX_EXPORT(__imp__NetDll_XNetQosGetListenStats, rex::kernel::xam::NetDll_XNetQosGetListenStats_entry)
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetServerToInAddr, rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XnpLogonGetStatus, rex::kernel::xam::NetDll_XnpLogonGetStatus_entry)
REX_EXPORT(__imp__XNetLogonGetMachineID, rex::kernel::xam::XNetLogonGetMachineID_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
REX_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT_STUB(__imp__NetDll_getpeername);
REX_EXPORT_STUB(__imp__NetDll_getsockname);
REX_EXPORT_STUB(__imp__NetDll_getsockopt);
