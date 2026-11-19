#include "lan_discovery.h"

#include <algorithm>

#include "net_util.h"

#if defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace lan {

namespace {

std::string Clean(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r'; }), s.end());
  return s.substr(0, 32);
}

int ToInt(const std::string& s) {
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return v;
    v = v * 10 + (c - '0');
    if (v > 100000) return v;
  }
  return v;
}

}  // namespace

std::string FormatReply(const HostInfo& info) {
  return std::string(kReplyHeader) + "name=" + Clean(info.name) + "\nport=" + std::to_string(info.port) +
         "\nplayers=" + std::to_string(info.players) + "\nmatches=" + std::to_string(info.matches) +
         "\nversion=" + Clean(info.version) + "\n";
}

std::optional<HostInfo> ParseReply(const std::string& text) {
  if (text.rfind(kReplyHeader, 0) != 0) return std::nullopt;
  HostInfo info;
  size_t pos = std::string(kReplyHeader).size();
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key == "name") info.name = Clean(value);
    else if (key == "port") info.port = static_cast<uint16_t>(ToInt(value));
    else if (key == "players") info.players = ToInt(value);
    else if (key == "matches") info.matches = ToInt(value);
    else if (key == "version") info.version = Clean(value);
  }
  if (!info.port) return std::nullopt;
  return info;
}

Scanner::~Scanner() {
  Stop();
}

void Scanner::Start(uint16_t discovery_port, std::string target) {
  if (running_.exchange(true)) return;
  discovery_port_ = discovery_port;
  target_ = std::move(target);
  stopping_ = false;
  {
    std::lock_guard lock(mutex_);
    hosts_.clear();
  }
  thread_ = std::thread([this] { Run(); });
}

void Scanner::Stop() {
  if (!running_.load()) return;
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

std::vector<HostInfo> Scanner::hosts() const {
  std::lock_guard lock(mutex_);
  std::vector<HostInfo> out = hosts_;
  std::sort(out.begin(), out.end(), [](const HostInfo& a, const HostInfo& b) { return a.name < b.name; });
  return out;
}

void Scanner::Run() {
  netu::Init();
  auto s = static_cast<netu::Socket>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (s == netu::kInvalidSocket) return;
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  netu::SetRecvTimeout(s, 200);

  auto next_query = std::chrono::steady_clock::now();
  char buffer[1024];
  while (!stopping_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_query) {
      std::vector<std::string> targets;
      if (!target_.empty()) {
        targets.push_back(target_);
      } else {
        for (const auto& i : netu::LocalInterfaces()) targets.push_back(i.broadcast);
        targets.push_back("255.255.255.255");
      }
      for (const auto& t : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(discovery_port_);
        to.sin_addr.s_addr = netu::ParseIp(t);
        if (!to.sin_addr.s_addr) continue;
        sendto(s, kQuery, static_cast<int>(std::char_traits<char>::length(kQuery)), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to));
      }
      next_query = now + std::chrono::seconds(1);
      // Forget hosts that stopped answering.
      std::lock_guard lock(mutex_);
      hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(),
                                  [&](const HostInfo& h) { return now - h.seen > std::chrono::seconds(5); }),
                   hosts_.end());
    }

    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int got = static_cast<int>(
        recvfrom(s, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len));
    if (got <= 0) continue;
    auto info = ParseReply(std::string(buffer, static_cast<size_t>(got)));
    if (!info) continue;
    info->address = netu::IpToString(from.sin_addr.s_addr);
    info->seen = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    bool found = false;
    for (auto& h : hosts_) {
      if (h.address == info->address && h.port == info->port) {
        h = *info;
        found = true;
      }
    }
    if (!found) hosts_.push_back(*info);
  }
  netu::CloseSocket(s);
}

}  // namespace lan
