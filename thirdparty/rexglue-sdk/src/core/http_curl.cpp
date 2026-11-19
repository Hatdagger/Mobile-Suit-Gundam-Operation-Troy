/**
 * @file        core/http_curl.cpp
 * @brief       rex::net::HttpSend on Linux and macOS, using the system libcurl
 *
 * libcurl is loaded with dlopen() the first time it is needed, so building
 * needs no curl headers or development package, and a missing libcurl only
 * turns online play off instead of stopping the game from starting.
 * The few libcurl names used below are copied from curl's public header
 * (include/curl/curl.h); they have been stable since libcurl 7.x.
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */

#if defined(_WIN32)
#error "This file is for Linux and macOS"
#endif

#include <rex/net/http.h>

#include <dlfcn.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace rex::net {
namespace {

// --- The parts of curl.h used here ----------------------------------------
using CURL = void;
struct curl_slist;
using CURLcode = int;
using curl_off_t = int64_t;

constexpr int kOptLong = 0;
constexpr int kOptObject = 10000;
constexpr int kOptFunction = 20000;
constexpr int kOptOffT = 30000;

constexpr int CURLOPT_WRITEDATA = kOptObject + 1;
constexpr int CURLOPT_URL = kOptObject + 2;
constexpr int CURLOPT_ERRORBUFFER = kOptObject + 10;
constexpr int CURLOPT_WRITEFUNCTION = kOptFunction + 11;
constexpr int CURLOPT_POSTFIELDS = kOptObject + 15;
constexpr int CURLOPT_USERAGENT = kOptObject + 18;
constexpr int CURLOPT_HTTPHEADER = kOptObject + 23;
constexpr int CURLOPT_CUSTOMREQUEST = kOptObject + 36;
constexpr int CURLOPT_NOBODY = kOptLong + 44;
constexpr int CURLOPT_POST = kOptLong + 47;
constexpr int CURLOPT_HTTPGET = kOptLong + 80;
constexpr int CURLOPT_NOSIGNAL = kOptLong + 99;
constexpr int CURLOPT_POSTFIELDSIZE_LARGE = kOptOffT + 120;
constexpr int CURLOPT_TIMEOUT_MS = kOptLong + 155;
constexpr int CURLOPT_CONNECTTIMEOUT_MS = kOptLong + 156;

constexpr int CURLINFO_RESPONSE_CODE = 0x200000 + 2;

constexpr long CURL_GLOBAL_ALL = 3;
constexpr CURLcode CURLE_OK = 0;
constexpr CURLcode CURLE_WRITE_ERROR = 23;
constexpr size_t CURL_ERROR_SIZE = 256;

struct CurlVersionInfo {
  int age;
  const char* version;
  // (more fields follow; not used)
};
constexpr int CURLVERSION_FIRST = 0;  // only "age" and "version" are read

// --- Loading libcurl --------------------------------------------------------
struct CurlApi {
  void* library = nullptr;
  std::string name;
  CURLcode (*global_init)(long) = nullptr;
  CURL* (*easy_init)() = nullptr;
  CURLcode (*easy_setopt)(CURL*, int, ...) = nullptr;
  CURLcode (*easy_perform)(CURL*) = nullptr;
  CURLcode (*easy_getinfo)(CURL*, int, ...) = nullptr;
  void (*easy_cleanup)(CURL*) = nullptr;
  const char* (*easy_strerror)(CURLcode) = nullptr;
  curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
  void (*slist_free_all)(curl_slist*) = nullptr;
  CurlVersionInfo* (*version_info)(int) = nullptr;
};

template <typename T>
bool LoadSymbol(void* library, const char* name, T& out) {
  out = reinterpret_cast<T>(dlsym(library, name));
  return out != nullptr;
}

const CurlApi* Curl() {
  static std::once_flag once;
  static CurlApi api;
  static bool ready = false;
  std::call_once(once, [] {
    const char* candidates[] = {
#if defined(__APPLE__)
        "libcurl.4.dylib", "/usr/lib/libcurl.4.dylib", "libcurl.dylib",
#else
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so",
#endif
    };
    for (const char* candidate : candidates) {
      api.library = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
      if (api.library) {
        api.name = candidate;
        break;
      }
    }
    if (!api.library) {
      return;
    }
    bool ok = LoadSymbol(api.library, "curl_global_init", api.global_init) &&
              LoadSymbol(api.library, "curl_easy_init", api.easy_init) &&
              LoadSymbol(api.library, "curl_easy_setopt", api.easy_setopt) &&
              LoadSymbol(api.library, "curl_easy_perform", api.easy_perform) &&
              LoadSymbol(api.library, "curl_easy_getinfo", api.easy_getinfo) &&
              LoadSymbol(api.library, "curl_easy_cleanup", api.easy_cleanup) &&
              LoadSymbol(api.library, "curl_easy_strerror", api.easy_strerror) &&
              LoadSymbol(api.library, "curl_slist_append", api.slist_append) &&
              LoadSymbol(api.library, "curl_slist_free_all", api.slist_free_all);
    LoadSymbol(api.library, "curl_version_info", api.version_info);  // optional
    // curl_global_init is not thread-safe; call_once makes it run exactly once.
    if (ok && api.global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
      ok = false;
    }
    ready = ok;
  });
  return ready ? &api : nullptr;
}

struct Sink {
  std::string* body;
  size_t limit;
  bool too_large = false;
};

size_t WriteCallback(char* data, size_t size, size_t count, void* user) {
  auto* sink = static_cast<Sink*>(user);
  const size_t bytes = size * count;
  if (sink->body->size() + bytes > sink->limit) {
    sink->too_large = true;
    return 0;  // makes libcurl stop with CURLE_WRITE_ERROR
  }
  sink->body->append(data, bytes);
  return bytes;
}

}  // namespace

bool HttpAvailable() {
  return Curl() != nullptr;
}

std::string HttpBackendName() {
  const CurlApi* api = Curl();
  if (!api) {
    return "none";
  }
  if (api->version_info) {
    const CurlVersionInfo* info = api->version_info(CURLVERSION_FIRST);
    if (info && info->version) {
      return std::string("libcurl ") + info->version;
    }
  }
  return "libcurl (" + api->name + ")";
}

HttpResponse HttpSend(const HttpRequest& request) {
  HttpResponse response;

  const CurlApi* api = Curl();
  if (!api) {
    response.error = "libcurl was not found on this system (needed for online play)";
    return response;
  }

  CURL* curl = api->easy_init();
  if (!curl) {
    response.error = "Starting libcurl failed";
    return response;
  }

  char error_text[CURL_ERROR_SIZE] = {};
  Sink sink{&response.body, request.max_response_bytes};
  const std::string agent(kHttpUserAgent);

  curl_slist* headers = nullptr;
  for (const auto& [name, value] : request.headers) {
    const std::string line = name + ": " + value;
    headers = api->slist_append(headers, line.c_str());
  }

  api->easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  api->easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
  api->easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  api->easy_setopt(curl, CURLOPT_ERRORBUFFER, error_text);
  api->easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_ms));
  api->easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(request.connect_timeout_ms));
  api->easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCallback);
  api->easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(&sink));
  if (headers) {
    api->easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  if (request.method == "GET") {
    api->easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (request.method == "HEAD") {
    api->easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else {
    if (request.method == "POST" || !request.body.empty()) {
      api->easy_setopt(curl, CURLOPT_POST, 1L);
      api->easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
      api->easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(request.body.size()));
    }
    if (request.method != "POST") {
      api->easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }
  }

  const CURLcode result = api->easy_perform(curl);
  if (result == CURLE_OK) {
    long status = 0;
    api->easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
  } else if (result == CURLE_WRITE_ERROR && sink.too_large) {
    response.error = "The answer was too large";
  } else {
    response.error = std::string("Request failed: ") +
                     (error_text[0] ? error_text : api->easy_strerror(result));
  }

  api->easy_cleanup(curl);
  if (headers) {
    api->slist_free_all(headers);
  }
  if (!response.error.empty()) {
    response.status = 0;
    response.body.clear();
  }
  return response;
}

}  // namespace rex::net
