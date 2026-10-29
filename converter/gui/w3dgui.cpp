// w3dgui.exe — one window for every converter run (stage 5.22, laid out again in 5.35h): everything is written to ONE
// folder that is copied to sdmc:/3ds/w3dproto/. It only starts w3dconv.exe (next to it) once per step and shows its
// output; the logic of the steps is in gui_core.h.
//
// 5.35h: the window takes the converter's output from a shared buffer on a timer. It used to post one window message per
// line: an "Everything" run prints ~40 000 lines, the burst at the end (checking the packs) overflowed the thread's message
// queue (10 000 messages), and the final "done" message was lost - the window kept "running" after w3dconv had finished.
// A step now also ends when the process ends, even if something else still holds the pipe.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cwchar>
#include <deque>
#include <string>
#include <vector>

#include "gui_core.h"

namespace {

enum {
    ID_DATA = 100, ID_DATA_BROWSE, ID_OUT, ID_OUT_BROWSE, ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION, ID_TX0, ID_TY0, ID_TX1, ID_TY1,
    ID_PACKWORLD, ID_CREATURES, ID_PLAYERS, ID_ITEMS, ID_TRANSPORTS, ID_GAMEOBJECTS, ID_DUNGEONS, ID_UI, ID_SOUND, ID_QUALITY, ID_EXTRA,
    ID_MODE_ALL, ID_MODE_SEL, ID_START, ID_CANCEL, ID_OPEN, ID_LOG, ID_STATUS, ID_STAGE, ID_PROGRESS, ID_LAB_TILES
};
constexpr UINT_PTR kTimer = 1;
constexpr size_t kLogLines = 600;   // lines kept in the window (the whole log is in log_*.txt)

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_bold = nullptr, g_mono = nullptr;
HANDLE g_thread = nullptr;
volatile LONG g_cancel = 0;
CRITICAL_SECTION g_cs;
HANDLE g_child = nullptr;   // the running w3dconv, for Cancel
std::wstring g_exeDir;

// shared with the worker thread (under g_cs)
std::deque<std::wstring> g_lines;
bool g_linesDirty = false;
std::wstring g_status, g_stage;
bool g_statusDirty = false;
bool g_finished = false;    // the worker is done; g_failed = steps with errors, -1 = cancelled
int g_failed = 0;
ULONGLONG g_startTick = 0;

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring getText(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    int n = GetWindowTextLengthW(h);
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(h, &w[0], n + 1);
    w.resize((size_t)n);
    return w;
}
void setText(int id, const std::wstring& s) { SetWindowTextW(GetDlgItem(g_wnd, id), s.c_str()); }
bool checked(int id) { return SendDlgItemMessageW(g_wnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void setChecked(int id, bool on) { SendDlgItemMessageW(g_wnd, id, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
int getInt(int id, int def) {
    std::wstring t = getText(id);
    return t.empty() ? def : _wtoi(t.c_str());
}

std::wstring settingsPath() { return g_exeDir + L"w3dgui.ini"; }

w3gui::Settings readUi() {
    w3gui::Settings s;
    s.data = narrow(getText(ID_DATA));
    s.out = narrow(getText(ID_OUT));
    s.map = narrow(getText(ID_MAP));
    s.world = checked(ID_RADIO_WORLD);
    s.tx0 = getInt(ID_TX0, 30); s.ty0 = getInt(ID_TY0, 46); s.tx1 = getInt(ID_TX1, 34); s.ty1 = getInt(ID_TY1, 50);
    s.packWorld = checked(ID_PACKWORLD);
    s.creatures = checked(ID_CREATURES); s.players = checked(ID_PLAYERS); s.items = checked(ID_ITEMS);
    s.transports = checked(ID_TRANSPORTS); s.gameobjects = checked(ID_GAMEOBJECTS); s.dungeons = checked(ID_DUNGEONS); s.ui = checked(ID_UI); s.sound = checked(ID_SOUND);
    s.everything = checked(ID_MODE_ALL);
    s.quality = (int)SendDlgItemMessageW(g_wnd, ID_QUALITY, CB_GETCURSEL, 0, 0);
    if (s.quality < 0) s.quality = 0;
    s.extra = narrow(getText(ID_EXTRA));
    return s;
}

void writeUi(const w3gui::Settings& s) {
    setText(ID_DATA, widen(s.data));
    setText(ID_OUT, widen(s.out));
    setText(ID_MAP, widen(s.map));
    setChecked(ID_RADIO_WORLD, s.world);
    setChecked(ID_RADIO_REGION, !s.world);
    setText(ID_TX0, std::to_wstring(s.tx0)); setText(ID_TY0, std::to_wstring(s.ty0));
    setText(ID_TX1, std::to_wstring(s.tx1)); setText(ID_TY1, std::to_wstring(s.ty1));
    setChecked(ID_PACKWORLD, s.packWorld);
    setChecked(ID_CREATURES, s.creatures); setChecked(ID_PLAYERS, s.players); setChecked(ID_ITEMS, s.items);
    setChecked(ID_TRANSPORTS, s.transports); setChecked(ID_GAMEOBJECTS, s.gameobjects); setChecked(ID_DUNGEONS, s.dungeons); setChecked(ID_UI, s.ui); setChecked(ID_SOUND, s.sound);
    setChecked(ID_MODE_ALL, s.everything);
    setChecked(ID_MODE_SEL, !s.everything);
    SendDlgItemMessageW(g_wnd, ID_QUALITY, CB_SETCURSEL, (WPARAM)s.quality, 0);
    setText(ID_EXTRA, widen(s.extra));
}

void saveSettings() {
    std::string text = w3gui::serialize(readUi());
    HANDLE f = CreateFileW(settingsPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, text.data(), (DWORD)text.size(), &n, nullptr);
    CloseHandle(f);
}

w3gui::Settings loadSettings() {
    HANDLE f = CreateFileW(settingsPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {   // first start: "Everything" is what most people want
        w3gui::Settings s;
        s.everything = true;
        return s;
    }
    std::string text(1 << 16, '\0');
    DWORD n = 0;
    ReadFile(f, &text[0], (DWORD)text.size(), &n, nullptr);
    CloseHandle(f);
    text.resize(n);
    return w3gui::parse(text);
}

// ---- output of the worker: into the shared buffer, shown by the timer
void addLine(const std::wstring& s) {
    EnterCriticalSection(&g_cs);
    g_lines.push_back(s);
    while (g_lines.size() > kLogLines) g_lines.pop_front();
    g_linesDirty = true;
    // the stage: the converter's section headers ("== map 1 (Kalimdor): 1018 tiles", "== creatures, ...")
    if (s.size() > 3 && s.compare(0, 3, L"== ") == 0) { g_stage = s.substr(3); g_statusDirty = true; }
    LeaveCriticalSection(&g_cs);
}
void setStatus(const std::wstring& s) {
    EnterCriticalSection(&g_cs);
    g_status = s;
    g_statusDirty = true;
    LeaveCriticalSection(&g_cs);
}

bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool nonAscii(const std::string& s) {
    for (unsigned char c : s) if (c > 127) return true;
    return false;
}

// Runs one w3dconv.exe, copies its output to the window and to the step's log file. Returns its exit code (-1: cannot start).
int runStep(const std::wstring& exe, const w3gui::Step& st, const std::wstring& outDir) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = widen(w3gui::commandLine(narrow(exe), st));
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                             g_exeDir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    EnterCriticalSection(&g_cs);
    g_child = pi.hProcess;
    LeaveCriticalSection(&g_cs);
    HANDLE logf = CreateFileW((outDir + L"\\" + widen(st.logFile)).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string pending;
    char chunk[8192];
    auto take = [&](DWORD n) {
        if (logf != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(logf, chunk, n, &w, nullptr); }
        pending.append(chunk, n);
        size_t at;
        while ((at = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, at);
            pending.erase(0, at + 1);
            if (!line.empty()) addLine(widen(line));
        }
    };
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // the writer closed it: everything read
        if (avail) {
            DWORD n = 0;
            if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof chunk), &n, nullptr) || !n) break;
            take(n);
            continue;
        }
        // nothing to read: the step is over when the process is, even if something else still holds the pipe
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                DWORD n = 0;
                if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof chunk), &n, nullptr) || !n) break;
                take(n);
            }
            break;
        }
    }
    if (!pending.empty()) addLine(widen(pending));
    CloseHandle(rd);
    if (logf != INVALID_HANDLE_VALUE) CloseHandle(logf);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    EnterCriticalSection(&g_cs);
    g_child = nullptr;
    LeaveCriticalSection(&g_cs);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

struct Job { w3gui::Settings s; };

DWORD WINAPI worker(LPVOID p) {
    Job* job = (Job*)p;
    const w3gui::Settings& s = job->s;
    std::wstring exe = g_exeDir + L"w3dconv.exe";
    std::wstring outDir = widen(s.out);
    SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr);
    std::vector<w3gui::Step> steps = w3gui::buildSteps(s);
    int failed = 0;
    for (size_t i = 0; i < steps.size() && !g_cancel; ++i) {
        addLine(L"");
        addLine(L"=== Step " + std::to_wstring(i + 1) + L" of " + std::to_wstring(steps.size()) + L": " + widen(steps[i].title) + L" ===");
        setStatus(L"Step " + std::to_wstring(i + 1) + L" of " + std::to_wstring(steps.size()) + L": " + widen(steps[i].title));
        int rc = runStep(exe, steps[i], outDir);
        if (g_cancel) break;
        if (rc != 0) {
            ++failed;
            wchar_t hex[16];
            std::swprintf(hex, 16, L"0x%08X", (unsigned)rc);
            const wchar_t* what = (unsigned)rc == 0xC0000005u ? L" = access violation (the converter crashed)"
                                  : (unsigned)rc == 0xC00000FDu ? L" = stack overflow"
                                  : (unsigned)rc == 0xC0000374u ? L" = heap corruption"
                                  : (unsigned)rc == 0xC0000409u ? L" = fast-fail / stack buffer overrun"
                                  : rc == 3 ? L" = aborted (see the FATAL line at the end of the log)"
                                  : rc == 4 ? L" = a pack did not pass its check (see INVALID in the log)"
                                  : rc == 5 ? L" = cannot write the spill file (disk full or file over 4 GB on FAT32)" : L"";
            addLine(L"!!! The step ended with code " + std::to_wstring(rc) + L" (" + hex + what + L"), log: " + widen(steps[i].logFile));
            addLine(L"!!! Look at the LAST lines of that log: a line starting with FATAL says what was being converted.");
        } else {
            addLine(L"Step done.");
        }
    }
    EnterCriticalSection(&g_cs);
    g_failed = g_cancel ? -1 : failed;
    g_finished = true;
    LeaveCriticalSection(&g_cs);
    delete job;
    return 0;
}

// "Everything" or "Selected parts": the choices of the other mode are greyed out. Creatures, players and ships are always
// in "Everything"; interface, sound, armour and game objects can be left out of it.
void applyMode() {
    const bool all = checked(ID_MODE_ALL);
    for (int id : {ID_PACKWORLD, ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION, ID_TX0, ID_TY0, ID_TX1, ID_TY1, ID_LAB_TILES, ID_DUNGEONS,
                   ID_CREATURES, ID_PLAYERS, ID_TRANSPORTS})
        EnableWindow(GetDlgItem(g_wnd, id), !all);
    if (!all) {
        const bool world = checked(ID_PACKWORLD), region = checked(ID_RADIO_REGION);
        for (int id : {ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION}) EnableWindow(GetDlgItem(g_wnd, id), world);
        for (int id : {ID_TX0, ID_TY0, ID_TX1, ID_TY1, ID_LAB_TILES}) EnableWindow(GetDlgItem(g_wnd, id), world && region);
    }
}

void setRunning(bool running) {
    for (int id : {ID_START, ID_DATA, ID_DATA_BROWSE, ID_OUT, ID_OUT_BROWSE, ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION, ID_TX0, ID_TY0, ID_TX1, ID_TY1,
                   ID_LAB_TILES, ID_PACKWORLD, ID_CREATURES, ID_PLAYERS, ID_ITEMS, ID_TRANSPORTS, ID_GAMEOBJECTS, ID_DUNGEONS, ID_UI, ID_SOUND,
                   ID_QUALITY, ID_EXTRA, ID_MODE_ALL, ID_MODE_SEL})
        EnableWindow(GetDlgItem(g_wnd, id), !running);
    if (!running) applyMode();
    EnableWindow(GetDlgItem(g_wnd, ID_CANCEL), running);
    HWND bar = GetDlgItem(g_wnd, ID_PROGRESS);
    LONG_PTR st = GetWindowLongPtrW(bar, GWL_STYLE);
    SetWindowLongPtrW(bar, GWL_STYLE, running ? (st | PBS_MARQUEE) : (st & ~(LONG_PTR)PBS_MARQUEE));
    SendMessageW(bar, PBM_SETMARQUEE, running ? TRUE : FALSE, 30);
    if (!running) SendMessageW(bar, PBM_SETPOS, 0, 0);
}

std::wstring elapsed() {
    if (!g_startTick) return L"";
    const ULONGLONG s = (GetTickCount64() - g_startTick) / 1000;
    wchar_t b[32];
    if (s >= 3600) std::swprintf(b, 32, L"%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
    else std::swprintf(b, 32, L"%llu:%02llu", s / 60, s % 60);
    return b;
}

// The timer: the newest lines into the log box, the status line, the end of the run.
void tick() {
    std::wstring text, status, stage;
    bool linesDirty, statusDirty, finished;
    int failed;
    EnterCriticalSection(&g_cs);
    linesDirty = g_linesDirty;
    if (linesDirty) {
        for (const std::wstring& l : g_lines) { text += l; text += L"\r\n"; }
        g_linesDirty = false;
    }
    statusDirty = g_statusDirty;
    g_statusDirty = false;
    status = g_status;
    stage = g_stage;
    finished = g_finished;
    failed = g_failed;
    g_finished = false;
    LeaveCriticalSection(&g_cs);
    if (linesDirty) {
        HWND h = GetDlgItem(g_wnd, ID_LOG);
        SendMessageW(h, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(h, text.c_str());
        SendMessageW(h, EM_SETSEL, (WPARAM)text.size(), (LPARAM)text.size());
        SendMessageW(h, EM_SCROLLCARET, 0, 0);
        SendMessageW(h, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(h, nullptr, TRUE);
    }
    if (g_thread) {
        (void)statusDirty;
        setText(ID_STATUS, status + L"   (" + elapsed() + L")");
        setText(ID_STAGE, stage.empty() ? L"" : L"Now: " + stage);
    }
    if (finished) {
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
        setRunning(false);
        const std::wstring out = getText(ID_OUT), took = elapsed();
        g_startTick = 0;
        if (failed < 0) {
            setText(ID_STATUS, L"Cancelled after " + took + L".");
            addLine(L"Cancelled.");
        } else if (failed == 0) {
            setText(ID_STATUS, L"Done in " + took + L". Copy the contents of the output folder to sdmc:/3ds/w3dproto/");
            addLine(L"All done in " + took + L". Copy the contents of " + out + L" to sdmc:/3ds/w3dproto/ on the SD card.");
            SendDlgItemMessageW(g_wnd, ID_PROGRESS, PBM_SETPOS, 100, 0);
        } else {
            setText(ID_STATUS, L"Finished with errors after " + took + L": see the log (log_*.txt in the output folder).");
            addLine(L"Steps with errors: " + std::to_wstring(failed) + L". The logs are in " + out);
        }
        setText(ID_STAGE, L"");
        FlashWindow(g_wnd, TRUE);
        tick();   // the last lines
    }
}

void startRun() {
    w3gui::Settings s = readUi();
    std::string bad = w3gui::validate(s);
    if (bad.empty() && !dirExists(widen(s.data))) bad = "Data folder not found: " + s.data;
    if (bad.empty() && GetFileAttributesW((g_exeDir + L"w3dconv.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        bad = "w3dconv.exe is not next to w3dgui.exe.";
    if (!bad.empty()) {
        MessageBoxW(g_wnd, widen(bad).c_str(), L"WoW 3DS Converter", MB_ICONWARNING);
        return;
    }
    saveSettings();
    EnterCriticalSection(&g_cs);
    g_lines.clear();
    g_linesDirty = true;
    g_stage.clear();
    g_finished = false;
    LeaveCriticalSection(&g_cs);
    if (nonAscii(s.data) || nonAscii(s.out))
        addLine(L"Warning: the paths contain non-Latin letters, w3dconv.exe may fail to open them. Prefer paths like C:\\wow\\Data and C:\\w3dproto.");
    if (s.everything) addLine(L"Everything: every map, dungeon and raid. This takes hours and several GB of disk space.");
    else if (s.world && s.packWorld) addLine(L"Whole map: this takes a long time. A pack over 4 GB cannot be written (FAT32 and format limit).");
    g_cancel = 0;
    g_startTick = GetTickCount64();
    setRunning(true);
    g_thread = CreateThread(nullptr, 0, worker, new Job{s}, 0, nullptr);
}

std::wstring browse(HWND owner, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return path;
}

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0, HFONT f = nullptr) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}
HWND group(const wchar_t* title, int x, int y, int w, int h) { return mk(L"BUTTON", title, BS_GROUPBOX, x, y, w, h, 0, 0, g_bold); }
HWND check(const wchar_t* t, int x, int y, int w, int id) { return mk(L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id); }
HWND label(const wchar_t* t, int x, int y, int w, int id = 0, DWORD style = 0) { return mk(L"STATIC", t, style, x, y, w, 18, id); }

constexpr int kW = 660;   // client width
constexpr int kM = 14;    // margin
constexpr int kIn = 26;   // inside a group

void createControls() {
    const DWORD edit = WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER;
    const int gw = kW - 2 * kM;
    int y = 10;
    // ---- folders
    group(L"Folders", kM, y, gw, 118);
    label(L"WoW 1.12 Data folder (the one with the .MPQ files)", kM + 12, y + 22, 400);
    mk(L"EDIT", L"", edit, kM + 12, y + 40, gw - 130, 24, ID_DATA, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", WS_TABSTOP, kM + gw - 108, y + 40, 96, 24, ID_DATA_BROWSE);
    label(L"Output folder (copy its contents to sdmc:/3ds/w3dproto/ on the SD card)", kM + 12, y + 70, 500);
    mk(L"EDIT", L"", edit, kM + 12, y + 88, gw - 130, 24, ID_OUT, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", WS_TABSTOP, kM + gw - 108, y + 88, 96, 24, ID_OUT_BROWSE);
    y += 128;
    // ---- what
    group(L"What to convert", kM, y, gw, 176);
    mk(L"BUTTON", L"Everything (recommended): every map, dungeon and raid with one shared asset pool",
       BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kM + 12, y + 22, gw - 24, 20, ID_MODE_ALL);
    label(L"map<ID>.w3dp for each map + common*.w3dp. Takes hours and several GB.", kM + 32, y + 42, gw - 50);
    mk(L"BUTTON", L"Selected parts only", BS_AUTORADIOBUTTON | WS_TABSTOP, kM + 12, y + 66, 200, 20, ID_MODE_SEL);
    const int sx = kM + 32;
    check(L"World map:", sx, y + 92, 96, ID_PACKWORLD);
    HWND map = mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, sx + 100, y + 90, 130, 120, ID_MAP);
    for (const wchar_t* m : {L"Azeroth", L"Kalimdor"}) SendMessageW(map, CB_ADDSTRING, 0, (LPARAM)m);
    mk(L"BUTTON", L"whole map", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, sx + 246, y + 92, 96, 20, ID_RADIO_WORLD);
    mk(L"BUTTON", L"tiles:", BS_AUTORADIOBUTTON | WS_TABSTOP, sx + 346, y + 92, 56, 20, ID_RADIO_REGION);
    const int ids[] = {ID_TX0, ID_TY0, ID_TX1, ID_TY1};
    const int tx[] = {sx + 404, sx + 446, sx + 502, sx + 544};
    for (int i = 0; i < 4; ++i) mk(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | WS_BORDER, tx[i], y + 90, 36, 22, ids[i], WS_EX_CLIENTEDGE);
    label(L"to", sx + 485, y + 93, 16, ID_LAB_TILES);
    label(L"Azeroth = Eastern Kingdoms (test.w3dp), Kalimdor = map1.w3dp. Tiles: x y from - x y to, 0..63.", sx + 18, y + 116, gw - 60);
    check(L"Dungeons and raids (one map<ID>.w3dp each)", sx, y + 142, 320, ID_DUNGEONS);
    y += 186;
    // ---- content
    group(L"Content", kM, y, gw, 100);
    const int c1 = kM + 12, c2 = kM + 12 + gw / 3, c3 = kM + 12 + 2 * gw / 3;
    check(L"Interface and texts", c1, y + 22, 190, ID_UI);
    check(L"Sound and music", c2, y + 22, 190, ID_SOUND);
    check(L"Armor and weapons", c3, y + 22, 190, ID_ITEMS);
    check(L"Creatures", c1, y + 46, 190, ID_CREATURES);
    check(L"Players", c2, y + 46, 190, ID_PLAYERS);
    check(L"Ships and zeppelins", c3, y + 46, 190, ID_TRANSPORTS);
    check(L"All game objects (chests, lamps, decorations; large)", c1, y + 70, 360, ID_GAMEOBJECTS);
    y += 110;
    // ---- options
    group(L"Options", kM, y, gw, 58);
    label(L"Texture quality:", kM + 12, y + 26, 100);
    HWND q = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kM + 112, y + 22, 110, 100, ID_QUALITY);
    for (const wchar_t* m : {L"fast", L"better", L"best"}) SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)m);
    label(L"Extra options:", kM + 244, y + 26, 88);
    mk(L"EDIT", L"", edit, kM + 334, y + 22, gw - 346, 24, ID_EXTRA, WS_EX_CLIENTEDGE);
    y += 68;
    // ---- run
    mk(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, kM, y, 130, 32, ID_START, 0, g_bold);
    mk(L"BUTTON", L"Cancel", WS_TABSTOP | WS_DISABLED, kM + 138, y, 100, 32, ID_CANCEL);
    mk(L"BUTTON", L"Open the output folder", WS_TABSTOP, kM + gw - 190, y, 190, 32, ID_OPEN);
    y += 40;
    mk(L"msctls_progress32", L"", PBS_SMOOTH, kM, y, gw, 12, ID_PROGRESS);
    y += 18;
    mk(L"STATIC", L"Ready.", SS_ENDELLIPSIS, kM, y, gw, 18, ID_STATUS, 0, g_bold);
    y += 20;
    mk(L"STATIC", L"", SS_ENDELLIPSIS, kM, y, gw, 18, ID_STAGE);
    y += 22;
    HWND logBox = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, kM, y, gw, 150, ID_LOG,
                     WS_EX_CLIENTEDGE, g_mono);
    SendMessageW(logBox, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
}
constexpr int kH = 10 + 128 + 186 + 110 + 68 + 40 + 18 + 20 + 22 + 150 + 14;   // client height

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            g_wnd = h;
            createControls();
            writeUi(loadSettings());
            applyMode();
            SetTimer(h, kTimer, 250, nullptr);
            return 0;
        case WM_TIMER:
            if (w == kTimer) tick();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_DATA_BROWSE: {
                    std::wstring p = browse(h, L"Data folder of the WoW 1.12 client");
                    if (!p.empty()) setText(ID_DATA, p);
                    return 0;
                }
                case ID_OUT_BROWSE: {
                    std::wstring p = browse(h, L"Output folder");
                    if (!p.empty()) setText(ID_OUT, p);
                    return 0;
                }
                case ID_START: startRun(); return 0;
                case ID_MODE_ALL: case ID_MODE_SEL: case ID_PACKWORLD: case ID_RADIO_WORLD: case ID_RADIO_REGION:
                    if (!g_thread) applyMode();
                    return 0;
                case ID_CANCEL:
                    InterlockedExchange(&g_cancel, 1);
                    EnterCriticalSection(&g_cs);
                    if (g_child) TerminateProcess(g_child, 1);
                    LeaveCriticalSection(&g_cs);
                    return 0;
                case ID_OPEN: {
                    std::wstring o = getText(ID_OUT);
                    if (!o.empty() && dirExists(o)) ShellExecuteW(h, L"open", o.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            if (g_thread) {
                if (MessageBoxW(h, L"A conversion is running. Cancel it and exit?", L"WoW 3DS Converter", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                InterlockedExchange(&g_cancel, 1);
                EnterCriticalSection(&g_cs);
                if (g_child) TerminateProcess(g_child, 1);
                LeaveCriticalSection(&g_cs);
                WaitForSingleObject(g_thread, 5000);
            }
            saveSettings();
            KillTimer(h, kTimer);
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    InitializeCriticalSection(&g_cs);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_exeDir = path;
    g_exeDir.resize(g_exeDir.find_last_of(L'\\') + 1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);
    LOGFONTW mono = ncm.lfMessageFont;
    mono.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy(mono.lfFaceName, L"Consolas");
    g_mono = CreateFontIndirectW(&mono);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"w3dgui";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{0, 0, kW, kH};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND w = CreateWindowW(L"w3dgui", L"WoW 3DS Converter", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
