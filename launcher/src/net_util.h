// Small portable socket helpers for the launcher's LAN server and LAN discovery.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace netu {

#if defined(_WIN32)
using Socket = uintptr_t;
#else
using Socket = int;
#endif
extern const Socket kInvalidSocket;

// Starts the socket library once (Windows).
void Init();
void CloseSocket(Socket s);
// Last socket error as text.
std::string LastErrorText();
// Receive timeout in milliseconds (0 = none).
void SetRecvTimeout(Socket s, uint32_t ms);

// "192.168.1.20" <-> network-order address. 0 when invalid.
std::string IpToString(uint32_t net_order);
uint32_t ParseIp(const std::string& text);

struct Interface {
  std::string address;    // "192.168.1.20"
  std::string broadcast;  // "192.168.1.255"
};
// This PC's IPv4 network interfaces (no loopback), best first: the one with the default route.
std::vector<Interface> LocalInterfaces();
// The address other PCs on the network should use for this PC ("" if none found).
std::string PrimaryLocalAddress();

}  // namespace netu
