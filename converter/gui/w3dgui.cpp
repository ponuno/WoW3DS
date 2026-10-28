// w3dgui.exe — one window for every converter run (stage 5.22): the world pack (any tile range or the whole map),
// the interface pack, sound, texts; everything is written to ONE folder that is copied to sdmc:/3ds/w3dproto/.
// It only starts w3dconv.exe (next to it) once per step and shows its output; the logic is in gui_core.h.
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
#include <string>
#include <vector>

#include "gui_core.h"

namespace {

enum {
    ID_DATA = 100, ID_DATA_BROWSE, ID_OUT, ID_OUT_BROWSE, ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION, ID_TX0, ID_TY0, ID_TX1, ID_TY1,
    ID_PACKWORLD, ID_CREATURES, ID_PLAYERS, ID_ITEMS, ID_TRANSPORTS, ID_GAMEOBJECTS, ID_DUNGEONS, ID_UI, ID_SOUND, ID_QUALITY, ID_EXTRA, ID_EVERYTHING,
    ID_START, ID_CANCEL, ID_OPEN, ID_LOG, ID_STATUS, ID_PROGRESS
};
constexpr UINT WM_LOGLINE = WM_APP + 1, WM_STEPS_DONE = WM_APP + 2, WM_STATUS = WM_APP + 3;

HWND g_wnd = nullptr;
HFONT g_font = nullptr;
HANDLE g_thread = nullptr;
volatile LONG g_cancel = 0;
CRITICAL_SECTION g_cs;
HANDLE g_child = nullptr;   // the running w3dconv, for Cancel
std::wstring g_exeDir;

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
    s.everything = checked(ID_EVERYTHING);
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
    setChecked(ID_EVERYTHING, s.everything);
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
    if (f == INVALID_HANDLE_VALUE) return w3gui::Settings{};
    std::string text(1 << 16, '\0');
    DWORD n = 0;
    ReadFile(f, &text[0], (DWORD)text.size(), &n, nullptr);
    CloseHandle(f);
    text.resize(n);
    return w3gui::parse(text);
}

void postLine(const std::wstring& s) { PostMessageW(g_wnd, WM_LOGLINE, 0, (LPARAM) new std::wstring(s)); }
void postStatus(const std::wstring& s) { PostMessageW(g_wnd, WM_STATUS, 0, (LPARAM) new std::wstring(s)); }

void appendLog(const std::wstring& line) {
    HWND h = GetDlgItem(g_wnd, ID_LOG);
    if (GetWindowTextLengthW(h) > 400000) {   // keep the box small: drop the first half
        SendMessageW(h, EM_SETSEL, 0, 200000);
        SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    SendMessageW(h, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    std::wstring t = line + L"\r\n";
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
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
    char chunk[4096];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
        if (logf != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(logf, chunk, n, &w, nullptr); }
        pending.append(chunk, n);
        size_t at;
        while ((at = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, at);
            pending.erase(0, at + 1);
            if (!line.empty()) postLine(widen(line));
        }
    }
    if (!pending.empty()) postLine(widen(pending));
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
        postLine(L"");
        postLine(L"=== Step " + std::to_wstring(i + 1) + L" of " + std::to_wstring(steps.size()) + L": " + widen(steps[i].title) + L" ===");
        postStatus(L"Step " + std::to_wstring(i + 1) + L"/" + std::to_wstring(steps.size()) + L": " + widen(steps[i].title));
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
                                  : rc == 5 ? L" = cannot write the spill file (disk full or file over 4 GB on FAT32)" : L"";
            postLine(L"!!! The step ended with code " + std::to_wstring(rc) + L" (" + hex + what + L"), log: " + widen(steps[i].logFile));
            postLine(L"!!! Look at the LAST lines of that log: a line starting with FATAL says what was being converted.");
        } else {
            postLine(L"Done.");
        }
    }
    PostMessageW(g_wnd, WM_STEPS_DONE, (WPARAM)(g_cancel ? -1 : failed), 0);
    delete job;
    return 0;
}

// "Everything" converts every map with creatures, players and ships: the per-map choices are greyed out.
void applyEverything() {
    const bool all = SendMessageW(GetDlgItem(g_wnd, ID_EVERYTHING), BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (int id : {ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION, ID_TX0, ID_TY0, ID_TX1, ID_TY1, ID_PACKWORLD, ID_CREATURES, ID_PLAYERS,
                   ID_TRANSPORTS, ID_DUNGEONS})
        EnableWindow(GetDlgItem(g_wnd, id), !all);
}

void setRunning(bool running) {
    for (int id : {ID_START, ID_DATA, ID_DATA_BROWSE, ID_OUT, ID_OUT_BROWSE, ID_MAP, ID_RADIO_WORLD, ID_RADIO_REGION, ID_TX0, ID_TY0, ID_TX1, ID_TY1,
                   ID_PACKWORLD, ID_CREATURES, ID_PLAYERS, ID_ITEMS, ID_TRANSPORTS, ID_GAMEOBJECTS, ID_DUNGEONS, ID_UI, ID_SOUND, ID_QUALITY, ID_EXTRA,
                   ID_EVERYTHING})
        EnableWindow(GetDlgItem(g_wnd, id), !running);
    if (!running) applyEverything();
    EnableWindow(GetDlgItem(g_wnd, ID_CANCEL), running);
    HWND bar = GetDlgItem(g_wnd, ID_PROGRESS);
    LONG_PTR st = GetWindowLongPtrW(bar, GWL_STYLE);
    SetWindowLongPtrW(bar, GWL_STYLE, running ? (st | PBS_MARQUEE) : (st & ~(LONG_PTR)PBS_MARQUEE));
    SendMessageW(bar, PBM_SETMARQUEE, running ? TRUE : FALSE, 30);
}

void startRun() {
    w3gui::Settings s = readUi();
    std::string bad = w3gui::validate(s);
    if (bad.empty() && !dirExists(widen(s.data))) bad = "Data folder not found: " + s.data;
    if (bad.empty() && GetFileAttributesW((g_exeDir + L"w3dconv.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        bad = "w3dconv.exe is not next to w3dgui.exe.";
    if (!bad.empty()) {
        MessageBoxW(g_wnd, widen(bad).c_str(), L"w3dgui", MB_ICONWARNING);
        return;
    }
    saveSettings();
    SetWindowTextW(GetDlgItem(g_wnd, ID_LOG), L"");
    if (nonAscii(s.data) || nonAscii(s.out))
        appendLog(L"Warning: the paths contain non-Latin letters, w3dconv.exe may fail to open them. Prefer paths like C:\\wow\\Data and C:\\w3dproto.");
    if (s.world && s.packWorld)
        appendLog(L"Whole world: this takes hours and a lot of disk space. A pack over 4 GB cannot be written (FAT32 and format limit).");
    g_cancel = 0;
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

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void createControls() {
    const DWORD edit = WS_TABSTOP, ro = ES_AUTOHSCROLL;
    int y = 12;
    mk(L"STATIC", L"Data folder of the WoW 1.12 client (the one with the .MPQ files):", 0, 12, y, 560, 18, 0);
    y += 20;
    mk(L"EDIT", L"", edit | ro | WS_BORDER, 12, y, 480, 24, ID_DATA, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", WS_TABSTOP, 500, y, 90, 24, ID_DATA_BROWSE);
    y += 34;
    mk(L"STATIC", L"Output folder (copy it to sdmc:/3ds/w3dproto/, or pick the folder on the SD card directly):", 0, 12, y, 580, 18, 0);
    y += 20;
    mk(L"EDIT", L"", edit | ro | WS_BORDER, 12, y, 480, 24, ID_OUT, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", WS_TABSTOP, 500, y, 90, 24, ID_OUT_BROWSE);
    y += 36;
    mk(L"BUTTON", L"World (Azeroth: test.w3dp, Kalimdor: map1.w3dp)", BS_GROUPBOX, 12, y, 578, 138, 0);
    mk(L"BUTTON", L"Build the world pack", BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 20, 160, 20, ID_PACKWORLD);
    mk(L"STATIC", L"Map:", 0, 200, y + 22, 50, 18, 0);
    HWND map = mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, 252, y + 18, 150, 120, ID_MAP);
    for (const wchar_t* m : {L"Azeroth", L"Kalimdor"}) SendMessageW(map, CB_ADDSTRING, 0, (LPARAM)m);
    mk(L"STATIC", L"Textures:", 0, 420, y + 22, 64, 18, 0);
    HWND q = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 486, y + 18, 94, 100, ID_QUALITY);
    for (const wchar_t* m : {L"fast", L"better", L"best"}) SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)m);
    mk(L"BUTTON", L"Whole world (all tiles 0..63)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 24, y + 46, 220, 20, ID_RADIO_WORLD);
    mk(L"BUTTON", L"Tile region:", BS_AUTORADIOBUTTON | WS_TABSTOP, 24, y + 68, 120, 20, ID_RADIO_REGION);
    int x = 150;
    const wchar_t* lab[] = {L"x from", L"y from", L"x to", L"y to"};
    const int ids[] = {ID_TX0, ID_TY0, ID_TX1, ID_TY1};
    for (int i = 0; i < 4; ++i) {
        mk(L"STATIC", lab[i], 0, x, y + 71, 36, 18, 0);
        mk(L"EDIT", L"", edit | ES_NUMBER | WS_BORDER, x + 38, y + 68, 40, 22, ids[i], WS_EX_CLIENTEDGE);
        x += 108;
    }
    mk(L"BUTTON", L"Creatures", BS_AUTOCHECKBOX | WS_TABSTOP, 24, y + 96, 78, 20, ID_CREATURES);
    mk(L"BUTTON", L"Players", BS_AUTOCHECKBOX | WS_TABSTOP, 104, y + 96, 66, 20, ID_PLAYERS);
    mk(L"BUTTON", L"Armor, weapons", BS_AUTOCHECKBOX | WS_TABSTOP, 172, y + 96, 112, 20, ID_ITEMS);
    mk(L"BUTTON", L"Ships, zeppelins", BS_AUTOCHECKBOX | WS_TABSTOP, 286, y + 96, 112, 20, ID_TRANSPORTS);
    mk(L"BUTTON", L"All game objects (big)", BS_AUTOCHECKBOX | WS_TABSTOP, 402, y + 96, 180, 20, ID_GAMEOBJECTS);
    y += 146;
    mk(L"BUTTON", L"Interface and texts (ui.w3dp, spells, talents, taxi ...)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 400, 20, ID_UI);
    mk(L"BUTTON", L"Sound and music (folder sound)", BS_AUTOCHECKBOX | WS_TABSTOP, 420, y, 170, 20, ID_SOUND);
    y += 24;
    mk(L"BUTTON", L"Dungeons and raids (one map<ID>.w3dp each; slow, with Creatures/Players it is big)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 578, 20, ID_DUNGEONS);
    y += 24;
    mk(L"BUTTON", L"EVERYTHING: all maps and dungeons, one shared asset pool (map<ID>.w3dp + common*.w3dp; hours, ~GBs)",
       BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 578, 20, ID_EVERYTHING);
    y += 28;
    mk(L"STATIC", L"Extra (e.g. --gameobjects pumpkin):", 0, 12, y + 3, 230, 18, 0);
    mk(L"EDIT", L"", edit | ro | WS_BORDER, 244, y, 346, 24, ID_EXTRA, WS_EX_CLIENTEDGE);
    y += 36;
    mk(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, y, 120, 30, ID_START);
    mk(L"BUTTON", L"Cancel", WS_TABSTOP | WS_DISABLED, 140, y, 100, 30, ID_CANCEL);
    mk(L"BUTTON", L"Open the output folder", WS_TABSTOP, 392, y, 198, 30, ID_OPEN);
    y += 38;
    mk(L"msctls_progress32", L"", PBS_SMOOTH, 12, y, 578, 10, ID_PROGRESS);
    y += 14;
    mk(L"STATIC", L"Ready.", SS_ENDELLIPSIS, 12, y, 578, 18, ID_STATUS);
    y += 22;
    HWND logBox = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 12, y, 578, 180, ID_LOG, WS_EX_CLIENTEDGE);
    // An EDIT control stops accepting text after 32 KB unless told otherwise: the window used to freeze a few hundred lines
    // into the run (the screenshot of a failed run ended in the first map) and never showed the final "step ended with code".
    SendMessageW(logBox, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            g_wnd = h;
            createControls();
            writeUi(loadSettings());
            applyEverything();
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
                case ID_EVERYTHING: applyEverything(); return 0;
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
        case WM_LOGLINE: {
            std::wstring* s = (std::wstring*)l;
            appendLog(*s);
            delete s;
            return 0;
        }
        case WM_STATUS: {
            std::wstring* s = (std::wstring*)l;
            setText(ID_STATUS, *s);
            delete s;
            return 0;
        }
        case WM_STEPS_DONE: {
            if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
            setRunning(false);
            int failed = (int)w;
            std::wstring out = getText(ID_OUT);
            if (failed < 0) {
                setText(ID_STATUS, L"Cancelled.");
                appendLog(L"\r\nCancelled.");
            } else if (failed == 0) {
                setText(ID_STATUS, L"Done. Copy the output folder to sdmc:/3ds/w3dproto/");
                appendLog(L"\r\nAll done. Copy the contents of " + out + L" to sdmc:/3ds/w3dproto/ on the SD card.");
            } else {
                setText(ID_STATUS, L"There were errors: see the logs (log_*.txt in the output folder).");
                appendLog(L"\r\nSteps with errors: " + std::to_wstring(failed) + L". The logs are in " + out);
            }
            return 0;
        }
        case WM_CLOSE:
            if (g_thread) {
                if (MessageBoxW(h, L"A conversion is running. Cancel it and exit?", L"w3dgui", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                InterlockedExchange(&g_cancel, 1);
                EnterCriticalSection(&g_cs);
                if (g_child) TerminateProcess(g_child, 1);
                LeaveCriticalSection(&g_cs);
                WaitForSingleObject(g_thread, 5000);
            }
            saveSettings();
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
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"w3dgui";
    RegisterClassW(&wc);
    RECT r{0, 0, 602, 648};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND w = CreateWindowW(L"w3dgui", L"WoW 3DS converter (everything in one folder)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
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
