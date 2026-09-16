module;

// Native file handles for the durability step (fsync/_commit) and for a
// process-unique staging name. Kept to the small platform headers on purpose:
// <windows.h> would drag a large macro surface into this module's global
// module fragment for no gain (_wopen/_commit handle a wide path directly).
#include <cerrno>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

export module VulkanShared.Storage;

import std;

import VulkanShared.UserPaths;

export namespace VulkanShared::Storage {

// Per-location storage with one read and one write per location, plus the
// handful of lifecycle operations that need to see more than one file.
//
// The object is a value-like handle: constructing it resolves the roots once
// and creates their directory trees, and every operation afterwards is a
// member call that cannot fail for want of a directory. Nothing is resolved
// per call, so the platform lookup happens exactly once per process.
//
// Three properties the API encodes deliberately:
//
//   * Errors are values, not exceptions and not bools. A missing settings file
//     is ErrorCode::NotFound, which is a normal state meaning "use defaults";
//     everything else is a real failure worth reporting.
//   * Cache writes cannot fail by contract (they return void). Cache contents
//     are regenerable, so a read-only or full disk degrades to a slow start
//     rather than a broken one. Settings and save writes return an error
//     because losing those is not recoverable.
//   * Writes are atomic and, by default, durable: contents are staged in a
//     process-unique temporary file in the destination directory, flushed, and
//     then published with a single rename.

enum class ErrorCode {
    NotFound, // the file does not exist yet; callers treat this as "no data"
    InvalidName, // the name is not usable as a single path component
    TooLarge, // larger than the configured limit
    Unavailable, // no usable storage root could be resolved
    CreateDirectoryFailed,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
};

struct Error {
    ErrorCode code = ErrorCode::Unavailable;
    std::string message;
    std::filesystem::path path;

    [[nodiscard]] std::string ToString() const {
        std::string out = message;
        if (!path.empty()) {
            out += " (";
            out += UserPaths::ToUtf8(path);
            out += ')';
        }
        return out;
    }
};

// Whether a file's contents are sensitive. Private files are created with
// owner-only permissions on POSIX (0600); on Windows the per-user root
// (%APPDATA%/%LOCALAPPDATA%) already carries a user-scoped ACL, so the flag is
// a no-op there. Use it for credentials and tokens, not for ordinary saves.
enum class Visibility {
    Normal,
    Private,
};

struct Options {
    // Upper bounds applied on read. A corrupt or runaway file must not become a
    // huge allocation; these are generous enough for real data and small enough
    // that a truncated write is caught rather than mapped.
    std::size_t max_config_bytes = 4u * 1024u * 1024u;
    std::size_t max_save_bytes = 256u * 1024u * 1024u;
    std::size_t max_cache_bytes = 256u * 1024u * 1024u;

    // Flush contents to stable storage before the rename that publishes them.
    // The rename alone makes a write atomic but not durable: the directory
    // entry can land while the data is still in the page cache, so a power loss
    // leaves a correctly named, empty file. A sync per write costs a disk
    // flush, which is the right trade for a save and the wrong one for a cache.
    bool durable_writes = true;

    // Preserve the previous settings file as settings.json.bak before
    // overwriting it, so a bad write is recoverable by hand.
    bool keep_config_backup = true;
};

class Storage {
public:
    // Resolves the roots via VulkanShared.UserPaths and creates them. Fails
    // only when no usable root exists (no home directory, or an explicit
    // --user-dir that cannot be created).
    [[nodiscard]] static std::expected<Storage, Error>
    Create(const UserPaths::Identity& identity, const UserPaths::Options& options = {},
           const UserPaths::EnvLookup& env = UserPaths::SystemEnv());

    // Takes the roots verbatim. This is how a test points storage at a scratch
    // directory, and how a platform whose sandbox dictates its directories
    // (a console, a mobile app bundle) supplies them.
    [[nodiscard]] static std::expected<Storage, Error> CreateWithRoots(UserPaths::Roots roots,
                                                                      Options options = {});

    Storage(const Storage&) = default;
    Storage& operator=(const Storage&) = default;
    Storage(Storage&&) = default;
    Storage& operator=(Storage&&) = default;
    ~Storage() = default;

    // ── settings ────────────────────────────────────────────────────────
    // The settings file as JSON text. The caller owns the schema; this layer
    // owns the file. NotFound means "nothing saved yet".
    [[nodiscard]] std::expected<std::string, Error> ReadConfig() const;
    [[nodiscard]] std::expected<void, Error> WriteConfig(
        std::string_view json, Visibility visibility = Visibility::Normal) const;

    // ── saves ───────────────────────────────────────────────────────────
    // One slot, one read and one write. `slot` is a single path component
    // (a slot name), rejected rather than sanitized if it is anything else.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error>
    ReadSave(std::string_view slot) const;
    [[nodiscard]] std::expected<void, Error> WriteSave(
        std::string_view slot, std::span<const std::byte> data,
        Visibility visibility = Visibility::Normal) const;

    // ── cache ───────────────────────────────────────────────────────────
    // `name` is a single path component under the cache root. Read and write
    // are best effort by contract; see WriteCache.
    [[nodiscard]] std::expected<std::vector<std::byte>, Error>
    ReadCache(std::string_view name) const;
    void WriteCache(std::string_view name, std::span<const std::byte> data) const noexcept;

    // ── lifecycle ───────────────────────────────────────────────────────
    // Deletes the settings file and every save. The cache root is deliberately
    // left untouched: a pipeline cache is expensive to rebuild and is not user
    // data, so a reset must not cost the user a slow next launch. Logs are left
    // alone for the same reason - they are diagnostics, not user data.
    [[nodiscard]] std::expected<void, Error> ResetUserData() const;

    // ── paths ───────────────────────────────────────────────────────────
    // Exposed because the surrounding engine already takes directories (the
    // shader pipeline cache, the crash reporter) rather than file handles.
    [[nodiscard]] const UserPaths::Roots& GetRoots() const noexcept { return roots_; }
    [[nodiscard]] const std::filesystem::path& ConfigPath() const noexcept {
        return config_path_;
    }
    [[nodiscard]] const std::filesystem::path& ConfigBackupPath() const noexcept {
        return config_backup_path_;
    }
    [[nodiscard]] const std::filesystem::path& SaveDir() const noexcept { return save_dir_; }
    [[nodiscard]] const std::filesystem::path& CacheDir() const noexcept { return roots_.cache; }
    [[nodiscard]] const std::filesystem::path& LogDir() const noexcept { return roots_.log; }
    [[nodiscard]] const Options& GetOptions() const noexcept { return options_; }

    // Empty when `slot` is not a valid name. Callers that need the reason use
    // ReadSave/WriteSave, which report it.
    [[nodiscard]] std::filesystem::path SavePath(std::string_view slot) const;

private:
    Storage(UserPaths::Roots roots, Options options);

    [[nodiscard]] std::expected<std::filesystem::path, Error>
    ResolveLeaf(const std::filesystem::path& dir, std::string_view name) const;

    UserPaths::Roots roots_{};
    Options options_{};
    std::filesystem::path config_path_;
    std::filesystem::path config_backup_path_;
    std::filesystem::path save_dir_;
};

} // namespace VulkanShared::Storage

// ── Implementation ──────────────────────────────────────────────────────
namespace VulkanShared::Storage {

namespace {

[[nodiscard]] std::string DescribeErrno(int value) {
    return std::error_code{value, std::generic_category()}.message();
}

[[nodiscard]] std::uint64_t ProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// Disambiguates staging files between concurrent writers inside one process.
// A pid alone is not enough: two threads writing the same file would stage into
// one buffer, and whichever renamed last would publish a blend of both.
[[nodiscard]] std::uint64_t NextWriteSequence() noexcept {
    static std::atomic<std::uint64_t> sequence{0};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] std::expected<void, Error> EnsureDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return {};
    }
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected(Error{.code = ErrorCode::CreateDirectoryFailed,
                                     .message = "cannot create directory: " + ec.message(),
                                     .path = dir});
    }
    return {};
}

// Rejects anything that would escape the target directory or name something the
// OS cannot represent. Names arrive from application code, and for saves
// potentially from a user-entered slot name, so this is a boundary rather than
// a formality: it rejects instead of sanitizing, because silently writing to a
// different file than the caller asked for is worse than refusing.
[[nodiscard]] std::expected<void, Error> ValidateLeafName(std::string_view name,
                                                         const std::filesystem::path& parent) {
    const auto reject = [&parent](std::string_view reason) {
        return std::unexpected(Error{.code = ErrorCode::InvalidName,
                                     .message = "invalid name: " + std::string{reason},
                                     .path = parent});
    };

    if (name.empty()) {
        return reject("empty");
    }
    if (name == "." || name == "..") {
        return reject("directory reference");
    }
    if (name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos) {
        return reject("contains a path separator");
    }
    if (name.find('\0') != std::string_view::npos) {
        return reject("contains a NUL byte");
    }
#if defined(_WIN32)
    // Characters Windows forbids in a filename, and a trailing dot or space,
    // which the API strips silently - making two distinct names collide.
    if (name.find_first_of("<>:\"|?*") != std::string_view::npos) {
        return reject("contains a character Windows forbids in a filename");
    }
    if (name.back() == '.' || name.back() == ' ') {
        return reject("ends with a dot or space");
    }
    // Reserved DOS device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9) open a
    // device instead of a file, even with an extension.
    std::string device{name.substr(0, name.find('.'))};
    for (char& c : device) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const bool digits = device.size() == 4 &&
                        device[3] >= '1' && device[3] <= '9';
    if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
        (digits && (device.starts_with("COM") || device.starts_with("LPT")))) {
        return reject("is a reserved device name");
    }
#endif
    return {};
}

// Flushes a freshly closed file to stable storage. The rename that publishes it
// is atomic but not durable on its own; this is what makes the contents survive
// a power loss. Reopening is enough on POSIX (fsync flushes the file's dirty
// pages regardless of which descriptor wrote them) but Windows needs a
// writable handle for FlushFileBuffers, hence the different flags.
[[nodiscard]] std::expected<void, Error> SyncToDisk(const std::filesystem::path& path) {
#if defined(_WIN32)
    const int fd = ::_wopen(path.c_str(), _O_WRONLY);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
#endif
    if (fd < 0) {
        return std::unexpected(Error{.code = ErrorCode::OpenFailed,
                                     .message = "cannot reopen for sync: " + DescribeErrno(errno),
                                     .path = path});
    }
#if defined(_WIN32)
    const int rc = ::_commit(fd);
    const int saved = errno;
    ::_close(fd);
#else
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
#endif
    if (rc != 0) {
        return std::unexpected(Error{.code = ErrorCode::SyncFailed,
                                     .message = "cannot flush to disk: " + DescribeErrno(saved),
                                     .path = path});
    }
    return {};
}

// Removes the staging file unless it has been committed. Every early return in
// WriteFileAtomic below is a failure path that must not leave detritus behind,
// and the rename is the only point at which the staging file stops being ours.
class StagingFile {
public:
    explicit StagingFile(std::filesystem::path path) : path_(std::move(path)) {}

    ~StagingFile() {
        if (!committed_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;
    StagingFile(StagingFile&&) = delete;
    StagingFile& operator=(StagingFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    void Commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

// Writes the staged file's contents. A POSIX private file is created 0600 with
// open(2) rather than ofstream, because the stream API cannot set a mode at
// creation and a follow-up chmod would leave a window where the file is
// world-readable.
[[nodiscard]] std::expected<void, Error> WriteStagedContents(const std::filesystem::path& path,
                                                             std::span<const std::byte> data,
                                                             Visibility visibility) {
#if !defined(_WIN32)
    if (visibility == Visibility::Private) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            return std::unexpected(Error{.code = ErrorCode::OpenFailed,
                                         .message = "cannot open for writing: " + DescribeErrno(errno),
                                         .path = path});
        }
        std::size_t written = 0;
        while (written < data.size()) {
            const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int saved = errno;
                ::close(fd);
                return std::unexpected(Error{.code = ErrorCode::WriteFailed,
                                             .message = "write failed: " + DescribeErrno(saved),
                                             .path = path});
            }
            written += static_cast<std::size_t>(count);
        }
        ::close(fd);
        return {};
    }
#else
    (void)visibility;
#endif
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(Error{.code = ErrorCode::OpenFailed,
                                     .message = "cannot open for writing",
                                     .path = path});
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    // close() is what flushes the stream buffer, so the failure check has to
    // come after it or a full disk is missed.
    out.close();
    if (out.fail()) {
        return std::unexpected(Error{.code = ErrorCode::WriteFailed,
                                     .message = "write failed",
                                     .path = path});
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> WriteFileAtomic(const std::filesystem::path& target,
                                                        std::span<const std::byte> data,
                                                        bool durable,
                                                        Visibility visibility) {
    if (auto ready = EnsureDirectory(target.parent_path()); !ready) {
        return ready;
    }

    // Staging name carries the pid and a per-process sequence: two writers -
    // two instances of the application, or two threads in one - must not stage
    // into one another's buffer, or the winner's rename publishes a blend of
    // both.
    std::filesystem::path staging = target;
    staging += std::format(".{}.{}.tmp", ProcessId(), NextWriteSequence());
    StagingFile guard{staging};

    if (auto written = WriteStagedContents(guard.Path(), data, visibility); !written) {
        return written;
    }

    if (durable) {
        if (auto synced = SyncToDisk(guard.Path()); !synced) {
            return synced;
        }
    }

    // Publish. rename replaces an existing regular file on POSIX and on Windows,
    // and is atomic within one filesystem - guaranteed here because the staging
    // file lives in the destination directory.
    std::error_code ec;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::filesystem::rename(guard.Path(), target, ec);
        if (!ec) {
            guard.Commit();
            return {};
        }
        // Retry only what can actually succeed on a second attempt. A staging
        // file that is already gone means this rename can never work, so a
        // retry would just cost the caller 150ms before the same error. Anything
        // else is treated as a transient lock on the destination, which is a
        // real Windows condition (antivirus, the search indexer, a sync client
        // holding the target open).
        std::error_code probe;
        if (!std::filesystem::exists(guard.Path(), probe)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
    }
    return std::unexpected(Error{.code = ErrorCode::RenameFailed,
                                 .message = "cannot publish file: " + ec.message(),
                                 .path = target});
}

[[nodiscard]] std::expected<std::vector<std::byte>, Error>
ReadFileLimited(const std::filesystem::path& path, std::size_t max_bytes) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    // not_found is checked before ec so that a genuine absence (a normal state
    // meaning "no settings yet") is not confused with a real lookup failure such
    // as an unreadable parent directory.
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::unexpected(Error{.code = ErrorCode::NotFound,
                                     .message = "no such file",
                                     .path = path});
    }
    if (ec) {
        return std::unexpected(Error{.code = ErrorCode::ReadFailed,
                                     .message = "cannot examine file: " + ec.message(),
                                     .path = path});
    }
    if (status.type() != std::filesystem::file_type::regular) {
        return std::unexpected(Error{.code = ErrorCode::ReadFailed,
                                     .message = "not a regular file",
                                     .path = path});
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error{.code = ErrorCode::ReadFailed,
                                     .message = "cannot determine size: " + ec.message(),
                                     .path = path});
    }
    if (size > max_bytes) {
        return std::unexpected(
            Error{.code = ErrorCode::TooLarge,
                  .message = std::format("file is {} bytes, limit is {}", size, max_bytes),
                  .path = path});
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{.code = ErrorCode::OpenFailed,
                                     .message = "cannot open for reading",
                                     .path = path});
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (in.gcount() != static_cast<std::streamsize>(data.size())) {
            return std::unexpected(Error{.code = ErrorCode::ReadFailed,
                                         .message = "short read",
                                         .path = path});
        }
    }
    return data;
}

} // namespace

Storage::Storage(UserPaths::Roots roots, Options options)
    : roots_(std::move(roots)), options_(options) {
    config_path_ = roots_.persistent / "settings.json";
    config_backup_path_ = roots_.persistent / "settings.json.bak";
    save_dir_ = roots_.persistent / "saves";
}

std::expected<Storage, Error> Storage::Create(const UserPaths::Identity& identity,
                                             const UserPaths::Options& options,
                                             const UserPaths::EnvLookup& env) {
    auto roots = UserPaths::Resolve(identity, options, env);
    if (!roots) {
        return std::unexpected(Error{.code = ErrorCode::Unavailable, .message = roots.error()});
    }
    return CreateWithRoots(std::move(*roots));
}

std::expected<Storage, Error> Storage::CreateWithRoots(UserPaths::Roots roots, Options options) {
    for (const std::filesystem::path* dir : {&roots.persistent, &roots.cache, &roots.log}) {
        if (auto ready = EnsureDirectory(*dir); !ready) {
            return std::unexpected(ready.error());
        }
    }

    Storage storage{std::move(roots), options};
    if (auto ready = EnsureDirectory(storage.save_dir_); !ready) {
        return std::unexpected(ready.error());
    }
    return storage;
}

std::expected<std::filesystem::path, Error>
Storage::ResolveLeaf(const std::filesystem::path& dir, std::string_view name) const {
    if (auto valid = ValidateLeafName(name, dir); !valid) {
        return std::unexpected(valid.error());
    }
    // name is UTF-8 from the caller; going through FromUtf8 is what makes a
    // non-ASCII slot name work on Windows instead of being read as ANSI.
    return dir / UserPaths::FromUtf8(name);
}

std::filesystem::path Storage::SavePath(std::string_view slot) const {
    auto resolved = ResolveLeaf(save_dir_, slot);
    if (!resolved) {
        return {};
    }
    return *resolved;
}

std::expected<std::string, Error> Storage::ReadConfig() const {
    auto bytes = ReadFileLimited(config_path_, options_.max_config_bytes);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

std::expected<void, Error> Storage::WriteConfig(std::string_view json, Visibility visibility) const {
    if (auto ready = EnsureDirectory(config_path_.parent_path()); !ready) {
        return ready;
    }
    if (options_.keep_config_backup) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(config_path_, ec)) {
            // Copy rather than rename: the current settings must stay readable
            // until the new file is published. A failed backup is not fatal -
            // it is a convenience, and blocking the write would be worse.
            std::filesystem::copy_file(config_path_, config_backup_path_,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    return WriteFileAtomic(config_path_, std::as_bytes(std::span{json}), options_.durable_writes,
                           visibility);
}

std::expected<std::vector<std::byte>, Error> Storage::ReadSave(std::string_view slot) const {
    auto path = ResolveLeaf(save_dir_, slot);
    if (!path) {
        return std::unexpected(path.error());
    }
    return ReadFileLimited(*path, options_.max_save_bytes);
}

std::expected<void, Error> Storage::WriteSave(std::string_view slot,
                                             std::span<const std::byte> data,
                                             Visibility visibility) const {
    auto path = ResolveLeaf(save_dir_, slot);
    if (!path) {
        return std::unexpected(path.error());
    }
    if (data.size() > options_.max_save_bytes) {
        return std::unexpected(Error{.code = ErrorCode::TooLarge,
                                     .message = std::format("save is {} bytes, limit is {}",
                                                            data.size(), options_.max_save_bytes),
                                     .path = *path});
    }
    return WriteFileAtomic(*path, data, options_.durable_writes, visibility);
}

std::expected<std::vector<std::byte>, Error> Storage::ReadCache(std::string_view name) const {
    auto path = ResolveLeaf(roots_.cache, name);
    if (!path) {
        return std::unexpected(path.error());
    }
    return ReadFileLimited(*path, options_.max_cache_bytes);
}

void Storage::WriteCache(std::string_view name, std::span<const std::byte> data) const noexcept {
    // Best effort by contract: a cache that cannot be written is not an error,
    // the artifact is simply rebuilt next run. This is a different durability
    // policy from settings and saves, and the void return type is what encodes
    // it - there is nothing for a caller to handle or report.
    try {
        auto path = ResolveLeaf(roots_.cache, name);
        if (!path || data.size() > options_.max_cache_bytes) {
            return;
        }
        static_cast<void>(WriteFileAtomic(*path, data, /*durable=*/false, Visibility::Normal));
    } catch (...) {
        // A cache write must never be the reason a frame is lost.
    }
}

std::expected<void, Error> Storage::ResetUserData() const {
    // Removes exactly the paths this object owns under the persistent root. It
    // deliberately does not remove_all() the persistent root itself, for two
    // reasons: the cache and log roots are nested inside it for --user-dir and
    // portable mode, so a blanket delete would destroy the cache a reset is
    // supposed to preserve; and in portable mode the persistent root is a
    // directory the user chose and may have their own files in.
    const std::array<std::filesystem::path, 3> owned{config_path_, config_backup_path_, save_dir_};

    std::error_code ec;
    for (const auto& path : owned) {
        std::filesystem::remove_all(path, ec);
        if (ec) {
            return std::unexpected(Error{.code = ErrorCode::WriteFailed,
                                         .message = "cannot remove user data: " + ec.message(),
                                         .path = path});
        }
    }
    // The object stays usable: writes recreate what they need, and recreating
    // the saves directory here makes that explicit for callers that inspect the
    // paths afterwards.
    return EnsureDirectory(save_dir_);
}

} // namespace VulkanShared::Storage
