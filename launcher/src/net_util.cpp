#include "net_util.h"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace netu {

#if defined(_WIN32)
const Socket kInvalidSocket = static_cast<Socket>(INVALID_SOCKET);
#else
const Socket kInvalidSocket = -1;
#endif

void Init() {
#if defined(_WIN32)
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

void CloseSocket(Socket s) {
  if (s == kInvalidSocket) return;
#if defined(_WIN32)
  closesocket(static_cast<SOCKET>(s));
#else
  close(s);
#endif
}

std::string LastErrorText() {
#if defined(_WIN32)
  return "socket error " + std::to_string(WSAGetLastError());
#else
  return std::strerror(errno);
#endif
}

void SetRecvTimeout(Socket s, uint32_t ms) {
#if defined(_WIN32)
  DWORD value = ms;
  setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value),
             sizeof(value));
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

std::string IpToString(uint32_t net_order) {
  const auto* b = reinterpret_cast<const uint8_t*>(&net_order);
  return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
         std::to_string(b[3]);
}

uint32_t ParseIp(const std::string& text) {
  uint8_t bytes[4] = {};
  int part = 0;
  int value = -1;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : '.';
    if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
      if (value > 255) return 0;
    } else if (c == '.') {
      if (value < 0 || part > 3) return 0;
      bytes[part++] = static_cast<uint8_t>(value);
      value = -1;
    } else {
      return 0;
    }
  }
  if (part != 4) return 0;
  uint32_t out = 0;
  std::memcpy(&out, bytes, 4);
  return out;
}

namespace {

// The local address of the default route (no packet is sent).
uint32_t DefaultRouteAddress() {
  Init();
#if defined(_WIN32)
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return 0;
#else
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return 0;
#endif
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(53);
  target.sin_addr.s_addr = ParseIp("1.1.1.1");
  uint32_t local = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in name{};
    socklen_t len = sizeof(name);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&name), &len) == 0) local = name.sin_addr.s_addr;
  }
  CloseSocket(static_cast<Socket>(s));
  return local;
}

bool IsUsable(uint32_t net_order) {
  const auto* b = reinterpret_cast<const uint8_t*>(&net_order);
  return net_order != 0 && b[0] != 127 && !(b[0] == 169 && b[1] == 254);
}

}  // namespace

std::vector<Interface> LocalInterfaces() {
  Init();
  std::vector<Interface> out;
  auto add = [&](uint32_t addr, uint32_t mask) {
    if (!IsUsable(addr)) return;
    const uint32_t bcast = mask ? (addr | ~mask) : 0xFFFFFFFFu;
    for (const auto& i : out)
      if (i.address == IpToString(addr)) return;
    out.push_back({IpToString(addr), IpToString(bcast)});
  };
#if defined(_WIN32)
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer(size);
  auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
  }
  if (result == NO_ERROR) {
    for (auto* a = list; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
        const uint32_t addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
        const UINT8 bits = u->OnLinkPrefixLength;
        const uint32_t mask = bits == 0 ? 0 : htonl(bits >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1));
        add(addr, mask);
      }
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (auto* a = list; a; a = a->ifa_next) {
      if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP)) continue;
      const uint32_t addr = reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr;
      const uint32_t mask = a->ifa_netmask ? reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr : 0;
      add(addr, mask);
    }
    freeifaddrs(list);
  }
#endif
  // Put the default-route interface first.
  const std::string primary = IpToString(DefaultRouteAddress());
  for (size_t i = 1; i < out.size(); ++i) {
    if (out[i].address == primary) {
      std::swap(out[0], out[i]);
      break;
    }
  }
  return out;
}

std::string PrimaryLocalAddress() {
  const uint32_t route = DefaultRouteAddress();
  if (IsUsable(route)) return IpToString(route);
  auto list = LocalInterfaces();
  return list.empty() ? std::string() : list.front().address;
}

}  // namespace netu
