module;

// On-demand scene downloads for the shared explorer.
//
// The binary contains only a catalog of pinned HTTPS URLs plus a SHA-256 for
// each asset; the bytes are fetched when the user asks for them. Downloads land
// in the per-user cache (never next to the executable), are verified before
// they become visible, and are installed atomically. Nothing here knows about
// rendering, so it is testable without a Vulkan device.
export module Examples.Explorer.Download;

import std;

export namespace Examples::Explorer {

// One downloadable asset. `url` must be an immutable, pinned HTTPS address (a
// commit SHA, not a branch) so the checksum stays meaningful; `sha256` is
// lowercase hex over the exact bytes that URL serves.
struct DownloadableAsset {
    std::string id{};
    std::string name{};
    std::string category{};     // grouping shown by the asset library, e.g. "Materials"
    std::string summary{};
    std::string license{};      // SPDX identifier; only permissive ones are accepted
    std::string attribution{};  // what the credits/UI must show for CC BY
    std::string source_page{};  // human-readable original page
    std::string url{};
    std::string file_name{};  // single path component, e.g. "toy-car.glb"
    std::uint64_t size = 0;
    std::string sha256{};
};

enum class DownloadState : std::uint8_t {
    Idle,
    Running,
    Done,
    Failed,
    Cancelled,
};

struct DownloadProgress {
    DownloadState state = DownloadState::Idle;
    std::string asset_id{};
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_total = 0;
    std::string error{};

    [[nodiscard]] float Fraction() const noexcept {
        if (bytes_total == 0) return 0.0f;
        return std::clamp(static_cast<float>(bytes_received) /
                              static_cast<float>(bytes_total),
                          0.0f, 1.0f);
    }
};

struct DownloadOutcome {
    bool ok = false;
    std::string asset_id{};
    std::filesystem::path path{};
    std::string error{};
};

// Lowercase hex SHA-256 of `bytes`.
[[nodiscard]] std::string Sha256Hex(std::span<const std::byte> bytes);

// The license policy the downloader enforces: commercial use and redistribution
// allowed, attribution at most. Non-commercial, share-alike and proprietary
// (Cryengine EULA, "testing only", Sketchfab standard) are rejected outright.
[[nodiscard]] bool IsPermissiveLicense(std::string_view spdx);

// Validates a catalog entry: pinned HTTPS URL, safe single-component file name,
// permissive license, 64-hex checksum, non-zero size.
[[nodiscard]] bool IsValidDownloadableAsset(const DownloadableAsset& asset,
                                            std::string* error = nullptr);

class SceneDownloader {
public:
    explicit SceneDownloader(std::filesystem::path cache_root);
    ~SceneDownloader();

    SceneDownloader(const SceneDownloader&) = delete;
    SceneDownloader& operator=(const SceneDownloader&) = delete;

    void SetCatalog(std::span<const DownloadableAsset> catalog);
    [[nodiscard]] const std::vector<DownloadableAsset>& Catalog() const;

    [[nodiscard]] const std::filesystem::path& Root() const noexcept;

    // True when the installed file exists at the expected size. The checksum is
    // verified at download time, not on every startup (that would mean hashing
    // hundreds of megabytes on launch).
    [[nodiscard]] bool IsInstalled(const DownloadableAsset& asset) const;
    [[nodiscard]] std::filesystem::path InstallPath(const DownloadableAsset& asset) const;
    [[nodiscard]] std::filesystem::path LicensePath(const DownloadableAsset& asset) const;

    // Installed assets under the cache root (top level, .glb/.gltf only).
    [[nodiscard]] std::vector<std::filesystem::path> InstalledFiles() const;

    // Starts one download on a worker thread. Returns false when another one is
    // running, the asset is already installed, or the entry is invalid.
    bool Start(const DownloadableAsset& asset);
    void Cancel();

    [[nodiscard]] DownloadProgress Progress() const;

    // Consumes the completion record exactly once, so the UI reacts to a
    // finished download in the frame it lands.
    [[nodiscard]] std::optional<DownloadOutcome> TakeOutcome();

    // Deletes the installed file and its license metadata. Refuses while that
    // asset is downloading.
    bool Remove(const DownloadableAsset& asset);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Examples::Explorer
