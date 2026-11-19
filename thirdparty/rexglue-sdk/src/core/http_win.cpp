/**
 * @file        core/http_win.cpp
 * @brief       rex::net::HttpSend on Windows, using WinHTTP
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */

#if !defined(_WIN32)
#error "This file is Windows-only"
#endif

#include <rex/net/http.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace rex::net {
namespace {

std::wstring Widen(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                      length);
  return result;
}

std::string DescribeError(const char* step, DWORD code) {
  const char* meaning = nullptr;
  switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
      meaning = "the server took too long to answer";
      break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
      meaning = "the server name was not found";
      break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
      meaning = "could not connect to the server";
      break;
    case ERROR_WINHTTP_CONNECTION_ERROR:
      meaning = "the connection was closed";
      break;
    case ERROR_WINHTTP_SECURE_FAILURE:
      meaning = "the secure (HTTPS) connection failed";
      break;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
      meaning = "the server address is not valid";
      break;
    default:
      break;
  }
  std::string text = std::string(step) + " failed (error " + std::to_string(code) + ")";
  if (meaning) {
    text += ": ";
    text += meaning;
  }
  return text;
}

struct InternetHandle {
  HINTERNET handle = nullptr;
  InternetHandle() = default;
  explicit InternetHandle(HINTERNET h) : handle(h) {}
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  ~InternetHandle() {
    if (handle) {
      WinHttpCloseHandle(handle);
    }
  }
  explicit operator bool() const { return handle != nullptr; }
};

// One WinHTTP session for the whole process; WinHTTP handles are thread-safe.
HINTERNET SharedSession(DWORD* error_out) {
  static std::once_flag once;
  static HINTERNET session = nullptr;
  static DWORD open_error = 0;
  std::call_once(once, [] {
    const std::wstring agent = Widen(kHttpUserAgent);
    // Automatic proxy needs Windows 8.1 or newer; fall back to the system default.
    session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
      session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
      open_error = GetLastError();
      return;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                          sizeof(protocols))) {
      // Older Windows 10 builds do not know TLS 1.3.
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
  });
  if (error_out) {
    *error_out = open_error;
  }
  return session;
}

}  // namespace

bool HttpAvailable() {
  return SharedSession(nullptr) != nullptr;
}

std::string HttpBackendName() {
  return HttpAvailable() ? "WinHTTP" : "none";
}

HttpResponse HttpSend(const HttpRequest& request) {
  HttpResponse response;

  DWORD open_error = 0;
  HINTERNET session = SharedSession(&open_error);
  if (!session) {
    response.error = DescribeError("Starting WinHTTP", open_error);
    return response;
  }

  // Split the URL into its parts.
  const std::wstring url = Widen(request.url);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
    response.error = DescribeError("Reading the server address", GetLastError());
    return response;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
  if (parts.lpszExtraInfo && parts.dwExtraInfoLength) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  if (path.empty()) {
    path = L"/";
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

  InternetHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
  if (!connection) {
    response.error = DescribeError("Connecting", GetLastError());
    return response;
  }

  const std::wstring method = Widen(request.method);
  InternetHandle req(WinHttpOpenRequest(connection.handle, method.c_str(), path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0));
  if (!req) {
    response.error = DescribeError("Opening the request", GetLastError());
    return response;
  }

  const int connect_ms = static_cast<int>(request.connect_timeout_ms);
  const int total_ms = static_cast<int>(request.timeout_ms);
  WinHttpSetTimeouts(req.handle, connect_ms, connect_ms, total_ms, total_ms);

  std::wstring headers;
  for (const auto& [name, value] : request.headers) {
    headers += Widen(name);
    headers += L": ";
    headers += Widen(value);
    headers += L"\r\n";
  }

  const DWORD body_size = static_cast<DWORD>(request.body.size());
  LPVOID body_data = body_size ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA;
  if (!WinHttpSendRequest(req.handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : static_cast<DWORD>(-1L), body_data, body_size,
                          body_size, 0)) {
    response.error = DescribeError("Sending the request", GetLastError());
    return response;
  }
  if (!WinHttpReceiveResponse(req.handle, nullptr)) {
    response.error = DescribeError("Waiting for the answer", GetLastError());
    return response;
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(req.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) {
    response.error = DescribeError("Reading the answer", GetLastError());
    return response;
  }

  if (request.method != "HEAD") {
    for (;;) {
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(req.handle, &available)) {
        response.error = DescribeError("Reading the answer", GetLastError());
        return response;
      }
      if (available == 0) {
        break;
      }
      if (response.body.size() + available > request.max_response_bytes) {
        response.error = "The answer was too large";
        return response;
      }
      const size_t offset = response.body.size();
      response.body.resize(offset + available);
      DWORD read = 0;
      if (!WinHttpReadData(req.handle, response.body.data() + offset, available, &read)) {
        response.error = DescribeError("Reading the answer", GetLastError());
        return response;
      }
      response.body.resize(offset + read);
      if (read == 0) {
        break;
      }
    }
  }

  response.status = static_cast<int>(status);
  return response;
}

}  // namespace rex::net
