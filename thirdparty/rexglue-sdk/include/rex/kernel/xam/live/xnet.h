/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/xam/live/xnet.h
 * @brief       Xbox 360 network structures and helpers (XNADDR, XNKID, QoS, ...)
 *
 * From Xenia Canary Netplay's src/xenia/kernel/xnet.h (netplay_canary_experimental,
 * 6342e12), reduced to what the recomp uses. All structures are in guest
 * (big-endian) layout; in_addr fields hold network byte order, as on the console.
 *
 * @modified    2026, Gundam Operation Troy recomp - adapted for ReXGlue
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <random>

#include <rex/types.h>

namespace rex::kernel::xam {

// An IPv4 address in network byte order (same layout as in_addr).
struct XIN_ADDR {
  uint32_t addr;  // not "s_addr": Windows headers #define s_addr
};
static_assert(sizeof(XIN_ADDR) == 4);

// Xbox network address of a console.
struct XNADDR {
  XIN_ADDR ina;                   // local IP (on Live: the public IP, like Xenia Netplay)
  XIN_ADDR inaOnline;             // public IP (zero when not online)
  rex::be<uint16_t> wPortOnline;  // public port
  uint8_t abEnet[6];              // MAC address
  // SGADDR (security gateway address), 20 bytes:
  //   +0 ina, +4 security parameter index, +8 xbox id, +16 platform type, +17 reserved[3]
  uint8_t abOnline[20];
};
static_assert(sizeof(XNADDR) == 0x24);

inline constexpr size_t kXnAddrPlatformTypeOffset = 16;  // within abOnline
enum XNADDR_PLATFORM_TYPE : uint8_t { kPlatformXbox1 = 0, kPlatformXbox360 = 1, kPlatformPC = 2 };

// Session key ID: top byte tells the kind of session.
struct XNKID {
  uint8_t ab[8];
  uint64_t as_u64() const {  // the big-endian value as a number
    uint64_t v = 0;
    for (const uint8_t b : ab) {
      v = (v << 8) | b;
    }
    return v;
  }
  void set_u64(uint64_t v) {
    for (int i = 7; i >= 0; --i) {
      ab[i] = static_cast<uint8_t>(v & 0xFF);
      v >>= 8;
    }
  }
};
static_assert(sizeof(XNKID) == 8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert(sizeof(XNKEY) == 16);

struct XNDNS {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  XIN_ADDR aina[8];
};

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
};
static_assert(sizeof(XNQOSINFO) == 0x18);

struct XNQOS {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
};
static_assert(sizeof(XNQOS) == 0x20);

struct XNQOSLISTENSTATS {
  rex::be<uint32_t> size_of_struct;
  rex::be<uint32_t> requests_received_count;
  rex::be<uint32_t> probes_received_count;
  rex::be<uint32_t> slots_full_discards_count;
  rex::be<uint32_t> data_replies_sent_count;
  rex::be<uint32_t> data_reply_bytes_sent;
  rex::be<uint32_t> probe_replies_sent_count;
};

// XNetGetTitleXnAddr status bits
inline constexpr uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
inline constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
inline constexpr uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
inline constexpr uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
inline constexpr uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
inline constexpr uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
inline constexpr uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
inline constexpr uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
inline constexpr uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
inline constexpr uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;

// XNetGetEthernetLinkStatus bits
inline constexpr uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
inline constexpr uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
inline constexpr uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
inline constexpr uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
inline constexpr uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;

// XNQOSINFO flags
inline constexpr uint8_t XNET_XNQOSINFO_COMPLETE = 0x01;
inline constexpr uint8_t XNET_XNQOSINFO_TARGET_CONTACTED = 0x02;
inline constexpr uint8_t XNET_XNQOSINFO_TARGET_DISABLED = 0x04;
inline constexpr uint8_t XNET_XNQOSINFO_DATA_RECEIVED = 0x08;
inline constexpr uint8_t XNET_XNQOSINFO_PARTIAL_COMPLETE = 0x10;

// XNetQosListen flags
inline constexpr uint32_t XNET_QOS_LISTEN_ENABLE = 0x01;
inline constexpr uint32_t XNET_QOS_LISTEN_DISABLE = 0x02;
inline constexpr uint32_t XNET_QOS_LISTEN_SET_DATA = 0x04;
inline constexpr uint32_t XNET_QOS_LISTEN_SET_BITSPERSEC = 0x08;
inline constexpr uint32_t XNET_QOS_LISTEN_RELEASE = 0x10;

// XNetGetConnectStatus results
inline constexpr uint32_t XNET_CONNECT_STATUS_IDLE = 0;
inline constexpr uint32_t XNET_CONNECT_STATUS_PENDING = 1;
inline constexpr uint32_t XNET_CONNECT_STATUS_CONNECTED = 2;
inline constexpr uint32_t XNET_CONNECT_STATUS_LOST = 3;

// Winsock errors as the console reports them
inline constexpr uint32_t X_WSAEACCES = 10013;
inline constexpr uint32_t X_WSAEFAULT = 10014;
inline constexpr uint32_t X_WSAEINVAL = 10022;
inline constexpr uint32_t X_WSAEINPROGRESS = 10036;
inline constexpr uint32_t X_WSAEMSGSIZE = 10040;
inline constexpr uint32_t X_WSAEADDRINUSE = 10048;
inline constexpr uint32_t X_WSANOTINITIALISED = 10093;
inline constexpr uint32_t X_WSAHOST_NOT_FOUND = 11001;
inline constexpr uint32_t X_WSATRY_AGAIN = 11002;
inline constexpr uint32_t X_WSANO_DATA = 11004;

// IPv4 values as numbers (host order), as XNet functions receive them.
inline constexpr uint32_t kIpLoopback = 0x7F000001;
inline constexpr uint32_t kIpBroadcast = 0xFFFFFFFF;

// Session ID kinds (top byte of an XNKID)
inline constexpr uint8_t XNKID_MASK = 0xF0;
inline constexpr uint8_t XNKID_SYSTEM_LINK = 0x00;
inline constexpr uint8_t XNKID_SYSTEM_LINK_XPLAT = 0x40;
inline constexpr uint8_t XNKID_ONLINE_PEER = 0x80;
inline constexpr uint8_t XNKID_ONLINE_NAT_OPEN = 0x90;
inline constexpr uint8_t XNKID_ONLINE_NAT_MODERATE = 0xA0;
inline constexpr uint8_t XNKID_ONLINE_NAT_STRICT = 0xB0;
inline constexpr uint8_t XNKID_ONLINE_SERVER = 0xC0;
inline constexpr uint8_t XNKID_ONLINE_TITLESERVER = 0xE0;

inline bool IsSystemlinkSession(uint64_t session_id) {
  return ((session_id >> 56) & XNKID_MASK) == XNKID_SYSTEM_LINK;
}
inline bool IsOnlinePeerSession(uint64_t session_id) {
  return ((session_id >> 56) & XNKID_MASK) == XNKID_ONLINE_PEER;
}
inline bool IsServerSession(uint64_t session_id) {
  return ((session_id >> 56) & XNKID_MASK) == XNKID_ONLINE_SERVER;
}
inline bool IsValidSessionId(uint64_t session_id) {
  return session_id != 0 && (IsOnlinePeerSession(session_id) ||
                             IsSystemlinkSession(session_id) || IsServerSession(session_id));
}

// A new random session ID of the given kind (top byte = kind).
inline uint64_t GenerateSessionId(uint8_t kind) {
  std::random_device rnd;
  const uint64_t r = (static_cast<uint64_t>(rnd()) << 32) | rnd();
  return (static_cast<uint64_t>(kind) << 56) | (r & 0x00FFFFFFFFFFFFFFull);
}

// Xenia's identity exchange key: bytes 0..15.
inline void GenerateIdentityExchangeKey(XNKEY* key) {
  for (uint8_t i = 0; i < sizeof(XNKEY); i++) {
    key->ab[i] = i;
  }
}

}  // namespace rex::kernel::xam
