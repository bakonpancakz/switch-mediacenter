// HTTPS GETs through WinHTTP (Windows' own TLS, no extra libraries).
#pragma once
#include <atomic>
#include <string>
#include <vector>

struct HttpResult {
    int status = 0;  // 0 = no answer
    std::string body;
    std::string error;
};

HttpResult httpGet(const std::string& url, const std::vector<std::string>& headers = {}, int timeoutSec = 30,
                   const std::atomic<bool>* cancel = nullptr);
std::string urlEncode(const std::string& s);
