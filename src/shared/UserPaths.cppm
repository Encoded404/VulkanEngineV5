module;

export module VulkanShared.UserPaths;

import std;

export namespace VulkanShared::UserPaths {

// ── Why this exists ─────────────────────────────────────────────────────
//
// The C++ standard library has no notion of "the directory this application
// should write user data to": std::filesystem exposes exactly one OS-defined
// directory, temp_directory_path(), and nothing for %APPDATA%, the XDG base
// directories or ~/Library/Application Support. Every desktop OS answers the
// question differently, so the answer is hand-rolled here.
//
// SDL is deliberately not used. SDL_GetPrefPath() covers the persistent root
// well, but it answers only that one question, allocates and re-walks the path
// on every call, cannot express a cache or log root, and would drag SDL into
// the lowest engine layer and take the resolution away from this module's
// tests. Environment variables are the one mechanism that is uniform across
// Linux, macOS and Windows, and reading them through an injectable lookup
// (see MapEnv) makes the whole resolution a pure function of a snapshot that
// tests can drive without touching the process environment.

// The three roots an application owns, each with a different lifetime:
//
//   persistent  settings and saves. Durable; the only root a user-data reset
//               clears. Back this up and never delete it behind the user's back.
//   cache       regenerable artifacts (pipeline caches, derived data). Safe to
//               delete at any time, including by the OS, so nothing may depend
//               on it being present. Survives a user-data reset.
//   log         session logs and crash reports. Diagnostic, ephemeral.
struct Roots {
    std::filesystem::path persistent;
    std::filesystem::path cache;
    std::filesystem::path log;

    // Which rule produced these paths. Reported at startup so it is possible
    // to tell why an application's data landed where it did.
    enum class Origin {
        Override, // explicit --user-dir or VKENGINE_USER_DIR
        Portable, // marker file next to the executable
        Platform, // OS convention
    } origin = Origin::Platform;

    // Set when a requested rule could not be used and a later one took over
    // (e.g. portable mode was asked for but the directory is not writable).
    // Empty on the normal paths. Never an error on its own: the paths below are
    // still usable.
    std::string fallback_reason;
};

[[nodiscard]] std::string_view ToString(Roots::Origin origin);

struct Identity {
    // Both become path components, so they must be stable once shipped:
    // changing either moves every existing user's settings and saves. Keep them
    // ASCII, short, and different from any user-facing title (a title gets
    // edited for marketing; a directory name must not).
    std::string org;
    std::string app;
};

// Environment access, injected rather than called directly. std::function
// rather than a template so the resolver keeps a single definition in this
// module's BMI and callers do not recompile it.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

[[nodiscard]] EnvLookup SystemEnv();
[[nodiscard]] EnvLookup MapEnv(std::unordered_map<std::string, std::string> vars);

struct Options {
    // Highest priority: an explicit base directory. The application's whole
    // tree lives under it, which is what a --user-dir flag, a test run and a
    // portable launcher all want.
    std::optional<std::filesystem::path> base_dir;
    // Directory containing the executable, used to detect portable mode.
    std::optional<std::filesystem::path> executable_dir;
    // Force portable mode on or off, bypassing the marker file.
    bool force_portable = false;
    bool disable_portable = false;
    // Marker filename looked for next to the executable.
    std::string portable_marker = "portable.txt";
};

// Resolves the three roots. `identity` is sanitized before use, so a title with
// punctuation degrades to a usable directory name instead of failing.
//
// Precedence: options.base_dir -> VKENGINE_USER_DIR -> portable marker ->
// platform default. Portable mode falls back to the platform default (with
// fallback_reason set) when the executable directory is not writable. An
// explicit base directory is never silently overridden: if it cannot be
// created, that is an error.
[[nodiscard]] std::expected<Roots, std::string> Resolve(const Identity& identity,
                                                        const Options& options,
                                                        const EnvLookup& env);
[[nodiscard]] std::expected<Roots, std::string> Resolve(const Identity& identity,
                                                        const Options& options = {});

// Makes `segment` safe to use as a single path component: every character
// outside [A-Za-z0-9._ -] becomes '_'. Returns empty when nothing usable
// remains, which callers must treat as a configuration error. Guards the
// resolution against a ".." or a path separator smuggled in through a name.
[[nodiscard]] std::string SanitizeSegment(std::string_view segment);

// Narrow UTF-8 form of a path, for logs and diagnostics. On Windows this is the
// only lossless narrow conversion; path::string() would go through the active
// ANSI codepage and can throw on a non-representable character.
[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path);

// Inverse of ToUtf8: builds a path from UTF-8 text, e.g. a command-line value.
[[nodiscard]] std::filesystem::path FromUtf8(std::string_view utf8);

} // namespace VulkanShared::UserPaths

// ── Implementation ──────────────────────────────────────────────────────
namespace VulkanShared::UserPaths {

namespace {

constexpr std::string_view kEnvUserDir = "VKENGINE_USER_DIR";

[[nodiscard]] bool IsAbsoluteEnvValue(const std::string& value) {
    return !value.empty() && std::filesystem::path{value}.is_absolute();
}

// XDG base directory specification: a relative value in one of these variables
// is invalid and must be ignored rather than resolved against the CWD. SDL does
// not check this; neither do most naive implementations.
//
// XDG only applies on the platforms that follow it, so this is compiled out
// elsewhere - Windows and macOS have their own conventions and no XDG variable
// to consult.
#if !defined(_WIN32) && !defined(__APPLE__)
[[nodiscard]] std::optional<std::filesystem::path> XdgDir(const EnvLookup& env,
                                                          std::string_view variable) {
    const auto value = env(variable);
    if (!value || !IsAbsoluteEnvValue(*value)) {
        return std::nullopt;
    }
    return std::filesystem::path{*value};
}
#endif

[[nodiscard]] std::optional<std::filesystem::path> HomeDir(const EnvLookup& env) {
#if defined(_WIN32)
    // HOME is a POSIX convention; USERPROFILE is the Windows equivalent and is
    // what the shell actually sets. Check USERPROFILE first so a stray HOME
    // pointing at an MSYS root does not win.
    if (const auto profile = env("USERPROFILE"); profile && IsAbsoluteEnvValue(*profile)) {
        return std::filesystem::path{*profile};
    }
    const auto drive = env("HOMEDRIVE");
    const auto path = env("HOMEPATH");
    if (drive && path && !drive->empty() && !path->empty()) {
        return std::filesystem::path{*drive + *path};
    }
#endif
    if (const auto home = env("HOME"); home && IsAbsoluteEnvValue(*home)) {
        return std::filesystem::path{*home};
    }
    return std::nullopt;
}

// ── Platform defaults ───────────────────────────────────────────────────
//
// Windows splits %APPDATA% (roams with the user profile) from %LOCALAPPDATA%
// (machine specific), so durable data goes in the former and cache/logs in the
// latter. macOS has the same split as Application Support / Caches / Logs.
// Everything else follows XDG.

[[nodiscard]] std::optional<std::filesystem::path>
PersistentRoot(const EnvLookup& env, const std::string& org, const std::string& app) {
#if defined(_WIN32)
    if (const auto appdata = env("APPDATA"); appdata && IsAbsoluteEnvValue(*appdata)) {
        return std::filesystem::path{*appdata} / org / app;
    }
    if (const auto home = HomeDir(env)) {
        return *home / "AppData" / "Roaming" / org / app;
    }
    return std::nullopt;
#elif defined(__APPLE__)
    if (const auto home = HomeDir(env)) {
        return *home / "Library" / "Application Support" / org / app;
    }
    return std::nullopt;
#else
    if (const auto xdg = XdgDir(env, "XDG_DATA_HOME")) {
        return *xdg / org / app;
    }
    if (const auto home = HomeDir(env)) {
        return *home / ".local" / "share" / org / app;
    }
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::filesystem::path>
CacheRoot(const EnvLookup& env, const std::string& org, const std::string& app) {
#if defined(_WIN32)
    if (const auto local = env("LOCALAPPDATA"); local && IsAbsoluteEnvValue(*local)) {
        return std::filesystem::path{*local} / org / app / "cache";
    }
    if (const auto home = HomeDir(env)) {
        return *home / "AppData" / "Local" / org / app / "cache";
    }
    return std::nullopt;
#elif defined(__APPLE__)
    if (const auto home = HomeDir(env)) {
        return *home / "Library" / "Caches" / org / app;
    }
    return std::nullopt;
#else
    if (const auto xdg = XdgDir(env, "XDG_CACHE_HOME")) {
        return *xdg / org / app;
    }
    if (const auto home = HomeDir(env)) {
        return *home / ".cache" / org / app;
    }
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::filesystem::path>
LogRoot(const EnvLookup& env, const std::string& org, const std::string& app) {
#if defined(_WIN32)
    if (const auto local = env("LOCALAPPDATA"); local && IsAbsoluteEnvValue(*local)) {
        return std::filesystem::path{*local} / org / app / "logs";
    }
    if (const auto home = HomeDir(env)) {
        return *home / "AppData" / "Local" / org / app / "logs";
    }
    return std::nullopt;
#elif defined(__APPLE__)
    if (const auto home = HomeDir(env)) {
        return *home / "Library" / "Logs" / org / app;
    }
    return std::nullopt;
#else
    if (const auto xdg = XdgDir(env, "XDG_STATE_HOME")) {
        return *xdg / org / app / "logs";
    }
    if (const auto home = HomeDir(env)) {
        return *home / ".local" / "state" / org / app / "logs";
    }
    return std::nullopt;
#endif
}

// The whole tree under one directory. Used for an explicit base directory and
// for portable mode, where the application owns a single self-contained tree
// and there is no OS convention left to honour.
[[nodiscard]] Roots RootsUnder(const std::filesystem::path& base, Roots::Origin origin) {
    Roots roots;
    roots.persistent = base;
    roots.cache = base / "cache";
    roots.log = base / "logs";
    roots.origin = origin;
    return roots;
}

// Proves `base` is writable by actually writing to it. Existence is not enough:
// a marker beside a binary installed under Program Files, or on a read-only
// mount, must fall back rather than turn portable mode into a failed launch.
[[nodiscard]] bool ProbeWritable(const std::filesystem::path& base) {
    std::error_code ec;
    const auto probe_dir = base / "cache";
    std::filesystem::create_directories(probe_dir, ec);
    if (ec) {
        return false;
    }
    const auto probe = probe_dir / ".write_probe";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.put('1');
        if (!out) {
            std::filesystem::remove(probe, ec);
            return false;
        }
    }
    std::filesystem::remove(probe, ec);
    return true;
}

} // namespace

std::string_view ToString(Roots::Origin origin) {
    switch (origin) {
        case Roots::Origin::Override: return "explicit user directory";
        case Roots::Origin::Portable: return "portable mode";
        case Roots::Origin::Platform: return "platform default";
    }
    return "unknown";
}

EnvLookup SystemEnv() {
    return [](std::string_view name) -> std::optional<std::string> {
        // std::getenv wants a NUL-terminated name. Callers pass literals, but
        // the contract is a string_view, so copy defensively.
        const std::string key{name};
        if (const char* value = std::getenv(key.c_str()); value != nullptr) {
            return std::string{value};
        }
        return std::nullopt;
    };
}

EnvLookup MapEnv(std::unordered_map<std::string, std::string> vars) {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string{name});
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

std::string SanitizeSegment(std::string_view segment) {
    std::string out;
    out.reserve(segment.size());
    for (const char c : segment) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ' || c == '.';
        out.push_back(keep ? c : '_');
    }
    // "." and ".." are still path components after sanitizing, and a name made
    // only of dots would resolve to a parent directory.
    if (out.find_first_not_of('.') == std::string::npos) {
        return {};
    }
    return out;
}

std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    // char8_t is an aliasing type, so reinterpreting the buffer as char is
    // defined; the bytes are unchanged.
    return std::string{reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path FromUtf8(std::string_view utf8) {
    return std::filesystem::path{std::u8string_view{
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()}};
}

std::expected<Roots, std::string> Resolve(const Identity& identity, const Options& options,
                                          const EnvLookup& env) {
    const std::string org = SanitizeSegment(identity.org);
    const std::string app = SanitizeSegment(identity.app);
    if (app.empty()) {
        return std::unexpected("application identity is empty after sanitizing: '" + identity.app +
                               "'");
    }

    // 1. Explicit base directory: --user-dir, or the environment variable with
    //    the same meaning. Both are a deliberate choice, so a failure to create
    //    the tree is reported instead of silently falling back.
    std::optional<std::filesystem::path> base = options.base_dir;
    if (!base) {
        if (const auto from_env = env(kEnvUserDir); from_env && IsAbsoluteEnvValue(*from_env)) {
            base = std::filesystem::path{*from_env};
        }
    }
    if (base) {
        std::error_code ec;
        std::filesystem::create_directories(*base, ec);
        if (ec) {
            return std::unexpected("cannot create user directory '" + ToUtf8(*base) +
                                   "': " + ec.message());
        }
        return RootsUnder(*base, Roots::Origin::Override);
    }

    // 2. Portable mode: a marker file next to the executable, or an explicit
    //    request for it. Not writable is not an error - the marker may simply
    //    have shipped alongside an installed binary - so this falls through to
    //    the platform default with the reason recorded.
    std::string fallback_reason;
    const bool marker_present =
        options.executable_dir && !options.executable_dir->empty() &&
        std::filesystem::exists(*options.executable_dir / options.portable_marker);
    if (!options.disable_portable && (options.force_portable || marker_present)) {
        if (options.executable_dir && !options.executable_dir->empty()) {
            if (ProbeWritable(*options.executable_dir)) {
                return RootsUnder(*options.executable_dir, Roots::Origin::Portable);
            }
            fallback_reason = "portable mode requested but '" +
                              ToUtf8(*options.executable_dir) +
                              "' is not writable; using the platform default";
        } else {
            fallback_reason =
                "portable mode requested but the executable directory is unknown; "
                "using the platform default";
        }
    }

    // 3. Platform convention.
    const auto persistent = PersistentRoot(env, org, app);
    const auto cache = CacheRoot(env, org, app);
    const auto log = LogRoot(env, org, app);
    if (!persistent || !cache || !log) {
        return std::unexpected(
            "cannot resolve a per-user directory: no usable home directory in the environment "
            "(checked APPDATA, LOCALAPPDATA, USERPROFILE, HOMEDRIVE+HOMEPATH, HOME)");
    }

    Roots roots;
    roots.persistent = *persistent;
    roots.cache = *cache;
    roots.log = *log;
    roots.origin = Roots::Origin::Platform;
    roots.fallback_reason = std::move(fallback_reason);
    return roots;
}

std::expected<Roots, std::string> Resolve(const Identity& identity, const Options& options) {
    return Resolve(identity, options, SystemEnv());
}

} // namespace VulkanShared::UserPaths
