#include "engine/core/Crash.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define VKENGINE_HAVE_EXECINFO 1
#  endif
#endif

// AddressSanitizer installs its own SIGSEGV/SIGBUS handlers and produces far
// better diagnostics than this subsystem can. When it is active (Debug builds
// with ENABLE_SANITIZERS), leave the signal handlers to ASan and only keep the
// crash ring + session log capture. Release builds have no ASan and get the
// full handler set.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define VKENGINE_ASAN_ACTIVE 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(VKENGINE_ASAN_ACTIVE)
#  define VKENGINE_ASAN_ACTIVE 1
#endif

namespace VulkanEngine::Crash {
namespace {

// ── Crash-safe log ring ────────────────────────────────────────────────
// One fixed slot per line: LogLine(), Stage() and Breadcrumb() are the only
// writers, and they never allocate. The fault handlers read the ring with
// plain open/write and never take a lock, so a crash while another thread
// holds the logging mutex is still capturable.
constexpr unsigned kRingLines = 1024;
constexpr unsigned kLineCap = 384;
constexpr unsigned kCrumbs = 64;

char g_ring[kRingLines][kLineCap];
std::atomic<unsigned> g_ring_len[kRingLines]{};
std::atomic<unsigned> g_ring_next{0};

char g_crumb_name[kCrumbs][64];
std::atomic<unsigned> g_crumb_name_len[kCrumbs]{};
std::atomic<unsigned long long> g_crumb_value[kCrumbs]{};
std::atomic<unsigned> g_crumb_next{0};

const char* const kStageStartup = "startup";
std::atomic<const char*> g_stage{kStageStartup};

std::atomic<bool> g_installed{false};
[[maybe_unused]] std::atomic<bool> g_faulted{false};
std::atomic<bool> g_reporting{false};

std::mutex g_file_mutex;
std::FILE* g_log_file = nullptr;

char g_report_dir[1024] = ".";
char g_session_path[1200] = "";
char g_crash_path[1200] = "";
char g_app_name[128] = "VulkanEngineV5";
char g_boot_time[64] = "";

// ── Async-safe raw output ──────────────────────────────────────────────
#if defined(_WIN32)
struct RawOut {
    HANDLE handle = INVALID_HANDLE_VALUE;
    [[nodiscard]] bool valid() const { return handle != INVALID_HANDLE_VALUE; }
    void write(const void* data, std::size_t size) const {
        if (!valid() || size == 0) return;
        DWORD written = 0;
        ::WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr);
    }
    void close() {
        if (valid()) {
            ::CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
};

RawOut RawOpen(const char* path) {
    RawOut out;
    if (path != nullptr && *path != '\0') {
        out.handle = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return out;
}
#else
struct RawOut {
    int fd = -1;
    [[nodiscard]] bool valid() const { return fd >= 0; }
    void write(const void* data, std::size_t size) const {
        if (fd < 0 || size == 0) return;
        const ssize_t written = ::write(fd, data, size);
        static_cast<void>(written);
    }
    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

RawOut RawOpen(const char* path) {
    RawOut out;
    if (path != nullptr && *path != '\0') {
        out.fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    return out;
}
#endif

// ── Async-safe formatting (no printf) ──────────────────────────────────
void WriteStr(const RawOut& out, const char* text) {
    if (text == nullptr) return;
    out.write(text, std::strlen(text));
}

void WriteDec(const RawOut& out, unsigned long long value) {
    char reversed[24];
    int count = 0;
    if (value == 0) {
        reversed[count++] = '0';
    } else {
        while (value != 0 && count < 20) {
            reversed[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    char text[24];
    for (int i = 0; i < count; ++i) {
        text[i] = reversed[count - 1 - i];
    }
    out.write(text, static_cast<std::size_t>(count));
}

void WriteHex(const RawOut& out, unsigned long long value) {
    static const char kDigits[] = "0123456789abcdef";
    char reversed[16];
    int count = 0;
    if (value == 0) {
        reversed[count++] = '0';
    } else {
        while (value != 0 && count < 16) {
            reversed[count++] = kDigits[value & 0xFu];
            value >>= 4;
        }
    }
    char text[16];
    for (int i = 0; i < count; ++i) {
        text[i] = reversed[count - 1 - i];
    }
    out.write(text, static_cast<std::size_t>(count));
}

void CopyString(char* dst, std::size_t cap, const char* src) {
    if (cap == 0) return;
    std::size_t i = 0;
    if (src != nullptr) {
        while (src[i] != '\0' && i + 1 < cap) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

// ── Stack capture ──────────────────────────────────────────────────────
void WriteStack(const RawOut& out) {
#if defined(_WIN32)
    void* frames[64];
    const USHORT count = ::RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        WriteStr(out, "0x");
        WriteHex(out, reinterpret_cast<unsigned long long>(frames[i]));
        WriteStr(out, "\n");
    }
#elif defined(VKENGINE_HAVE_EXECINFO)
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    if (count > 0) {
        ::backtrace_symbols_fd(frames, count, out.fd);
    }
#else
    static_cast<void>(out);
#endif
}

// ── Report body ────────────────────────────────────────────────────────
void WriteReportHeader(const RawOut& out, const char* reason) {
    WriteStr(out, "===== VULKANENGINE CRASH REPORT =====\n");
    WriteStr(out, "app: ");
    WriteStr(out, g_app_name);
    WriteStr(out, "\nreason: ");
    WriteStr(out, reason != nullptr ? reason : "unknown");
    WriteStr(out, "\nstarted: ");
    WriteStr(out, g_boot_time);
    WriteStr(out, "\nstage: ");
    WriteStr(out, g_stage.load(std::memory_order_relaxed));
    WriteStr(out, "\nsession log: ");
    WriteStr(out, g_session_path);
    WriteStr(out, "\n");
}

void WriteReportBody(const RawOut& out) {
    WriteStr(out, "\n--- breadcrumbs ---\n");
    const unsigned crumb_end = g_crumb_next.load(std::memory_order_relaxed);
    const unsigned crumb_begin = crumb_end > kCrumbs ? crumb_end - kCrumbs : 0;
    for (unsigned i = crumb_begin; i < crumb_end; ++i) {
        const unsigned slot = i % kCrumbs;
        const unsigned name_len = g_crumb_name_len[slot].load(std::memory_order_relaxed);
        if (name_len == 0) continue;
        WriteStr(out, g_crumb_name[slot]);
        WriteStr(out, " = ");
        WriteDec(out, g_crumb_value[slot].load(std::memory_order_relaxed));
        WriteStr(out, "\n");
    }

    WriteStr(out, "\n--- log tail ---\n");
    const unsigned ring_end = g_ring_next.load(std::memory_order_relaxed);
    const unsigned ring_begin = ring_end > kRingLines ? ring_end - kRingLines : 0;
    for (unsigned i = ring_begin; i < ring_end; ++i) {
        const unsigned slot = i % kRingLines;
        const unsigned len = g_ring_len[slot].load(std::memory_order_acquire);
        if (len == 0 || len >= kLineCap) continue;
        out.write(g_ring[slot], len);
        out.write("\n", 1);
    }

    WriteStr(out, "\n--- stack ---\n");
    WriteStack(out);
    WriteStr(out, "\n===== END CRASH REPORT =====\n");
}

// ── Path helpers ───────────────────────────────────────────────────────
std::string ExecutablePath() {
#if defined(_WIN32)
    char buffer[MAX_PATH * 4];
    const DWORD len = ::GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (len > 0 && len < sizeof(buffer)) return std::string(buffer, buffer + len);
#elif defined(__linux__)
    char buffer[4096];
    const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) return std::string(buffer, buffer + len);
#endif
    return {};
}

void EnsureDirectory(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    static_cast<void>(ec);
}

void DeriveDefaultPaths() {
    const std::string exe = ExecutablePath();
    std::string exe_dir;
    if (!exe.empty()) {
        exe_dir = std::filesystem::path(exe).parent_path().string();
    }
    if (exe_dir.empty()) exe_dir = ".";

    const std::string dir = exe_dir + "/logs/" + g_app_name;
    EnsureDirectory(dir);
    CopyString(g_report_dir, sizeof(g_report_dir), dir.c_str());
    CopyString(g_session_path, sizeof(g_session_path), (dir + "/session.log").c_str());
    CopyString(g_crash_path, sizeof(g_crash_path), (dir + "/crash_report.txt").c_str());
}

void OpenSessionLog() {
    std::lock_guard<std::mutex> const lock(g_file_mutex);
    if (g_log_file != nullptr) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
    if (g_session_path[0] != '\0') {
        g_log_file = std::fopen(g_session_path, "w");
    }
}

// ── Terminate / abort handling ─────────────────────────────────────────
void WriteFaultReport(const char* reason, unsigned long long code, const char* detail) {
    RawOut out = RawOpen(g_crash_path);
    if (!out.valid()) return;
    WriteReportHeader(out, reason);
    WriteStr(out, "code: 0x");
    WriteHex(out, code);
    WriteStr(out, "\n");
    if (detail != nullptr && *detail != '\0') {
        WriteStr(out, "detail: ");
        WriteStr(out, detail);
        WriteStr(out, "\n");
    }
    WriteReportBody(out);
    out.close();
}

void FlushSessionLog() {
    std::lock_guard<std::mutex> const lock(g_file_mutex);
    if (g_log_file != nullptr) {
        std::fflush(g_log_file);
    }
}

void CrashTerminateHandler() {
    WriteFaultReport("std::terminate", 0, "unhandled exception or noexcept violation");
    FlushSessionLog();
    std::_Exit(3);
}

#if defined(_WIN32)
LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* info) {
    if (g_faulted.exchange(true)) {
        ::TerminateProcess(::GetCurrentProcess(), 3);
    }
    const unsigned long code =
        (info != nullptr && info->ExceptionRecord != nullptr) ? info->ExceptionRecord->ExceptionCode : 0u;
    RawOut out = RawOpen(g_crash_path);
    if (out.valid()) {
        WriteReportHeader(out, "structured exception");
        WriteStr(out, "code: 0x");
        WriteHex(out, code);
        WriteStr(out, "\n");
        if (info != nullptr && info->ExceptionRecord != nullptr) {
            WriteStr(out, "fault address: 0x");
            WriteHex(out, reinterpret_cast<unsigned long long>(info->ExceptionRecord->ExceptionAddress));
            WriteStr(out, "\n");
        }
        WriteReportBody(out);
        out.close();
    }
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code != 0 ? code : 3u));
    return EXCEPTION_EXECUTE_HANDLER;
}

extern "C" void CrashAbortHandler(int) {
    WriteFaultReport("SIGABRT", 0, nullptr);
    FlushSessionLog();
    std::_Exit(3);
}
#else
#if !defined(VKENGINE_ASAN_ACTIVE)
extern "C" void CrashSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    static_cast<void>(ucontext);
    if (g_faulted.exchange(true)) {
        ::_exit(128 + sig);
    }
    RawOut out = RawOpen(g_crash_path);
    if (out.valid()) {
        WriteReportHeader(out, "signal");
        WriteStr(out, "signal: ");
        WriteDec(out, static_cast<unsigned long long>(sig));
        WriteStr(out, "\n");
        if (info != nullptr) {
            WriteStr(out, "fault address: 0x");
            WriteHex(out, reinterpret_cast<unsigned long long>(info->si_addr));
            WriteStr(out, "\n");
        }
        WriteReportBody(out);
        out.close();
    }
    // Re-raise with the default disposition so the OS still produces a core
    // dump and reports the conventional 128+N status.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
    ::_exit(128 + sig);
}
#endif
#endif

void InstallHandlers() {
    std::set_terminate(&CrashTerminateHandler);
#if defined(_WIN32)
    ::SetUnhandledExceptionFilter(&CrashExceptionFilter);
    ::signal(SIGABRT, &CrashAbortHandler);
#  if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#  endif
#else
#if !defined(VKENGINE_ASAN_ACTIVE)
    static char altstack[64 * 1024];
    stack_t stack{};
    stack.ss_sp = altstack;
    stack.ss_size = sizeof(altstack);
    stack.ss_flags = 0;
    ::sigaltstack(&stack, nullptr);

    int signals[8];
    int count = 0;
    signals[count++] = SIGSEGV;
    signals[count++] = SIGBUS;
    signals[count++] = SIGILL;
    signals[count++] = SIGFPE;
    signals[count++] = SIGABRT;
#  ifdef SIGSYS
    signals[count++] = SIGSYS;
#  endif
#  ifdef SIGTRAP
    signals[count++] = SIGTRAP;
#  endif

    for (int i = 0; i < count; ++i) {
        struct sigaction action {};
        action.sa_sigaction = &CrashSignalHandler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(signals[i], &action, nullptr);
    }
#endif
#endif
}

void RestoreHandlers() {
#if defined(_WIN32)
    ::SetUnhandledExceptionFilter(nullptr);
    ::signal(SIGABRT, SIG_DFL);
#else
#if !defined(VKENGINE_ASAN_ACTIVE)
    ::signal(SIGSEGV, SIG_DFL);
    ::signal(SIGBUS, SIG_DFL);
    ::signal(SIGILL, SIG_DFL);
    ::signal(SIGFPE, SIG_DFL);
    ::signal(SIGABRT, SIG_DFL);
#  ifdef SIGSYS
    ::signal(SIGSYS, SIG_DFL);
#  endif
#  ifdef SIGTRAP
    ::signal(SIGTRAP, SIG_DFL);
#  endif
#endif
#endif
}

} // namespace

// ── Public API ─────────────────────────────────────────────────────────

void Boot(const char* app_name) noexcept {
    if (g_installed.exchange(true)) {
        return;
    }
    try {
        if (app_name != nullptr && *app_name != '\0') {
            CopyString(g_app_name, sizeof(g_app_name), app_name);
        } else {
            const std::string exe = ExecutablePath();
            if (!exe.empty()) {
                const std::string stem = std::filesystem::path(exe).stem().string();
                if (!stem.empty()) CopyString(g_app_name, sizeof(g_app_name), stem.c_str());
            }
        }

        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        ::localtime_s(&tm, &now);
#else
        ::localtime_r(&now, &tm);
#endif
        if (std::strftime(g_boot_time, sizeof(g_boot_time), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
            CopyString(g_boot_time, sizeof(g_boot_time), "unknown");
        }

        DeriveDefaultPaths();
        OpenSessionLog();
        InstallHandlers();
    } catch (...) {
        // Crash handling must never abort startup. Handlers may be partially
        // installed; the app still runs.
    }
}

void Shutdown() noexcept {
    if (!g_installed.exchange(false)) {
        return;
    }
    RestoreHandlers();
    std::lock_guard<std::mutex> const lock(g_file_mutex);
    if (g_log_file != nullptr) {
        std::fflush(g_log_file);
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

void SetReportDirectory(const char* directory) noexcept {
    if (!g_installed.load()) {
        return;
    }
    try {
        if (directory != nullptr && *directory != '\0') {
            const std::string dir = directory;
            EnsureDirectory(dir);
            CopyString(g_report_dir, sizeof(g_report_dir), dir.c_str());
            CopyString(g_session_path, sizeof(g_session_path), (dir + "/session.log").c_str());
            CopyString(g_crash_path, sizeof(g_crash_path), (dir + "/crash_report.txt").c_str());
            OpenSessionLog();
        } else {
            DeriveDefaultPaths();
            OpenSessionLog();
        }
    } catch (...) {
        // Keep the previous directory on failure.
    }
}

void LogLine(const char* line) noexcept {
    if (line == nullptr) return;

    const std::size_t length = std::strlen(line);
    const unsigned index = g_ring_next.fetch_add(1, std::memory_order_relaxed);
    const unsigned slot = index % kRingLines;
    const std::size_t copy = length < (kLineCap - 1) ? length : (kLineCap - 1);
    if (copy > 0) {
        std::memcpy(g_ring[slot], line, copy);
    }
    g_ring[slot][copy] = '\0';
    g_ring_len[slot].store(static_cast<unsigned>(copy), std::memory_order_release);

    std::lock_guard<std::mutex> const lock(g_file_mutex);
    if (g_log_file != nullptr) {
        std::fwrite(line, 1, length, g_log_file);
        std::fputc('\n', g_log_file);
        std::fflush(g_log_file);
    }
}

void Stage(const char* name) noexcept {
    if (name == nullptr) return;
    g_stage.store(name, std::memory_order_relaxed);
}

void Breadcrumb(const char* name, unsigned long long value) noexcept {
    if (name == nullptr) return;
    const unsigned index = g_crumb_next.fetch_add(1, std::memory_order_relaxed);
    const unsigned slot = index % kCrumbs;
    g_crumb_value[slot].store(value, std::memory_order_relaxed);
    const std::size_t length = std::strlen(name);
    const std::size_t copy = length < (sizeof(g_crumb_name[slot]) - 1)
                                 ? length
                                 : (sizeof(g_crumb_name[slot]) - 1);
    if (copy > 0) {
        std::memcpy(g_crumb_name[slot], name, copy);
    }
    g_crumb_name[slot][copy] = '\0';
    g_crumb_name_len[slot].store(static_cast<unsigned>(copy), std::memory_order_release);
}

void Report(const char* reason, const char* detail) noexcept {
    if (g_reporting.exchange(true)) {
        return;
    }
    try {
        char line[512];
        const int written = std::snprintf(line, sizeof(line), "[CRASH] %s%s%s",
                                          reason != nullptr ? reason : "unknown",
                                          (detail != nullptr && *detail != '\0') ? ": " : "",
                                          (detail != nullptr && *detail != '\0') ? detail : "");
        if (written > 0) {
            LogLine(line);
        }
        WriteFaultReport(reason != nullptr ? reason : "unknown", 0, detail);
    } catch (...) {
        // Nothing sensible left to do.
    }
}

void Fatal(const char* reason, const char* detail) noexcept {
    Report(reason, detail);
    Shutdown();
    std::_Exit(1);
}

bool Installed() noexcept {
    return g_installed.load(std::memory_order_relaxed);
}

const char* ReportDirectory() noexcept {
    return g_report_dir;
}

} // namespace VulkanEngine::Crash
