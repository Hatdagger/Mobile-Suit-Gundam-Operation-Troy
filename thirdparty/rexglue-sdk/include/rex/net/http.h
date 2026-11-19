/**
 * @file        net/http.h
 * @brief       Small blocking HTTP/HTTPS client
 *
 * One function, HttpSend(), for the few REST calls the Xbox Live replacement
 * makes (see rex/kernel/xam/live/live_api.h). Not for bulk downloads.
 *
 * Backends:
 *  - Windows: WinHTTP (part of Windows; HTTPS uses the system certificates).
 *  - Linux / macOS: the system libcurl, loaded when first used
 *    (libcurl.so.4 / libcurl.4.dylib), so building needs no curl headers.
 *    If no libcurl is found, HttpAvailable() is false and requests fail.
 *
 * Safe to call from several threads at once.
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::net {

struct HttpRequest {
  // "GET", "POST", "DELETE", "HEAD", ...
  std::string method = "GET";
  // Full URL, http:// or https://, e.g. "https://example.com/title/424107D6/sessions".
  std::string url;
  // Extra request headers, e.g. {"Content-Type", "application/json"}.
  std::vector<std::pair<std::string, std::string>> headers;
  // Request body (may hold binary data).
  std::string body;
  // Limit for the whole request, and for finding and connecting to the server.
  uint32_t timeout_ms = 15000;
  uint32_t connect_timeout_ms = 5000;
  // Largest response body accepted; a bigger one fails the request.
  size_t max_response_bytes = 16 * 1024 * 1024;
};

struct HttpResponse {
  // HTTP status code (200, 201, 404, ...). 0 when no response arrived.
  int status = 0;
  // Response body (may hold binary data).
  std::string body;
  // Empty on success. Otherwise why no response arrived (in plain words).
  std::string error;

  // True when the server answered (any status code).
  bool answered() const { return error.empty() && status != 0; }
  // True when the server answered with a 2xx status code.
  bool success() const { return answered() && status >= 200 && status < 300; }
};

// Sends one request and waits for the whole response.
HttpResponse HttpSend(const HttpRequest& request);

// False when this build or system has no HTTP backend (for example no libcurl on Linux).
bool HttpAvailable();

// "WinHTTP", "libcurl 8.5.0", or "none".
std::string HttpBackendName();

// The User-Agent sent with every request.
inline constexpr std::string_view kHttpUserAgent = "xenia-rexglue";

}  // namespace rex::net
