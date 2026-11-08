#include "http_client.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace {
std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
}  // namespace

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, int timeout_ms) {
  HttpResponse r;
  std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2047;
  wchar_t extra[2048] = {};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2047;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    r.error = "bad URL";
    return r;
  }
  HINTERNET session = WinHttpOpen(L"TrialsHD/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    r.error = "WinHttpOpen failed";
    return r;
  }
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
  std::wstring full_path = std::wstring(path) + extra;
  HINTERNET req = conn ? WinHttpOpenRequest(conn, Widen(method).c_str(), full_path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            uc.nScheme == INTERNET_SCHEME_HTTPS
                                                ? WINHTTP_FLAG_SECURE
                                                : 0)
                       : nullptr;
  if (req) {
    std::wstring hdrs;
    for (auto& [k, v] : headers) hdrs += Widen(k + ": " + v + "\r\n");
    if (WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                           DWORD(-1), (LPVOID)body.data(), DWORD(body.size()),
                           DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
      DWORD status = 0, len = sizeof(status);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
      r.status = int(status);
      for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read) || !read) break;
        r.body.append(chunk.data(), read);
      }
    } else {
      r.error = "request failed (error " + std::to_string(GetLastError()) + ")";
    }
    WinHttpCloseHandle(req);
  } else {
    r.error = "could not open connection";
  }
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return r;
}

#else  // Linux: libcurl via dlopen

#include <dlfcn.h>
#include <mutex>

namespace {
using CURL = void;
struct curl_slist;
// Values from curl.h (stable ABI).
constexpr int CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_HTTPHEADER = 10023,
              CURLOPT_WRITEFUNCTION = 20011, CURLOPT_WRITEDATA = 10001,
              CURLOPT_CUSTOMREQUEST = 10036, CURLOPT_TIMEOUT_MS = 155,
              CURLOPT_CONNECTTIMEOUT_MS = 156, CURLOPT_POSTFIELDSIZE = 60,
              CURLOPT_NOSIGNAL = 99, CURLOPT_USERAGENT = 10018, CURLOPT_HTTPGET = 80;
constexpr int CURLINFO_RESPONSE_CODE = 0x200000 + 2;
constexpr long CURL_GLOBAL_DEFAULT = 3;

struct Curl {
  bool ok = false;
  int (*global_init)(long);
  CURL* (*easy_init)();
  int (*easy_setopt)(CURL*, int, ...);
  int (*easy_perform)(CURL*);
  int (*easy_getinfo)(CURL*, int, ...);
  void (*easy_cleanup)(CURL*);
  const char* (*easy_strerror)(int);
  curl_slist* (*slist_append)(curl_slist*, const char*);
  void (*slist_free_all)(curl_slist*);
};

Curl& GetCurl() {
  static Curl c;
  static std::once_flag once;
  std::call_once(once, [] {
    void* h = nullptr;
    for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"})
      if ((h = dlopen(name, RTLD_NOW))) break;
    if (!h) return;
    auto sym = [h](const char* n) { return dlsym(h, n); };
    c.global_init = (int (*)(long))sym("curl_global_init");
    c.easy_init = (CURL * (*)()) sym("curl_easy_init");
    c.easy_setopt = (int (*)(CURL*, int, ...))sym("curl_easy_setopt");
    c.easy_perform = (int (*)(CURL*))sym("curl_easy_perform");
    c.easy_getinfo = (int (*)(CURL*, int, ...))sym("curl_easy_getinfo");
    c.easy_cleanup = (void (*)(CURL*))sym("curl_easy_cleanup");
    c.easy_strerror = (const char* (*)(int))sym("curl_easy_strerror");
    c.slist_append = (curl_slist * (*)(curl_slist*, const char*)) sym("curl_slist_append");
    c.slist_free_all = (void (*)(curl_slist*))sym("curl_slist_free_all");
    c.ok = c.global_init && c.easy_init && c.easy_setopt && c.easy_perform && c.easy_getinfo &&
           c.easy_cleanup && c.slist_append && c.slist_free_all && c.easy_strerror;
    if (c.ok) c.global_init(CURL_GLOBAL_DEFAULT);
  });
  return c;
}

size_t OnData(char* p, size_t size, size_t n, void* user) {
  static_cast<std::string*>(user)->append(p, size * n);
  return size * n;
}
}  // namespace

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, int timeout_ms) {
  HttpResponse r;
  Curl& c = GetCurl();
  if (!c.ok) {
    r.error = "libcurl not found (install libcurl4)";
    return r;
  }
  CURL* h = c.easy_init();
  if (!h) {
    r.error = "curl_easy_init failed";
    return r;
  }
  curl_slist* list = nullptr;
  for (auto& [k, v] : headers) list = c.slist_append(list, (k + ": " + v).c_str());
  c.easy_setopt(h, CURLOPT_URL, url.c_str());
  c.easy_setopt(h, CURLOPT_USERAGENT, "TrialsHD/1");
  c.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  c.easy_setopt(h, CURLOPT_TIMEOUT_MS, long(timeout_ms));
  c.easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, long(timeout_ms));
  if (list) c.easy_setopt(h, CURLOPT_HTTPHEADER, list);
  if (method == "GET") {
    c.easy_setopt(h, CURLOPT_HTTPGET, 1L);
  } else {
    c.easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    c.easy_setopt(h, CURLOPT_POSTFIELDSIZE, long(body.size()));
    c.easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
  }
  c.easy_setopt(h, CURLOPT_WRITEFUNCTION, &OnData);
  c.easy_setopt(h, CURLOPT_WRITEDATA, &r.body);
  int rc = c.easy_perform(h);
  if (rc == 0) {
    long status = 0;
    c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    r.status = int(status);
  } else {
    r.error = c.easy_strerror(rc);
  }
  if (list) c.slist_free_all(list);
  c.easy_cleanup(h);
  return r;
}

#endif
