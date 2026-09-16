module;

module Examples.InfiniteRunner.Leaderboard.Log;

import std;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_sink_mutex;
std::shared_ptr<LogSink> g_sink;
// Serialises the default sink so concurrent connection handlers cannot
// interleave a line. An installed sink is the caller's to synchronise.
std::mutex g_output_mutex;

} // namespace

std::string_view ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Off: return "off";
    }
    return "unknown";
}

std::optional<LogLevel> ParseLogLevel(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lowered == "trace") { return LogLevel::Trace; }
    if (lowered == "debug") { return LogLevel::Debug; }
    if (lowered == "info") { return LogLevel::Info; }
    if (lowered == "warn" || lowered == "warning") { return LogLevel::Warn; }
    if (lowered == "error") { return LogLevel::Error; }
    if (lowered == "off" || lowered == "none") { return LogLevel::Off; }
    return std::nullopt;
}

void SetLogLevel(LogLevel level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

LogLevel GetLogLevel() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

void SetLogSink(LogSink sink) {
    std::lock_guard lock(g_sink_mutex);
    if (!sink) {
        // Clearing falls back to the default stderr sink rather than storing an
        // empty function the log path would call.
        g_sink.reset();
        return;
    }
    g_sink = std::make_shared<LogSink>(std::move(sink));
}

bool ShouldLog(LogLevel level) noexcept {
    const LogLevel configured = GetLogLevel();
    if (configured == LogLevel::Off) {
        return false;
    }
    return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(configured);
}

void LogMessage(LogLevel level, std::string_view message) {
    if (!ShouldLog(level)) {
        return;
    }
    std::shared_ptr<LogSink> sink;
    {
        std::lock_guard lock(g_sink_mutex);
        sink = g_sink;
    }
    if (sink != nullptr) {
        (*sink)(level, message);
        return;
    }
    std::lock_guard lock(g_output_mutex);
    std::cerr << "[leaderboard] " << ToString(level) << ": " << message << '\n';
}

} // namespace Examples::InfiniteRunner::Leaderboard
