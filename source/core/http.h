// HTTPS requests with libcurl. On the Switch, curl's TLS goes through the
// system SSL service, so certificates are checked against the console's CA
// store. Blocking: call from worker threads.
#pragma once
#include <atomic>
#include <string>
#include <vector>

struct HttpResult {
    long status = 0;      // HTTP status, 0 if the request failed
    std::string body;
    std::string error;    // why it failed (curl's message)
};

void httpInit();  // once at startup (curl_global_init)
void httpExit();  // at exit, after every request has finished

// cancel: checked during the transfer; setting it ends the request within ~1 s.
HttpResult httpGet(const std::string& url, const std::vector<std::string>& headers = {}, int timeoutSec = 20,
                   const std::atomic<bool>* cancel = nullptr);
// Downloads to a file (written to path.part, then renamed). False on any error.
