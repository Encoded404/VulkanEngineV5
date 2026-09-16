#ifndef VULKANENGINE_CORE_CRASH_HPP
#define VULKANENGINE_CORE_CRASH_HPP

// Engine-owned crash handling and crash-safe log capture.
//
// This header deliberately includes no standard headers. That keeps it safe to
// include from a module's global module fragment alongside `import std;`
// (avoids Clang bug LLVM #138558) and from the plain entry-point translation
// unit. Everything here is noexcept and, on the fault path, allocation-free.
//
// Lifetime:
//   Crash::Boot()    installs handlers, opens the session log, arms the ring.
//   ... application runs; LogLine/Stage/Breadcrumb record state ...
//   Crash::Shutdown() restores default handlers and closes the session log.
//
// On a fatal fault the handlers write a crash report (report metadata, current
// stage, breadcrumbs, the full captured log ring and a stack trace) next to the
// session log, then terminate the process. Only async-signal-safe operations
// (open/write/close, backtrace_symbols_fd) run on that path.
//
// Session-log writes are batched: LogLine() appends whole lines to a
// per-thread buffer and drains it with a single write() once it fills, so the
// common path never pays a syscall per line. Before writing a report every
// fault handler drains all buffers with write() only, so a crash still
// captures every committed line.

namespace VulkanEngine::Crash {

// Install crash handlers, create the report directory and open the session log.
// Idempotent and safe to call from a static initializer; apps never call this
// directly because the engine entry point does.
void Boot(const char* app_name) noexcept;

// Restore default handlers and close the session log. Safe to call twice.
void Shutdown() noexcept;

// Re-point the session log and crash report to `directory` (created if needed).
// Called by the runtime once the command line has been parsed.
void SetReportDirectory(const char* directory) noexcept;

// Append one already-formatted log line to the crash ring and the session log.
void LogLine(const char* line) noexcept;

// Drain the calling thread's buffered session-log lines to disk. Logging is
// otherwise batched; call this to make the log current at a checkpoint.
void Flush() noexcept;

// Record the current phase. `name` must be a string literal (static lifetime).
void Stage(const char* name) noexcept;

// Record a small tagged event with a numeric payload. `name` must be a string
// literal (static lifetime).
void Breadcrumb(const char* name, unsigned long long value) noexcept;

// Write a crash report without terminating. Used for unhandled C++ exceptions
// that the entry point has already caught.
void Report(const char* reason, const char* detail) noexcept;

// Write a crash report, shut down, and terminate the process.
[[noreturn]] void Fatal(const char* reason, const char* detail) noexcept;

[[nodiscard]] bool Installed() noexcept;

// Directory the session log and crash reports are written to.
[[nodiscard]] const char* ReportDirectory() noexcept;

} // namespace VulkanEngine::Crash

#endif // VULKANENGINE_CORE_CRASH_HPP
