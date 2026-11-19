/**
 * @file        kernel/xam/live/live_upnp.cpp
 * @brief       Opens the game's ports on the home router (see live_upnp.h)
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#include <rex/kernel/xam/live/live_upnp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include <fmt/format.h>

#include <rex/kernel/xam/live/live_identity.h>
#include <rex/net/http.h>
#include <rex/system/xsocket_native.h>

namespace rex::kernel::xam::live {

namespace sock = rex::system::sock;
using namespace std::chrono_literals;

namespace {

constexpr auto kRediscoverAfter = std::chrono::minutes(5);

const char* const kServiceTypes[] = {
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
};

const char* const kSearchTargets[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
};

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string Trim(std::string_view s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
    ++a;
  }
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
    --b;
  }
  return std::string(s.substr(a, b - a));
}

// The text of the first <tag>...</tag> in xml from pos (namespace prefixes allowed).
std::string TagText(const std::string& xml, const std::string& tag, size_t from = 0,
                    size_t to = std::string::npos) {
  const std::string lower = Lower(xml.substr(from, to == std::string::npos ? to : to - from));
  const std::string want = Lower(tag);
  size_t pos = 0;
  while ((pos = lower.find('<', pos)) != std::string::npos) {
    size_t end = lower.find('>', pos);
    if (end == std::string::npos) {
      return {};
    }
    std::string name = lower.substr(pos + 1, end - pos - 1);
    if (auto space = name.find_first_of(" \t\r\n/"); space != std::string::npos) {
      name = name.substr(0, space);
    }
    if (auto colon = name.find(':'); colon != std::string::npos) {
      name = name.substr(colon + 1);
    }
    if (name == want && lower[end - 1] != '/') {
      size_t close = lower.find("</", end);
      while (close != std::string::npos) {
        size_t close_end = lower.find('>', close);
        std::string close_name = lower.substr(close + 2, close_end - close - 2);
        if (auto colon = close_name.find(':'); colon != std::string::npos) {
          close_name = close_name.substr(colon + 1);
        }
        if (Trim(close_name) == want) {
          return Trim(std::string_view(xml).substr(from + end + 1, close - end - 1));
        }
        close = lower.find("</", close + 2);
      }
      return {};
    }
    pos = end + 1;
  }
  return {};
}

std::string XmlEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

// "http://host:port/path" -> "http://host:port"
std::string UrlOrigin(const std::string& url) {
  size_t scheme = url.find("://");
  size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  size_t slash = url.find('/', start);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

// "http://host:port/path" -> "host"
std::string UrlHost(const std::string& url) {
  std::string origin = UrlOrigin(url);
  size_t scheme = origin.find("://");
  std::string host = scheme == std::string::npos ? origin : origin.substr(scheme + 3);
  if (auto colon = host.rfind(':'); colon != std::string::npos) {
    host = host.substr(0, colon);
  }
  return host;
}

std::string ResolveUrl(const std::string& base, const std::string& url) {
  if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
    return url;
  }
  std::string origin = UrlOrigin(base);
  if (!url.empty() && url[0] == '/') {
    return origin + url;
  }
  // Relative to the base's directory.
  size_t scheme = base.find("://");
  size_t path_start = base.find('/', scheme == std::string::npos ? 0 : scheme + 3);
  if (path_start == std::string::npos) {
    return origin + "/" + url;
  }
  size_t last_slash = base.rfind('/');
  return base.substr(0, last_slash + 1) + url;
}

// Network-order address (in_addr.s_addr) -> number (192.168.1.2 = 0xC0A80102).
uint32_t NetOrderToNumber(uint32_t net) {
  const auto* b = reinterpret_cast<const uint8_t*>(&net);
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}

}  // namespace

Upnp::Upnp(UpnpConfig config, LogSink log) : config_(std::move(config)), log_(std::move(log)) {}

Upnp::~Upnp() {
  Shutdown();
}

void Upnp::Log(LogLevel level, const std::string& message) const {
  if (log_) {
    log_(level, "UPnP: " + message);
  }
}

void Upnp::OpenAsync(uint16_t port, const std::string& protocol) {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return;
  }
  jobs_.push_back({true, port, protocol});
  if (!started_) {
    started_ = true;
    thread_ = std::thread([this] { Run(); });
  }
  wake_.notify_all();
}

void Upnp::CloseAsync(uint16_t port, const std::string& protocol) {
  std::lock_guard lock(mutex_);
  if (stopping_ || !started_) {
    return;
  }
  jobs_.push_back({false, port, protocol});
  wake_.notify_all();
}

void Upnp::Run() {
  auto next_renew = std::chrono::steady_clock::now() +
                    std::chrono::seconds(std::max<uint32_t>(config_.lease_seconds / 2, 60));
  while (true) {
    Job job;
    bool have_job = false;
    bool renew = false;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_until(lock, next_renew, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) {
        return;
      }
      if (!jobs_.empty()) {
        job = jobs_.front();
        jobs_.pop_front();
        have_job = true;
      } else if (std::chrono::steady_clock::now() >= next_renew) {
        renew = true;
      }
    }

    if (have_job) {
      auto key = std::make_pair(job.protocol, job.port);
      if (job.open) {
        bool first = false;
        {
          std::lock_guard lock(mutex_);
          first = mapped_.find(key) == mapped_.end();
        }
        if (!first) {
          std::lock_guard lock(mutex_);
          ++mapped_[key];
          continue;
        }
        int result = AddMapping(job.port, job.protocol);
        if (result == 0) {
          std::lock_guard lock(mutex_);
          ++mapped_[key];
        }
      } else {
        bool last = false;
        {
          std::lock_guard lock(mutex_);
          auto it = mapped_.find(key);
          if (it != mapped_.end() && --it->second <= 0) {
            mapped_.erase(it);
            last = true;
          }
        }
        if (last) {
          DeleteMapping(job.port, job.protocol);
        }
      }
    }
    if (renew) {
      RenewAll();
      next_renew = std::chrono::steady_clock::now() +
                   std::chrono::seconds(std::max<uint32_t>(config_.lease_seconds / 2, 60));
    }
  }
}

void Upnp::RenewAll() {
  std::vector<std::pair<std::string, uint16_t>> ports = mapped();
  bool permanent = false;
  {
    std::lock_guard lock(mutex_);
    permanent = permanent_only_;
  }
  if (permanent) {
    return;  // permanent mappings need no renewal
  }
  for (const auto& [protocol, port] : ports) {
    AddMappingLease(port, protocol, config_.lease_seconds);
  }
}

void Upnp::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  for (const auto& [protocol, port] : mapped()) {
    DeleteMapping(port, protocol);
  }
  std::lock_guard lock(mutex_);
  mapped_.clear();
}

std::string Upnp::control_url() const {
  std::lock_guard lock(mutex_);
  return service_.control_url;
}

std::string Upnp::service_type() const {
  std::lock_guard lock(mutex_);
  return service_.type;
}

std::string Upnp::local_ip() const {
  std::lock_guard lock(mutex_);
  return local_ip_;
}

std::vector<std::pair<std::string, uint16_t>> Upnp::mapped() const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<std::string, uint16_t>> out;
  for (const auto& [key, count] : mapped_) {
    out.push_back(key);
  }
  return out;
}

std::string Upnp::LocationFromSsdp(const std::string& response) {
  size_t pos = 0;
  while (pos < response.size()) {
    size_t end = response.find('\n', pos);
    if (end == std::string::npos) {
      end = response.size();
    }
    std::string line = response.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = line.find(':');
    if (colon != std::string::npos && Lower(Trim(line.substr(0, colon))) == "location") {
      return Trim(line.substr(colon + 1));
    }
  }
  return {};
}

Upnp::Service Upnp::FindService(const std::string& xml, const std::string& location) {
  std::string base = TagText(xml, "URLBase");
  if (base.empty()) {
    base = location;
  }
  const std::string lower = Lower(xml);
  for (const char* wanted : kServiceTypes) {
    size_t pos = 0;
    while ((pos = lower.find("<service>", pos)) != std::string::npos) {
      size_t end = lower.find("</service>", pos);
      if (end == std::string::npos) {
        break;
      }
      std::string type = TagText(xml, "serviceType", pos, end);
      if (type == wanted) {
        std::string control = TagText(xml, "controlURL", pos, end);
        if (!control.empty()) {
          return Service{type, ResolveUrl(base, control)};
        }
      }
      pos = end;
    }
  }
  return {};
}

std::vector<std::string> Upnp::SsdpSearch() {
  std::vector<std::string> locations;
  sock::Handle s = sock::Open(2, sock::kXSockDgram, 17);
  if (s == sock::kInvalidHandle) {
    Log(LogLevel::kWarning, fmt::format("can't open a UDP socket (error {})", sock::LastError()));
    return locations;
  }
  uint8_t non_blocking[4] = {0, 0, 0, 1};
  sock::IoControl(s, sock::kXFionbio, non_blocking);

  sock::Endpoint target{NetOrderToNumber(ParseIpv4(config_.ssdp_address)), config_.ssdp_port};
  for (const char* st : kSearchTargets) {
    std::string request = fmt::format(
        "M-SEARCH * HTTP/1.1\r\nHOST: {}:{}\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: {}\r\n\r\n",
        config_.ssdp_address, config_.ssdp_port, st);
    sock::SendTo(s, request.data(), static_cast<uint32_t>(request.size()), 0, &target);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.discover_ms);
  char buffer[2048];
  while (std::chrono::steady_clock::now() < deadline) {
    int got = sock::RecvFrom(s, buffer, sizeof(buffer) - 1, 0, nullptr);
    if (got <= 0) {
      std::this_thread::sleep_for(20ms);
      continue;
    }
    std::string location = LocationFromSsdp(std::string(buffer, static_cast<size_t>(got)));
    if (!location.empty() &&
        std::find(locations.begin(), locations.end(), location) == locations.end()) {
      locations.push_back(location);
    }
  }
  sock::Close(s);
  return locations;
}

bool Upnp::TryLocation(const std::string& location) {
  rex::net::HttpRequest request;
  request.url = location;
  request.timeout_ms = config_.http_timeout_ms;
  request.connect_timeout_ms = config_.http_timeout_ms;
  request.max_response_bytes = 256 * 1024;
  rex::net::HttpResponse response = rex::net::HttpSend(request);
  if (!response.success()) {
    Log(LogLevel::kDebug, fmt::format("{}: no description ({})", location,
                                      response.error.empty() ? fmt::format("HTTP {}", response.status)
                                                             : response.error));
    return false;
  }
  Service service = FindService(response.body, location);
  if (service.type.empty()) {
    Log(LogLevel::kDebug, fmt::format("{}: not a router with port forwarding", location));
    return false;
  }
  uint32_t local = DetectLocalIp(UrlHost(service.control_url));
  std::lock_guard lock(mutex_);
  service_ = service;
  local_ip_ = local ? Ipv4ToString(local) : std::string();
  return !local_ip_.empty();
}

bool Upnp::Discover() {
  {
    std::lock_guard lock(mutex_);
    if (discovered_) {
      return true;
    }
    auto now = std::chrono::steady_clock::now();
    if (discover_failed_ && now - last_discover_attempt_ < kRediscoverAfter) {
      return false;
    }
    last_discover_attempt_ = now;
  }

  std::vector<std::string> locations;
  if (!config_.root_url.empty()) {
    locations.push_back(config_.root_url);
  } else {
    locations = SsdpSearch();
  }
  for (const std::string& location : locations) {
    if (TryLocation(location)) {
      std::string type;
      std::string control;
      std::string local;
      {
        std::lock_guard lock(mutex_);
        discovered_ = true;
        discover_failed_ = false;
        type = service_.type;
        control = service_.control_url;
        local = local_ip_;
      }
      Log(LogLevel::kInfo,
          fmt::format("router found ({}), this PC is {}", type.substr(type.rfind(':', type.size() - 3) + 1), local));
      Log(LogLevel::kDebug, fmt::format("control URL {}", control));
      return true;
    }
  }
  {
    std::lock_guard lock(mutex_);
    discover_failed_ = true;
  }
  Log(LogLevel::kWarning,
      locations.empty()
          ? "no router answered. Turn on UPnP in the router's settings, or forward the "
            "game's ports by hand (see the log lines \"Socket ... bound to port\")."
          : "the router doesn't offer port forwarding through UPnP.");
  return false;
}

int Upnp::Soap(const std::string& action, const std::string& arguments, std::string* out_body) {
  std::string type;
  std::string control;
  {
    std::lock_guard lock(mutex_);
    type = service_.type;
    control = service_.control_url;
  }
  rex::net::HttpRequest request;
  request.method = "POST";
  request.url = control;
  request.timeout_ms = config_.http_timeout_ms;
  request.connect_timeout_ms = config_.http_timeout_ms;
  request.max_response_bytes = 64 * 1024;
  request.headers = {{"Content-Type", "text/xml; charset=\"utf-8\""},
                     {"SOAPAction", fmt::format("\"{}#{}\"", type, action)}};
  request.body = fmt::format(
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:{0} xmlns:u=\"{1}\">{2}</u:{0}></s:Body></s:Envelope>\r\n",
      action, type, arguments);
  rex::net::HttpResponse response = rex::net::HttpSend(request);
  if (out_body) {
    *out_body = response.body;
  }
  if (!response.answered()) {
    Log(LogLevel::kWarning, fmt::format("{}: the router didn't answer ({})", action, response.error));
    return -1;
  }
  if (response.success()) {
    return 0;
  }
  std::string code = TagText(response.body, "errorCode");
  int error = code.empty() ? response.status : std::atoi(code.c_str());
  return error ? error : -1;
}

int Upnp::AddMappingLease(uint16_t port, const std::string& protocol, uint32_t lease) {
  std::string local = local_ip();
  std::string arguments = fmt::format(
      "<NewRemoteHost></NewRemoteHost><NewExternalPort>{0}</NewExternalPort>"
      "<NewProtocol>{1}</NewProtocol><NewInternalPort>{0}</NewInternalPort>"
      "<NewInternalClient>{2}</NewInternalClient><NewEnabled>1</NewEnabled>"
      "<NewPortMappingDescription>{3}</NewPortMappingDescription>"
      "<NewLeaseDuration>{4}</NewLeaseDuration>",
      port, protocol, local, XmlEscape(config_.description), lease);
  return Soap("AddPortMapping", arguments, nullptr);
}

int Upnp::AddMapping(uint16_t port, const std::string& protocol) {
  if (!Discover()) {
    return -1;
  }
  bool permanent = false;
  {
    std::lock_guard lock(mutex_);
    permanent = permanent_only_;
  }
  int result = AddMappingLease(port, protocol, permanent ? 0 : config_.lease_seconds);
  if (result == kUpnpOnlyPermanentLeases && !permanent) {
    {
      std::lock_guard lock(mutex_);
      permanent_only_ = true;
    }
    result = AddMappingLease(port, protocol, 0);
  }
  if (result == 0) {
    Log(LogLevel::kInfo, fmt::format("opened {} port {} on the router", protocol, port));
  } else if (result == kUpnpConflict) {
    Log(LogLevel::kWarning,
        fmt::format("{} port {} is already forwarded to another device on your network "
                    "(another PC or console running the game?)",
                    protocol, port));
  } else if (result > 0) {
    Log(LogLevel::kWarning,
        fmt::format("the router refused to open {} port {} (UPnP error {})", protocol, port,
                    result));
  }
  return result;
}

int Upnp::DeleteMapping(uint16_t port, const std::string& protocol) {
  {
    std::lock_guard lock(mutex_);
    if (!discovered_) {
      return -1;
    }
  }
  int result = Soap("DeletePortMapping",
                    fmt::format("<NewRemoteHost></NewRemoteHost><NewExternalPort>{}</NewExternalPort>"
                                "<NewProtocol>{}</NewProtocol>",
                                port, protocol),
                    nullptr);
  if (result == 0) {
    Log(LogLevel::kInfo, fmt::format("closed {} port {} on the router", protocol, port));
  }
  return result;
}

std::string Upnp::ExternalIp() {
  if (!Discover()) {
    return {};
  }
  std::string body;
  if (Soap("GetExternalIPAddress", "", &body) != 0) {
    return {};
  }
  return TagText(body, "NewExternalIPAddress");
}

}  // namespace rex::kernel::xam::live
