module;

export module Examples.InfiniteRunner.Leaderboard.Log;

import std;

export namespace Examples::InfiniteRunner::Leaderboard {

// ─────────────────────────────────────────────────────────────────────────────
// Small dependency-free logging facility for the leaderboard.
//
// The leaderboard library is linked by both the game and the headless server,
// so it cannot use the engine's logger. Messages go to a sink when one is
// installed (the game forwards to LOGIFACE) and to stderr otherwise (the
// server). Levels are ordered so a configured minimum suppresses everything
// below it.
// ─────────────────────────────────────────────────────────────────────────────

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

[[nodiscard]] std::string_view ToString(LogLevel level);

// Accepts "trace", "debug", "info", "warn", "error", "off" (case-insensitive).
[[nodiscard]] std::optional<LogLevel> ParseLogLevel(std::string_view text);

using LogSink = std::function<void(LogLevel, std::string_view)>;

void SetLogLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel GetLogLevel() noexcept;
void SetLogSink(LogSink sink);
[[nodiscard]] bool ShouldLog(LogLevel level) noexcept;

// Formats nothing; callers pass an already-formatted message.
void LogMessage(LogLevel level, std::string_view message);

} // namespace Examples::InfiniteRunner::Leaderboard
