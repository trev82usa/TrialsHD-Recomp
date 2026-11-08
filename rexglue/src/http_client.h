#pragma once

#include <string>
#include <utility>
#include <vector>

// Minimal blocking HTTP(S) client. Windows: WinHTTP. Linux: libcurl, loaded at run time
// (libcurl.so.4) so the game still starts on systems without it.
struct HttpResponse {
  int status = 0;        // 0 = could not connect / no client available
  std::string body;
  std::string error;     // set when status == 0
};

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, int timeout_ms);
