/**
 * @file        system/xsocket_native.h
 * @brief       Xbox 360 socket calls on the PC's own sockets (Winsock / POSIX)
 *
 * The game talks Xbox Winsock: big-endian option values, Xbox option and
 * ioctl numbers, Winsock error codes. These functions do the translation, so
 * XSocket and the NetDll exports stay simple, and so the translation can be
 * tested without the game (tools/netcheck "sockets" test).
 *
 * Follows Xenia Canary Netplay's xsocket.cc (AdrianCassar/xenia-canary).
 *
 * Every function returns -1 on failure (like Winsock) and then LastError()
 * gives the Xbox (= Winsock) error code for the calling thread.
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>

namespace rex::system::sock {

using Handle = uint64_t;
inline constexpr Handle kInvalidHandle = ~Handle(0);

// Xbox Winsock error codes (same numbers as Windows).
inline constexpr uint32_t kWsaEINTR = 10004;
inline constexpr uint32_t kWsaEFAULT = 10014;
inline constexpr uint32_t kWsaEINVAL = 10022;
inline constexpr uint32_t kWsaEWOULDBLOCK = 10035;
inline constexpr uint32_t kWsaEINPROGRESS = 10036;
inline constexpr uint32_t kWsaENOTSOCK = 10038;
inline constexpr uint32_t kWsaEMSGSIZE = 10040;
inline constexpr uint32_t kWsaENOPROTOOPT = 10042;
inline constexpr uint32_t kWsaEADDRINUSE = 10048;
inline constexpr uint32_t kWsaEADDRNOTAVAIL = 10049;
inline constexpr uint32_t kWsaECONNRESET = 10054;
inline constexpr uint32_t kWsaENOTCONN = 10057;

// Xbox socket numbers.
inline constexpr uint32_t kXSolSocket = 0xFFFF;
inline constexpr uint32_t kXIpprotoTcp = 6;
inline constexpr uint32_t kXSoReuseAddr = 0x0004;
inline constexpr uint32_t kXSoBroadcast = 0x0020;
inline constexpr uint32_t kXSoLinger = 0x0080;
inline constexpr uint32_t kXSoMarkInsecure = 0x5801;
inline constexpr uint32_t kXSoPrivate = 0x5802;
inline constexpr uint32_t kXSoGrantInsecure = 0x5803;
inline constexpr uint32_t kXFionbio = 0x8004667E;
inline constexpr uint32_t kXFionread = 0x4004667F;
inline constexpr uint32_t kXSockStream = 1;
inline constexpr uint32_t kXSockDgram = 2;

// An IPv4 address and port as numbers (192.168.1.2 = 0xC0A80102), i.e. the
// value of the game's big-endian sin_addr / sin_port fields.
struct Endpoint {
  uint32_t ip = 0;
  uint16_t port = 0;
};

// Error code of the last failed call on this thread.
uint32_t LastError();
// Sets it (for errors found before calling the PC's sockets).
void SetError(uint32_t code);
// Winsock code for a POSIX errno value (used on Linux / macOS).
uint32_t ErrnoToWsa(int err);

// Starts the PC's socket library once (Windows); harmless elsewhere.
void EnsureStarted();

Handle Open(uint32_t af, uint32_t type, uint32_t protocol);
int Close(Handle h);

int Bind(Handle h, Endpoint local);
int Connect(Handle h, Endpoint remote);
int Listen(Handle h, int backlog);
Handle Accept(Handle h, Endpoint* remote);
int Shutdown(Handle h, int how);
int GetSockName(Handle h, Endpoint* out);
int GetPeerName(Handle h, Endpoint* out);

int Send(Handle h, const void* buf, uint32_t len, uint32_t flags);
int Recv(Handle h, void* buf, uint32_t len, uint32_t flags);
// to == nullptr: send on a connected socket.
int SendTo(Handle h, const void* buf, uint32_t len, uint32_t flags, const Endpoint* to);
// from may be nullptr.
int RecvFrom(Handle h, void* buf, uint32_t len, uint32_t flags, Endpoint* from);

// guest_value points to the game's big-endian option value.
int SetOption(Handle h, uint32_t x_level, uint32_t x_optname, const uint8_t* guest_value,
              uint32_t len);
// Writes a big-endian value; *len in: buffer size, out: bytes written.
int GetOption(Handle h, uint32_t x_level, uint32_t x_optname, uint8_t* guest_value,
              uint32_t* len);
// FIONBIO / FIONREAD; guest_arg is the game's big-endian u32.
int IoControl(Handle h, uint32_t x_cmd, uint8_t* guest_arg);

// The native descriptor number, for select() on POSIX (its nfds argument).
int NativeFd(Handle h);

}  // namespace rex::system::sock
