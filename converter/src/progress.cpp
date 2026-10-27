// Progress line on the console window even when stdout/stderr go to a log file
// (convert.bat redirects both): Windows "CONOUT$", elsewhere /dev/tty. Silent if neither opens.
#include "conv.h"

#include <chrono>
#include <cstdarg>

namespace w3d {

namespace {
FILE* g_con = nullptr;
bool g_tried = false;
std::chrono::steady_clock::time_point g_start, g_last;
size_t g_width = 0;

FILE* console() {
    if (!g_tried) {
        g_tried = true;
#ifdef _WIN32
        g_con = std::fopen("CONOUT$", "w");
#else
        g_con = std::fopen("/dev/tty", "w");
#endif
        g_start = std::chrono::steady_clock::now();
    }
    return g_con;
}
}  // namespace

void progress(bool force, const char* fmt, ...) {
    FILE* c = console();
    if (!c) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_last < std::chrono::milliseconds(250)) return;
    g_last = now;
    char msg[160];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    double s = std::chrono::duration<double>(now - g_start).count();
    char line[200];
    std::snprintf(line, sizeof line, "[%3d:%02d] %s", (int)s / 60, (int)s % 60, msg);
    size_t n = std::strlen(line);
    if (n > 78) { line[78] = 0; n = 78; }
    std::fprintf(c, "\r%s%*s", line, (int)(g_width > n ? g_width - n : 0), "");
    g_width = n;
    std::fflush(c);
}

void progressDone(const char* fmt, ...) {
    FILE* c = console();
    if (!c) return;
    char msg[160];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    progress(true, "%s", msg);
    std::fprintf(c, "\n");
    std::fflush(c);
    g_width = 0;
}

}  // namespace w3d
