// Crash guard: when w3dconv dies without a word (access violation, abort, uncaught C++ exception) the log used to end in the
// middle of a line and nothing said where. Now:
//  * trace() remembers what is being converted and flushes stdout, so the log is complete up to that point;
//  * installCrashGuard() makes a dying process print "FATAL ... while: <last trace>" to stderr (and exit with the
//    exception code on Windows, 128+signal elsewhere).
// Tests under AddressSanitizer keep ASan's own reports (the guard installs no signal handlers there).
#include "conv.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define W3D_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define W3D_ASAN 1
#endif
#endif

namespace w3d {

namespace {
char g_what[512] = "(start)";

void report(const char* why) {
    std::fflush(stdout);
    std::fprintf(stderr, "\nFATAL: %s\n  while converting: %s\n  (send this line with log_*.txt)\n", why, g_what);
    std::fflush(stderr);
}

#ifdef _WIN32
LONG WINAPI onException(EXCEPTION_POINTERS* ep) {
    char why[160];
    const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    const char* name = code == EXCEPTION_ACCESS_VIOLATION ? " (access violation)"
                       : code == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)"
                       : code == 0xC0000374 ? " (heap corruption)"
                       : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? " (integer divide by zero)"
                       : code == EXCEPTION_ILLEGAL_INSTRUCTION ? " (illegal instruction)" : "";
    std::snprintf(why, sizeof why, "exception 0x%08lX%s at address %p", (unsigned long)code, name,
                  ep ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    report(why);
    return EXCEPTION_EXECUTE_HANDLER;   // the process ends with the exception code as its exit code
}
#elif !defined(W3D_ASAN)
void onSignal(int sig) {
    // not async-signal-safe in the strict sense, but the process is dying anyway
    char why[64];
    std::snprintf(why, sizeof why, "signal %d", sig);
    report(why);
    std::_Exit(128 + sig);
}
#endif

void onTerminate() {
    std::fflush(stdout);
    std::fprintf(stderr, "\nFATAL: uncaught C++ exception\n  while converting: %s\n", g_what);
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "  what(): %s\n", ex.what());
    } catch (...) {
    }
    std::fflush(stderr);
    std::_Exit(3);
}
}  // namespace

void installCrashGuard() {
    std::set_terminate(onTerminate);
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(onException);
#elif !defined(W3D_ASAN)
    if (!std::getenv("W3D_NO_CRASHGUARD"))
        for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) std::signal(s, onSignal);
#endif
}

void trace(const char* fmt, ...) {
    char tmp[sizeof g_what];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(tmp, sizeof tmp, fmt, a);
    va_end(a);
    std::memcpy(g_what, tmp, sizeof g_what);
    std::fflush(stdout);
}

}  // namespace w3d
