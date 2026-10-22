#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <chrono>
#include <ctime>
#include <mutex>

#include "fsx.h"

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

std::wstring winPath(const std::string& path, bool longForm) {
    std::wstring w = wide(path);
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    bool absolute = w.size() > 2 && w[1] == L':' && w[2] == L'\\';
    if (absolute && (longForm || w.size() >= 240)) w = L"\\\\?\\" + w;
    return w;
}

std::string forwardSlashes(std::string path) {
    for (auto& c : path)
        if (c == '\\') c = '/';
    return path;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

std::string baseOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string extOf(const std::string& name) {
    std::string b = baseOf(name);
    size_t d = b.rfind('.');
    return d == std::string::npos || d == 0 ? "" : lower(b.substr(d + 1));
}

std::string stemOf(const std::string& name) {
    std::string b = baseOf(name);
    size_t d = b.rfind('.');
    return d == std::string::npos || d == 0 ? b : b.substr(0, d);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string formatBytes(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1000 && i < 4) { b /= 1000; i++; }
    char buf[32];
    snprintf(buf, sizeof(buf), i == 0 ? "%.0f %s" : b < 10 ? "%.2f %s" : b < 100 ? "%.1f %s" : "%.0f %s", b, u[i]);
    return buf;
}

int64_t unixNow() { return (int64_t)time(nullptr); }

uint64_t monoMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string dataDir() {
    static std::string dir = [] {
        std::string d;
        // the tests point it somewhere temporary
        if (const wchar_t* env = _wgetenv(L"MCSYNC_DATA_DIR")) d = utf8(env);
        else {
            wchar_t* p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = utf8(p);
            CoTaskMemFree(p);
            if (d.empty()) d = ".";
            d += "/MediaCenterSync";
        }
        d = forwardSlashes(d);
        fsx::mkdirs(d);
        return d;
    }();
    return dir;
}

// ---------------------------------------------------------------- log
static std::mutex s_logMx;
static std::function<void(const std::string&)> s_listener;

void setLogListener(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> l(s_logMx);
    s_listener = std::move(fn);
}

void logLine(const std::string& s) {
    char stamp[32];
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", &lt);
    std::lock_guard<std::mutex> l(s_logMx);
    static FILE* f = [] {
        std::string path = dataDir() + "/log.txt";
        // keep the last run's log as log.old.txt once it gets big
        if (fsx::size(path) > (4 << 20)) fsx::rename(path, dataDir() + "/log.old.txt");
        return fsx::open(path, "ab");
    }();
    if (f) {
        fprintf(f, "%s%s\r\n", stamp, s.c_str());
        fflush(f);
    }
    if (s_listener) s_listener(s);
}

// ---------------------------------------------------------------- hashing
std::string hashHex(const void* data, size_t len) {
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)len, digest, sizeof(digest));
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; i++) {
        s += hex[digest[i] >> 4];
        s += hex[digest[i] & 15];
    }
    return s;
}

std::string cardSafe(const std::string& path) {
    std::string out, part;
    auto flush = [&] {
        bool plain = true;
        for (unsigned char c : part) plain &= c < 0x80;
        if (plain) out += part;
        else {
            std::string ascii;
            for (size_t i = 0; i < part.size(); i++) {
                unsigned char c = (unsigned char)part[i];
                if (c < 0x80) ascii += (char)c;
                else if ((c & 0xC0) != 0x80 && (ascii.empty() || ascii.back() != '_')) ascii += '_';  // one per run
            }
            if (ascii.size() > 80) ascii.resize(80);
            out += ascii + "~" + hashHex(part.data(), part.size()).substr(0, 8);
        }
        part.clear();
    };
    for (char c : path) {
        if (c == '/' || c == '.') {
            flush();
            out += c;
        } else part += c;
    }
    flush();
    return out;
}

bool readWhole(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fsx::open(path, "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeWhole(const std::string& path, const void* data, size_t len) {
    std::string tmp = path + ".tmp";
    FILE* f = fsx::open(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok && fsx::rename(tmp, path);
}

// ---------------------------------------------------------------- processes
// Quotes one argument the way CommandLineToArgvW (and the C runtime) reads it back.
static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    q += L'"';
    return q;
}

int runProcess(const std::wstring& exe, const std::vector<std::wstring>& args, std::vector<uint8_t>& out, int timeoutMs,
               const std::atomic<bool>* cancel) {
    out.clear();
    std::wstring cmd = quoteArg(exe);
    for (auto& a : args) cmd += L" " + quoteArg(a);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    uint64_t start = monoMs();
    bool killed = false;
    uint8_t buf[65536];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // closed: the program is done
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, sizeof(buf), &got, nullptr) || !got) break;
            out.insert(out.end(), buf, buf + got);
            continue;
        }
        if ((int)(monoMs() - start) > timeoutMs || (cancel && *cancel)) {
            TerminateProcess(pi.hProcess, 1);
            killed = true;
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 5) == WAIT_OBJECT_0) {
            // it ended: take what's left in the pipe
            DWORD got = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got)
                out.insert(out.end(), buf, buf + got);
            break;
        }
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return killed ? -1 : (int)code;
}
