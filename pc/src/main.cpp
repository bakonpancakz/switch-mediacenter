// Media Center Sync: the PC half of Media Center. You tell it which folders go
// to the Switch and as what (Files, Media, Books); it builds the library
// (thumbnails, waveforms, TMDB posters, book covers) and copies whatever
// changed to the Switch's SD card when it's in this PC (a card reader, or hekate's USB mass storage).
// Lives in the tray; settings in %LOCALAPPDATA%\MediaCenterSync.
#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "builder.h"
#include "config.h"
#include "fsx.h"
#include "images.h"
#include "library.h"
#include "card_sync.h"
#include "tmdb.h"
#include "winmedia.h"
#include "util.h"

namespace {

enum {
    ID_LIST = 100, ID_ADD, ID_EDIT, ID_REMOVE, ID_FIX, ID_SETTINGS, ID_SYNC, ID_AUTO, ID_STATUS, ID_PROGRESS, ID_LOG, ID_SWITCH,
    ID_M_FOLDER = 150, ID_M_BROWSE, ID_M_CATEGORY, ID_M_NAME, ID_M_WHERE,
    ID_S_TOKEN = 170, ID_S_LANG, ID_S_FFMPEG, ID_S_FFBROWSE, ID_S_TRAY, ID_S_STARTUP,
    ID_F_LIST = 190, ID_F_QUERY, ID_F_SEARCH, ID_F_RESULTS, ID_F_USE, ID_F_NONE, ID_F_CLOSE,
    ID_TRAY_OPEN = 220, ID_TRAY_SYNC, ID_TRAY_QUIT,
    TIMER_UI = 1, TIMER_DEVICE = 2,
    WM_TRAY = WM_APP + 1, WM_LOGLINE, WM_JOBDONE,
};

const wchar_t* APP = L"Media Center Sync";
const wchar_t* CLASS_NAME = L"MediaCenterSyncWindow";
const wchar_t* EDITOR_CLASS = L"MediaCenterSyncEditor";

HWND g_wnd, g_switch, g_label, g_list, g_add, g_edit, g_remove, g_fix, g_settings, g_sync, g_auto, g_status, g_progress, g_log;
HFONT g_font, g_bold;
HICON g_iconBig, g_iconSmall;
HINSTANCE g_inst;
UINT g_dpi = 96;
NOTIFYICONDATAW g_tray = {};

Config g_cfg;
std::thread g_worker;
std::atomic<bool> g_busy{false}, g_cancel{false};
std::mutex g_mx;
struct Status {
    std::string stage, item;
    int done = 0, total = 0;
    uint64_t bytesDone = 0, bytes = 0;
    std::string idle = "Press Sync now, or put the Switch's SD card in";
} g_st;
std::shared_ptr<Model> g_lastModel;      // from the last build (for Fix matches)
Card g_card;                             // the Switch's SD card, when one is in (root empty otherwise)
bool g_syncWhenInserted = false;         // built without the card: send it when it shows up

int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}

// ---------------------------------------------------------------- icon

// Drawn here rather than shipped: two arrows chasing each other round a ring.
HICON makeSyncIcon(int size, uint32_t rgb) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    const double c = size / 2.0, ro = size * 0.42, ri = size * 0.29, rm = (ro + ri) / 2, PI = 3.14159265358979;
    auto inTri = [](double px, double py, double ax, double ay, double bx, double by, double cx, double cy) {
        double d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
        double d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
        double d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
        bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        return !(neg && pos);
    };
    auto inside = [&](double x, double y) {
        double dx = x - c, dy = c - y, r = std::sqrt(dx * dx + dy * dy);
        double a = std::atan2(dy, dx) * 180 / PI;
        if (a < 0) a += 360;
        if (r >= ri && r <= ro && ((a >= 35 && a <= 145) || (a >= 215 && a <= 325))) return true;
        for (double end : {145.0, 325.0}) {  // arrowheads, pointing on round the ring
            double t = end * PI / 180, px = c + rm * std::cos(t), py = c - rm * std::sin(t);
            double tx = -std::sin(t), ty = -std::cos(t), nx = std::cos(t), ny = -std::sin(t);
            double w = (ro - ri) * 1.15, len = (ro - ri) * 1.4;
            if (inTri(x, y, px + nx * w, py + ny * w, px - nx * w, py - ny * w, px + tx * len, py + ty * len)) return true;
        }
        return false;
    };
    uint32_t* px = (uint32_t*)bits;
    const int R = (rgb >> 16) & 255, G = (rgb >> 8) & 255, B = rgb & 255, SS = 4;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int hits = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) hits += inside(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS);
            int a = hits * 255 / (SS * SS);
            px[y * size + x] = (a << 24) | ((R * a / 255) << 16) | ((G * a / 255) << 8) | (B * a / 255);  // premultiplied
        }
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------- small helpers

void notify(const std::wstring& title, const std::wstring& text) {
    g_tray.uFlags = NIF_INFO;
    wcsncpy(g_tray.szInfoTitle, title.c_str(), 63);
    wcsncpy(g_tray.szInfo, text.c_str(), 255);
    g_tray.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    g_tray.hBalloonIcon = g_iconBig;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void setStage(const std::string& stage, const std::string& item = "", int done = 0, int total = 0) {
    std::lock_guard<std::mutex> l(g_mx);
    g_st.stage = stage;
    g_st.item = item;
    g_st.done = done;
    g_st.total = total;
    g_st.bytes = g_st.bytesDone = 0;
}

bool browseFolder(HWND owner, const wchar_t* title, std::wstring& path) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                path = p;
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

std::string pcName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(buf, &n) ? utf8(buf) : "PC";
}

// "Start with Windows": a Run entry for this exe, in the tray.
const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool startsWithWindows() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool there = RegQueryValueExW(k, L"MediaCenterSync", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return there;
}
void setStartsWithWindows(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" --tray";
        RegSetValueExW(k, L"MediaCenterSync", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"MediaCenterSync");
    RegCloseKey(k);
}

// ---------------------------------------------------------------- the job

void jobThread(Config cfg, bool wantSync) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    imagesThreadInit();
    winMediaThreadInit();
    std::string summary;
    bool ok = false, sent = false;
    Sql work;
    if (!openWorkDb(work, dataDir() + "/work.db")) summary = "Couldn't open " + dataDir() + "/work.db";
    else {
        FfTools ff = findFfmpeg(cfg.ffmpegDir);
        Builder b(work, cfg, ff, g_cancel, [](const BuildProgress& p) { setStage(p.stage, p.item, p.done, p.total); });
        auto model = std::make_shared<Model>();
        setStage("Looking through your folders");
        if (!b.build(*model)) {
            summary = g_cancel ? "Stopped" : b.error();
        } else {
            for (auto& w : b.warnings()) logLine(w);
            {
                std::lock_guard<std::mutex> l(g_mx);
                g_lastModel = model;
            }
            int files = 0;
            for (auto& it : model->items) files += it.kind != lib::K_DIR;
            logLine("library: " + std::to_string(files) + " files, " + std::to_string(model->shows.size()) + " shows, " +
                    std::to_string(model->movies.size()) + " movies, " + std::to_string(model->books.size()) + " books");
            std::vector<Card> cards = findCards();
            if (!wantSync || cards.empty()) {
                ok = true;
                summary = cards.empty() ? "Ready. Put the Switch's SD card in (a card reader, or hekate's USB mass storage) to send it"
                                        : "Library is up to date";
                g_syncWhenInserted = cards.empty();
            } else {
                const Card& c = cards[0];
                logLine("syncing to " + c.label + " (" + c.root + ")");
                SyncResult r = syncToCard(work, *model, c.root, pcName(), g_cancel, [](const SyncProgress& p) {
                    std::lock_guard<std::mutex> l(g_mx);
                    g_st.stage = p.action;
                    g_st.item = p.item;
                    g_st.done = p.stepsDone;
                    g_st.total = p.steps;
                    g_st.bytesDone = p.bytesDone;
                    g_st.bytes = p.bytes;
                });
                sent = true;
                ok = r.ok;
                g_syncWhenInserted = !r.ok;
                if (g_cancel) summary = "Stopped";
                else if (!r.ok) summary = r.error;
                else if (r.nothingChanged) summary = "The card is up to date";
                if (r.ok && !b.unmatched().empty() && r.nothingChanged)
                    summary += " (" + std::to_string(b.unmatched().size()) + " not found on TMDB: Fix shows/movies)";
                else {
                    summary = "Synced: " + std::to_string(r.copied) + " copied (" + formatBytes((double)r.bytes) + ")";
                    if (r.deleted) summary += ", " + std::to_string(r.deleted) + " deleted";
                    if (r.failed) summary += ", " + std::to_string(r.failed) + " couldn't be read (see the log)";
                    if (!b.unmatched().empty())
                        summary += ", " + std::to_string(b.unmatched().size()) + " not found on TMDB (Fix shows/movies)";
                    summary += ". Eject the card before taking it out";
                }
            }
        }
    }
    logLine(summary);
    winMediaThreadExit();
    imagesThreadExit();
    CoUninitialize();
    PostMessageW(g_wnd, WM_JOBDONE, (ok ? 1 : 0) | (sent ? 2 : 0), (LPARAM) new std::wstring(wide(summary)));
}

void startJob(bool wantSync) {
    if (g_busy) return;
    if (g_cfg.mappings.empty()) {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
        MessageBoxW(g_wnd, L"Add a folder first: pick a folder on this PC and what it is (Files, Media or Books).", APP, MB_ICONINFORMATION);
        return;
    }
    if (g_worker.joinable()) g_worker.join();
    g_busy = true;
    g_cancel = false;
    SetWindowTextW(g_sync, L"Stop");
    for (HWND h : {g_add, g_edit, g_remove, g_fix, g_settings}) EnableWindow(h, FALSE);
    g_worker = std::thread(jobThread, g_cfg, wantSync);
}

void stopJob() {
    g_cancel = true;
    std::lock_guard<std::mutex> l(g_mx);
    g_st.stage = "Stopping";
}

// ---------------------------------------------------------------- modal editors

// A small window built from controls, shown modally (the owner is disabled
// while it's up). build() makes the controls; command() handles buttons and
// returns true to close.
struct Editor {
    HWND wnd = nullptr;
    bool done = false;
    std::function<void(HWND)> build;
    std::function<bool(HWND, int, int)> command;  // (window, id, notification code)
    std::function<void(HWND, NMHDR*)> notify;
};
Editor* g_editor = nullptr;

HWND mkCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

LRESULT CALLBACK editorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Editor* e = g_editor;
    switch (msg) {
    case WM_CREATE:
        e->wnd = h;
        e->build(h);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if (GetDlgCtrlID((HWND)lp) == ID_M_WHERE) SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL || e->command(h, LOWORD(wp), HIWORD(wp))) DestroyWindow(h);
        return 0;
    case WM_NOTIFY:
        if (e->notify) e->notify(h, (NMHDR*)lp);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: e->done = true; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void runEditor(Editor& e, const wchar_t* title, int w, int h) {
    Editor* outer = g_editor;
    g_editor = &e;
    RECT r = {0, 0, S(w), S(h)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
    RECT owner;
    GetWindowRect(g_wnd, &owner);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, EDITOR_CLASS, title, WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               owner.left + (owner.right - owner.left - ww) / 2, owner.top + S(50), ww, hh, g_wnd, nullptr, g_inst, nullptr);
    EnableWindow(g_wnd, FALSE);
    MSG m;
    while (!e.done && GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(g_wnd, TRUE);
    SetForegroundWindow(g_wnd);
    g_editor = outer;
}

// Add or change a folder mapping. Returns true when OK was pressed.
bool editMapping(Mapping& m, bool isNew) {
    Mapping result = m;
    bool ok = false;
    HWND folder = nullptr, category = nullptr, name = nullptr, where = nullptr;
    bool nameTouched = !isNew;
    auto updateWhere = [&] {
        int c = (int)SendMessageW(category, CB_GETCURSEL, 0, 0);
        std::string n = cleanMappingName(utf8(getText(name)));
        std::wstring t = L"On the Switch: " + wide(std::string(lib::categoryLabel(c))) + L" tab, folder “" + wide(n) +
                         L"”  (sdmc:/switch/mediacenter/" + wide(std::string(lib::categoryDir(c))) + L"/" + wide(cardSafe(n)) + L")";
        SetWindowTextW(where, t.c_str());
    };
    Editor e;
    e.build = [&](HWND h) {
        mkCtl(h, L"STATIC", L"Folder on this PC", 0, 0, 16, 14, 460, 20);
        folder = mkCtl(h, L"EDIT", winPath(m.folder).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, ID_M_FOLDER, 16, 36, 352, 26, WS_EX_CLIENTEDGE);
        mkCtl(h, L"BUTTON", L"Browse…", WS_TABSTOP | BS_PUSHBUTTON, ID_M_BROWSE, 376, 35, 100, 28);
        mkCtl(h, L"STATIC", L"What's in it", 0, 0, 16, 76, 220, 20);
        category = mkCtl(h, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, ID_M_CATEGORY, 16, 98, 220, 200);
        for (const wchar_t* c : {L"Files (photos, videos, music)", L"Media (shows and movies)", L"Books (EPUB, PDF, comics)"})
            SendMessageW(category, CB_ADDSTRING, 0, (LPARAM)c);
        SendMessageW(category, CB_SETCURSEL, m.category, 0);
        mkCtl(h, L"STATIC", L"Name on the Switch", 0, 0, 252, 76, 224, 20);
        name = mkCtl(h, L"EDIT", wide(m.name).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, ID_M_NAME, 252, 98, 224, 26, WS_EX_CLIENTEDGE);
        where = mkCtl(h, L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_M_WHERE, 16, 136, 460, 20);
        mkCtl(h, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 290, 172, 88, 30);
        mkCtl(h, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 388, 172, 88, 30);
        updateWhere();
        SetFocus(isNew ? folder : name);
    };
    e.command = [&](HWND h, int id, int code) -> bool {
        if (id == ID_M_BROWSE) {
            std::wstring p;
            if (browseFolder(h, L"Folder to put on the Switch", p)) {
                SetWindowTextW(folder, p.c_str());
                if (!nameTouched) SetWindowTextW(name, wide(cleanMappingName(baseOf(utf8(p)))).c_str());
                updateWhere();
            }
            return false;
        }
        if (id == ID_M_NAME && code == EN_CHANGE) {
            if (GetFocus() == name) nameTouched = true;
            updateWhere();
        }
        if (id == ID_M_CATEGORY && code == CBN_SELCHANGE) updateWhere();
        if (id == ID_M_FOLDER && code == EN_KILLFOCUS && !nameTouched && getText(name).empty()) {
            SetWindowTextW(name, wide(cleanMappingName(baseOf(utf8(getText(folder))))).c_str());
            updateWhere();
        }
        if (id != IDOK) return false;
        Mapping r = m;
        r.folder = forwardSlashes(utf8(getText(folder)));
        while (r.folder.size() > 3 && r.folder.back() == '/') r.folder.pop_back();
        r.category = (int)SendMessageW(category, CB_GETCURSEL, 0, 0);
        r.name = cleanMappingName(utf8(getText(name)).empty() ? baseOf(r.folder) : utf8(getText(name)));
        if (r.folder.empty() || !fsx::isDir(r.folder)) {
            MessageBoxW(h, L"Pick a folder on this PC that exists.", APP, MB_ICONINFORMATION);
            return false;
        }
        for (auto& o : g_cfg.mappings) {
            if (o.id == r.id) continue;
            if (o.category == r.category && lower(o.name) == lower(r.name)) {
                MessageBoxW(h, L"Another folder already has that name in that tab. Pick another name.", APP, MB_ICONINFORMATION);
                return false;
            }
            std::string a = lower(o.folder) + "/", b = lower(r.folder) + "/";
            if (a.compare(0, b.size(), b) == 0 || b.compare(0, a.size(), a) == 0) {
                MessageBoxW(h, (L"That folder overlaps with " + winPath(o.folder) + L", which is already on the list.").c_str(), APP,
                            MB_ICONINFORMATION);
                return false;
            }
        }
        result = r;
        ok = true;
        return true;
    };
    runEditor(e, isNew ? L"Add a folder" : L"Edit folder", 492, 216);
    if (ok) m = result;
    return ok;
}

void editSettings() {
    HWND token = nullptr, lang = nullptr, ff = nullptr, tray = nullptr, startup = nullptr;
    Editor e;
    e.build = [&](HWND h) {
        mkCtl(h, L"STATIC", L"TMDB API key or read access token (free at themoviedb.org: posters, titles, episode names)", 0, 0, 16, 14, 500, 20);
        token = mkCtl(h, L"EDIT", wide(g_cfg.tmdbToken).c_str(), WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, ID_S_TOKEN, 16, 36, 500, 26,
                      WS_EX_CLIENTEDGE);
        mkCtl(h, L"STATIC", L"Language for titles and descriptions (like en-US, ja-JP, de-DE)", 0, 0, 16, 74, 500, 20);
        lang = mkCtl(h, L"EDIT", wide(g_cfg.language).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, ID_S_LANG, 16, 96, 140, 26, WS_EX_CLIENTEDGE);
        mkCtl(h, L"STATIC", L"Folder with ffmpeg.exe and ffprobe.exe (blank: find them)", 0, 0, 16, 134, 500, 20);
        ff = mkCtl(h, L"EDIT", winPath(g_cfg.ffmpegDir).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, ID_S_FFMPEG, 16, 156, 392, 26, WS_EX_CLIENTEDGE);
        mkCtl(h, L"BUTTON", L"Browse…", WS_TABSTOP | BS_PUSHBUTTON, ID_S_FFBROWSE, 416, 155, 100, 28);
        FfTools found = findFfmpeg(g_cfg.ffmpegDir);
        mkCtl(h, L"STATIC", found.ok() ? (L"Using " + found.ffmpeg).c_str() : L"Not found: videos and music won't get thumbnails or lengths",
              SS_ENDELLIPSIS | SS_NOPREFIX, ID_M_WHERE, 16, 188, 500, 20);
        tray = mkCtl(h, L"BUTTON", L"Closing the window keeps it running in the tray", WS_TABSTOP | BS_AUTOCHECKBOX, ID_S_TRAY, 16, 222, 500, 22);
        SendMessageW(tray, BM_SETCHECK, g_cfg.closeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
        startup = mkCtl(h, L"BUTTON", L"Start with Windows (in the tray)", WS_TABSTOP | BS_AUTOCHECKBOX, ID_S_STARTUP, 16, 248, 500, 22);
        SendMessageW(startup, BM_SETCHECK, startsWithWindows() ? BST_CHECKED : BST_UNCHECKED, 0);
        mkCtl(h, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 330, 286, 88, 30);
        mkCtl(h, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 428, 286, 88, 30);
    };
    e.command = [&](HWND h, int id, int) -> bool {
        if (id == ID_S_FFBROWSE) {
            std::wstring p;
            if (browseFolder(h, L"Folder with ffmpeg.exe and ffprobe.exe", p)) SetWindowTextW(ff, p.c_str());
            return false;
        }
        if (id != IDOK) return false;
        g_cfg.tmdbToken = trim(utf8(getText(token)));
        g_cfg.language = trim(utf8(getText(lang)));
        if (g_cfg.language.empty()) g_cfg.language = "en-US";
        g_cfg.ffmpegDir = forwardSlashes(trim(utf8(getText(ff))));
        g_cfg.closeToTray = SendMessageW(tray, BM_GETCHECK, 0, 0) == BST_CHECKED;
        bool start = SendMessageW(startup, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (start != startsWithWindows()) setStartsWithWindows(start);
        g_cfg.save(configPath());
        return true;
    };
    runEditor(e, L"Settings", 532, 330);
}

// Shows and movies with the TMDB match they got; pick another, or "not on TMDB".
void fixMatches() {
    std::shared_ptr<Model> model;
    {
        std::lock_guard<std::mutex> l(g_mx);
        model = g_lastModel;
    }
    if (!model || (model->shows.empty() && model->movies.empty())) {
        MessageBoxW(g_wnd, L"Sync once first (with a folder added as Media) so the shows and movies are known.", APP, MB_ICONINFORMATION);
        return;
    }
    if (g_cfg.tmdbToken.empty()) {
        MessageBoxW(g_wnd, L"Add a TMDB key in Settings first.", APP, MB_ICONINFORMATION);
        return;
    }
    struct Row { bool tv; std::string key, title; int tmdb, year; };
    std::vector<Row> rows;
    for (auto& s : model->shows) rows.push_back({true, s.key, s.title, s.tmdb, s.year});
    for (auto& m : model->movies) rows.push_back({false, m.key, m.title, m.tmdb, m.year});
    // the ones TMDB didn't find first: they're what needs fixing
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return (a.tmdb == 0) > (b.tmdb == 0); });
    Sql work;
    openWorkDb(work, dataDir() + "/work.db");
    Tmdb tmdb(work, g_cfg.tmdbToken, g_cfg.language);
    std::vector<TmdbHit> hits;
    HWND list = nullptr, query = nullptr, results = nullptr;
    bool changed = false;
    auto selected = [&] { return (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); };
    auto setRow = [&](int i) {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        std::wstring t = wide(rows[i].title + (rows[i].year ? " (" + std::to_string(rows[i].year) + ")" : ""));
        std::wstring k = rows[i].tv ? L"Show" : L"Movie";
        std::wstring m = rows[i].tmdb ? L"TMDB " + std::to_wstring(rows[i].tmdb) : L"not matched";
        it.pszText = (LPWSTR)t.c_str();
        if (SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) <= i) SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        else SendMessageW(list, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = (LPWSTR)k.c_str();
        SendMessageW(list, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = (LPWSTR)m.c_str();
        SendMessageW(list, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    };
    auto search = [&](HWND h) {
        int i = selected();
        if (i < 0) return;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        hits = tmdb.search(rows[i].tv, utf8(getText(query)));
        SendMessageW(results, LB_RESETCONTENT, 0, 0);
        for (auto& hh : hits)
            SendMessageW(results, LB_ADDSTRING, 0, (LPARAM)wide(hh.name + (hh.year ? " (" + std::to_string(hh.year) + ")" : "") +
                                                                 (hh.overview.empty() ? "" : "  —  " + hh.overview.substr(0, 90))).c_str());
        if (hits.empty())
            SendMessageW(results, LB_ADDSTRING, 0, (LPARAM)(tmdb.lastError().empty() ? L"Nothing found: try another name" : wide(tmdb.lastError()).c_str()));
        (void)h;
    };
    Editor e;
    e.build = [&](HWND h) {
        mkCtl(h, L"STATIC", L"Pick a show or movie, search for the right one, then Use it. Changes reach the Switch at the next sync.", 0, 0, 16,
              12, 640, 20);
        list = mkCtl(h, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, ID_F_LIST, 16, 38, 640, 220,
                     WS_EX_CLIENTEDGE);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = {L"Title", L"Kind", L"Matched to"};
        int widths[] = {420, 80, 120};
        for (int i = 0; i < 3; i++) {
            LVCOLUMNW c = {};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (LPWSTR)cols[i];
            c.cx = S(widths[i]);
            SendMessageW(list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
        }
        for (int i = 0; i < (int)rows.size(); i++) setRow(i);
        query = mkCtl(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_F_QUERY, 16, 270, 532, 26, WS_EX_CLIENTEDGE);
        mkCtl(h, L"BUTTON", L"Search", WS_TABSTOP | BS_PUSHBUTTON, ID_F_SEARCH, 556, 269, 100, 28);
        results = mkCtl(h, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_F_RESULTS, 16, 306, 640, 150,
                        WS_EX_CLIENTEDGE);
        mkCtl(h, L"BUTTON", L"Use this one", WS_TABSTOP | BS_PUSHBUTTON, ID_F_USE, 16, 468, 130, 30);
        mkCtl(h, L"BUTTON", L"Not on TMDB", WS_TABSTOP | BS_PUSHBUTTON, ID_F_NONE, 154, 468, 130, 30);
        mkCtl(h, L"BUTTON", L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_F_CLOSE, 568, 468, 88, 30);
    };
    e.notify = [&](HWND, NMHDR* n) {
        if (n->hwndFrom == list && n->code == LVN_ITEMCHANGED) {
            int i = selected();
            if (i >= 0) SetWindowTextW(query, wide(rows[i].title).c_str());
            SendMessageW(results, LB_RESETCONTENT, 0, 0);
            hits.clear();
        }
    };
    e.command = [&](HWND h, int id, int code) -> bool {
        int i = selected();
        if (id == ID_F_SEARCH) search(h);
        else if (id == ID_F_USE || (id == ID_F_RESULTS && code == LBN_DBLCLK)) {
            int r = (int)SendMessageW(results, LB_GETCURSEL, 0, 0);
            if (i >= 0 && r >= 0 && r < (int)hits.size()) {
                tmdb.setMatch(rows[i].tv, rows[i].key, hits[r].id);
                rows[i].tmdb = hits[r].id;
                setRow(i);
                changed = true;
            }
        } else if (id == ID_F_NONE && i >= 0) {
            tmdb.setMatch(rows[i].tv, rows[i].key, 0);
            rows[i].tmdb = 0;
            setRow(i);
            changed = true;
        }
        return id == ID_F_CLOSE;
    };
    runEditor(e, L"Fix shows and movies", 672, 512);
    if (changed) logLine("Matches changed: they reach the Switch at the next sync");
}

// ---------------------------------------------------------------- the window

std::string categoryOf(const Mapping& m) { return lib::categoryLabel(m.category); }

void fillList() {
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < g_cfg.mappings.size(); i++) {
        const Mapping& m = g_cfg.mappings[i];
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        std::wstring name = wide(m.name), cat = wide(categoryOf(m)), folder = winPath(m.folder);
        it.pszText = (LPWSTR)name.c_str();
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = (LPWSTR)cat.c_str();
        SendMessageW(g_list, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        it.iSubItem = 2;
        it.pszText = (LPWSTR)folder.c_str();
        SendMessageW(g_list, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
    EnableWindow(g_edit, FALSE);
    EnableWindow(g_remove, FALSE);
}

int selectedMapping() { return (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }

void addMapping() {
    Mapping m;
    m.id = newMappingId();
    if (!editMapping(m, true)) return;
    g_cfg.mappings.push_back(m);
    g_cfg.save(configPath());
    fillList();
}

void editSelected() {
    int i = selectedMapping();
    if (i < 0) return;
    Mapping m = g_cfg.mappings[i];
    if (!editMapping(m, false)) return;
    if (m.category != g_cfg.mappings[i].category || m.name != g_cfg.mappings[i].name)
        if (MessageBoxW(g_wnd, L"Moving it to another tab or name copies it to the Switch again at the next sync (and removes the old copy). Go ahead?",
                        APP, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    g_cfg.mappings[i] = m;
    g_cfg.save(configPath());
    fillList();
}

void removeSelected() {
    int i = selectedMapping();
    if (i < 0) return;
    std::wstring msg = L"Stop syncing " + wide(g_cfg.mappings[i].name) +
                       L"?\n\nIts copy on the Switch is deleted at the next sync. Nothing on this PC is touched.";
    if (MessageBoxW(g_wnd, msg.c_str(), APP, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    g_cfg.mappings.erase(g_cfg.mappings.begin() + i);
    g_cfg.save(configPath());
    fillList();
}

void updateStatus() {
    Status st;
    std::string info;
    {
        std::lock_guard<std::mutex> l(g_mx);
        st = g_st;
    }
    std::wstring text;
    int pos = 0;
    if (g_busy) {
        std::string t = st.stage;
        if (st.bytes) {
            pos = (int)(st.bytesDone * 1000 / st.bytes);
            t += "  \xC2\xB7  " + formatBytes((double)st.bytesDone) + " of " + formatBytes((double)st.bytes);
        } else if (st.total) {
            pos = st.done * 1000 / st.total;
            t += "  \xC2\xB7  " + std::to_string(st.done) + " of " + std::to_string(st.total);
        }
        if (!st.item.empty()) t += "  \xC2\xB7  " + baseOf(st.item);
        text = wide(t);
    } else text = wide(st.idle);
    if (getText(g_status) != text) SetWindowTextW(g_status, text.c_str());
    SendMessageW(g_progress, PBM_SETPOS, pos, 0);

    std::wstring sw = g_card.root.empty()
                          ? L"SD card: not found (put it in a card reader, or start hekate's USB mass storage)"
                          : L"SD card: " + wide(g_card.label) + L"  ·  " + wide(formatBytes((double)g_card.freeBytes)) + L" free" +
                                (g_card.hasApp ? L"" : L"  ·  no MediaCenter.nro on it yet");
    if (getText(g_switch) != sw) SetWindowTextW(g_switch, sw.c_str());

    std::wstring tip = g_busy ? L"Media Center Sync: working" : L"Media Center Sync";
    if (g_busy && st.bytes) tip += L" " + std::to_wstring(st.bytesDone * 100 / st.bytes) + L"%";
    if (tip != g_tray.szTip) {
        g_tray.uFlags = NIF_TIP;
        wcsncpy(g_tray.szTip, tip.c_str(), 127);
        Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    }
}

void checkDevice() {
    std::vector<Card> cards = findCards();
    Card now = cards.empty() ? Card() : cards[0];
    bool was = !g_card.root.empty(), present = !now.root.empty();
    bool same = now.root == g_card.root;
    if (!g_busy) g_card = now;  // (free space changes during a sync: shown after)
    if (same) return;
    if (present) logLine("SD card found: " + now.label);
    else if (was) logLine("The SD card was removed");
    if (present && !g_busy && !g_cfg.mappings.empty() && (g_cfg.autoSync || g_syncWhenInserted)) startJob(true);
    updateStatus();
}

void layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int w = rc.right, m = S(14), x = m, y = m, cw = w - 2 * m, lh = S(20), bh = S(30), bw = S(104);
    MoveWindow(g_switch, x, y, cw, lh, TRUE);
    y += lh + S(12);
    MoveWindow(g_label, x, y, cw, lh, TRUE);
    y += lh + S(4);
    int listH = S(150);
    MoveWindow(g_list, x, y, cw, listH, TRUE);
    int c0 = (cw - S(24)) * 24 / 100, c1 = (cw - S(24)) * 14 / 100;
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 0, c0);
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1, c1);
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 2, cw - S(24) - c0 - c1);
    y += listH + S(8);
    MoveWindow(g_add, x, y, bw, bh, TRUE);
    MoveWindow(g_edit, x + bw + S(8), y, bw, bh, TRUE);
    MoveWindow(g_remove, x + 2 * (bw + S(8)), y, bw, bh, TRUE);
    MoveWindow(g_settings, x + cw - bw, y, bw, bh, TRUE);
    MoveWindow(g_fix, x + cw - 2 * bw - S(36) - S(8), y, bw + S(36), bh, TRUE);
    y += bh + S(16);
    MoveWindow(g_sync, x, y, S(120), S(34), TRUE);
    MoveWindow(g_status, x + S(132), y + S(8), cw - S(132), lh, TRUE);
    y += S(44);
    MoveWindow(g_progress, x, y, cw, S(8), TRUE);
    y += S(16);
    MoveWindow(g_auto, x, y, cw, S(22), TRUE);
    y += S(30);
    MoveWindow(g_log, x, y, cw, rc.bottom - y - m, TRUE);
}

void applyFont() {
    if (g_font) DeleteObject(g_font);
    if (g_bold) DeleteObject(g_bold);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {g_label, g_list, g_add, g_edit, g_remove, g_fix, g_settings, g_sync, g_auto, g_status, g_log})
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_switch, WM_SETFONT, (WPARAM)g_bold, TRUE);
}

void showMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Open");
    AppendMenuW(m, MF_STRING | (g_busy ? MF_GRAYED : 0), ID_TRAY_SYNC, L"Sync now");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"Quit");
    SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
}

void quit() {
    if (g_busy) {
        if (MessageBoxW(g_wnd, L"A sync is running. Stop it and quit?\n\nNothing breaks: the next sync carries on from where this one stopped.",
                        APP, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        stopJob();
    }
    if (g_worker.joinable()) g_worker.join();
    g_cfg.save(configPath());
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    DestroyWindow(g_wnd);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        g_dpi = GetDpiForWindow(h);
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
            return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, h, (HMENU)(INT_PTR)id, g_inst, nullptr);
        };
        g_switch = mk(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_SWITCH);
        g_label = mk(L"STATIC", L"Folders on the Switch", 0, 0);
        g_list = mk(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, ID_LIST, WS_EX_CLIENTEDGE);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = {L"Name", L"Tab", L"Folder on this PC"};
        for (int i = 0; i < 3; i++) {
            LVCOLUMNW c = {};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (LPWSTR)cols[i];
            c.cx = 100;
            SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
        }
        g_add = mk(L"BUTTON", L"Add…", WS_TABSTOP | BS_PUSHBUTTON, ID_ADD);
        g_edit = mk(L"BUTTON", L"Edit…", WS_TABSTOP | BS_PUSHBUTTON, ID_EDIT);
        g_remove = mk(L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON, ID_REMOVE);
        g_fix = mk(L"BUTTON", L"Fix shows/movies…", WS_TABSTOP | BS_PUSHBUTTON, ID_FIX);
        g_settings = mk(L"BUTTON", L"Settings…", WS_TABSTOP | BS_PUSHBUTTON, ID_SETTINGS);
        g_sync = mk(L"BUTTON", L"Sync now", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_SYNC);
        g_status = mk(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_STATUS);
        g_progress = mk(PROGRESS_CLASSW, L"", 0, ID_PROGRESS);
        g_auto = mk(L"BUTTON", L"Sync by itself when the Switch's SD card is put in", WS_TABSTOP | BS_AUTOCHECKBOX, ID_AUTO);
        SendMessageW(g_auto, BM_SETCHECK, g_cfg.autoSync ? BST_CHECKED : BST_UNCHECKED, 0);
        g_log = mk(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, ID_LOG, WS_EX_CLIENTEDGE);
        SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
        applyFont();
        layout();
        fillList();
        SetTimer(h, TIMER_UI, 200, nullptr);
        SetTimer(h, TIMER_DEVICE, 2000, nullptr);
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {S(560), S(520)}; return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyFont();
        layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == g_status) SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        if ((HWND)lp == g_switch) SetTextColor((HDC)wp, !g_card.root.empty() ? RGB(0x2E, 0x7D, 0x32) : GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->hwndFrom == g_list) {
            if (n->code == NM_DBLCLK && !g_busy) editSelected();
            else if (n->code == LVN_ITEMCHANGED) {
                bool sel = selectedMapping() >= 0;
                EnableWindow(g_edit, sel && !g_busy);
                EnableWindow(g_remove, sel && !g_busy);
            }
        }
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) SetTimer(h, TIMER_DEVICE, 300, nullptr);  // look again shortly
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ADD: if (!g_busy) addMapping(); break;
        case ID_EDIT: if (!g_busy) editSelected(); break;
        case ID_REMOVE: if (!g_busy) removeSelected(); break;
        case ID_FIX: if (!g_busy) fixMatches(); break;
        case ID_SETTINGS: if (!g_busy) editSettings(); break;
        case ID_SYNC:
            if (g_busy) stopJob();
            else startJob(true);
            break;
        case ID_AUTO:
            g_cfg.autoSync = SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_cfg.save(configPath());
            break;
        case ID_TRAY_OPEN: ShowWindow(h, SW_SHOW); SetForegroundWindow(h); break;
        case ID_TRAY_SYNC: startJob(true); break;
        case ID_TRAY_QUIT: quit(); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_UI) updateStatus();
        if (wp == TIMER_DEVICE) {
            SetTimer(h, TIMER_DEVICE, 2000, nullptr);
            checkDevice();
        }
        return 0;
    case WM_LOGLINE: {
        auto* s = (std::wstring*)lp;
        int n = (int)SendMessageW(g_log, LB_ADDSTRING, 0, (LPARAM)s->c_str());
        if (n > 2000) SendMessageW(g_log, LB_DELETESTRING, 0, 0);
        SendMessageW(g_log, LB_SETTOPINDEX, n, 0);
        delete s;
        return 0;
    }
    case WM_JOBDONE: {
        auto* s = (std::wstring*)lp;
        if (g_worker.joinable()) g_worker.join();
        g_busy = false;
        SetWindowTextW(g_sync, L"Sync now");
        for (HWND b : {g_add, g_fix, g_settings}) EnableWindow(b, TRUE);
        bool sel = selectedMapping() >= 0;
        EnableWindow(g_edit, sel);
        EnableWindow(g_remove, sel);
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_st.idle = utf8(*s);
        }
        bool ok = wp & 1, sent = wp & 2;
        if (!IsWindowVisible(h) && (sent || !ok)) notify(ok ? L"Sync finished" : L"Sync didn't finish", *s);
        delete s;
        updateStatus();
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_CLOSE:
        if (g_cfg.closeToTray) ShowWindow(h, SW_HIDE);  // lives on in the tray
        else quit();
        return 0;
    case WM_ENDSESSION:
        if (wp) { stopJob(); g_cfg.save(configPath()); }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"MediaCenterSync.Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CLASS_NAME, nullptr);
        if (other) { ShowWindow(other, SW_SHOW); SetForegroundWindow(other); }
        return 0;
    }
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    g_cfg.load(configPath());
    setLogListener([](const std::string& s) {
        if (!g_wnd) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf(stamp, 32, L"[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
        PostMessageW(g_wnd, WM_LOGLINE, 0, (LPARAM) new std::wstring(stamp + wide(s)));
    });

    g_iconBig = makeSyncIcon(GetSystemMetrics(SM_CXICON) * 2, 0xFF8A65);
    g_iconSmall = makeSyncIcon(GetSystemMetrics(SM_CXSMICON), 0xFF8A65);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = editorProc;
    wc.lpszClassName = EDITOR_CLASS;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    HWND h = CreateWindowExW(0, CLASS_NAME, APP, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(720, dpi, 96), MulDiv(640, dpi, 96),
                             nullptr, nullptr, inst, nullptr);
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = h;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = g_iconSmall;
    wcscpy(g_tray.szTip, APP);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    g_tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_tray);

    bool trayOnly = cmdLine && wcsstr(cmdLine, L"--tray");
    if (!trayOnly) ShowWindow(h, SW_SHOW);
    logLine("Media Center Sync started (data in " + dataDir() + ")");
    if (!findFfmpeg(g_cfg.ffmpegDir).ok()) logLine("ffmpeg.exe wasn't found: set its folder in Settings for video and music thumbnails");
    checkDevice();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    setLogListener(nullptr);
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
