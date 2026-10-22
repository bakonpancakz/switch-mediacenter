#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include "util.h"

namespace {
struct H {
    HINTERNET h = nullptr;
    ~H() { if (h) WinHttpCloseHandle(h); }
};
}  // namespace

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

HttpResult httpGet(const std::string& url, const std::vector<std::string>& headers, int timeoutSec, const std::atomic<bool>* cancel) {
    HttpResult r;
    std::wstring wurl = wide(url);
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "bad url"; return r; }
    std::wstring full = std::wstring(path, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) full += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    H session, conn, req;
    session.h = WinHttpOpen(L"MediaCenterSync/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { r.error = "WinHttpOpen failed"; return r; }
    int ms = timeoutSec * 1000;
    WinHttpSetTimeouts(session.h, ms, ms, ms, ms);
    conn.h = WinHttpConnect(session.h, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (!conn.h) { r.error = "connect failed"; return r; }
    req.h = WinHttpOpenRequest(conn.h, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) { r.error = "request failed"; return r; }
    std::wstring hdrs;
    for (auto& h : headers) hdrs += wide(h) + L"\r\n";
    if (!WinHttpSendRequest(req.h, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = "no response (" + std::to_string(GetLastError()) + ")";
        return r;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    r.status = (int)status;
    for (;;) {
        if (cancel && *cancel) { r.error = "cancelled"; r.status = 0; return r; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) { r.error = "read failed"; r.status = 0; return r; }
        if (!avail) break;
        size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, &r.body[at], avail, &got)) { r.error = "read failed"; r.status = 0; return r; }
        r.body.resize(at + got);
        if (r.body.size() > (64u << 20)) { r.error = "too big"; r.status = 0; return r; }
    }
    return r;
}
