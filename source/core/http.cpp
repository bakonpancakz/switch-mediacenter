#include "core/http.h"

#include <curl/curl.h>

#include <cstdio>

#include "app.h"

static const size_t kMaxBody = 8u << 20;  // API answers are small; posters are written to files

void httpInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void httpExit() { curl_global_cleanup(); }

static size_t toString(char* p, size_t size, size_t n, void* ud) {
    auto* s = (std::string*)ud;
    if (s->size() + size * n > kMaxBody) return 0;  // aborts the transfer
    s->append(p, size * n);
    return size * n;
}


static CURL* newHandle(const std::string& url, int timeoutSec) {
    CURL* c = curl_easy_init();
    if (!c) return nullptr;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeoutSec);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // gzip if the server offers it
    curl_easy_setopt(c, CURLOPT_USERAGENT, "MediaCenter/1.0 (Nintendo Switch)");
    return c;
}

static int checkCancel(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return ((const std::atomic<bool>*)ud)->load() ? 1 : 0;  // nonzero aborts
}

HttpResult httpGet(const std::string& url, const std::vector<std::string>& headers, int timeoutSec,
                   const std::atomic<bool>* cancel) {
    HttpResult r;
    CURL* c = newHandle(url, timeoutSec);
    if (!c) { r.error = "curl_easy_init failed"; return r; }
    curl_slist* hl = nullptr;
    for (auto& h : headers) hl = curl_slist_append(hl, h.c_str());
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, toString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    if (cancel) {
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, checkCancel);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*)cancel);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    else r.error = curl_easy_strerror(rc);
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return r;
}
