module;

#include <cctype>

#include <curl/curl.h>

module Examples.Explorer.Download;

import std;

namespace Examples::Explorer {

namespace {

// ── SHA-256 ─────────────────────────────────────────────────────────────
// A small self-contained implementation: the downloader must verify bytes, and
// pulling a crypto library in for one hash is not worth the dependency.

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

[[nodiscard]] constexpr std::uint32_t Rotr(const std::uint32_t value, const int bits) noexcept {
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    void Update(std::span<const std::byte> bytes) {
        total_ += bytes.size();
        std::size_t offset = 0;
        if (block_size_ != 0) {
            const std::size_t take = std::min(bytes.size(), block_.size() - block_size_);
            std::copy_n(bytes.begin(), take, block_.begin() + static_cast<std::ptrdiff_t>(block_size_));
            block_size_ += take;
            offset += take;
            if (block_size_ == block_.size()) {
                Compress(block_.data());
                block_size_ = 0;
            }
        }
        while (offset + block_.size() <= bytes.size()) {
            Compress(bytes.data() + offset);
            offset += block_.size();
        }
        if (offset < bytes.size()) {
            block_size_ = bytes.size() - offset;
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), block_size_,
                        block_.begin());
        }
    }

    [[nodiscard]] std::string HexFinal() {
        const std::uint64_t bit_length = total_ * 8;
        const std::byte padding = std::byte{0x80};
        Update(std::span<const std::byte>{&padding, 1});
        const std::byte zero = std::byte{0};
        while (block_size_ != 56) {
            Update(std::span<const std::byte>{&zero, 1});
        }
        std::array<std::byte, 8> length{};
        for (int i = 0; i < 8; ++i) {
            length[static_cast<std::size_t>(i)] =
                static_cast<std::byte>((bit_length >> (56 - 8 * i)) & 0xFFu);
        }
        Update(length);

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out.push_back(kHex[(word >> shift) & 0xFu]);
            }
        }
        return out;
    }

private:
    void Compress(const std::byte* block) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const auto base = static_cast<std::size_t>(i) * 4;
            w[static_cast<std::size_t>(i)] =
                (static_cast<std::uint32_t>(block[base]) << 24) |
                (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                static_cast<std::uint32_t>(block[base + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotr(w[static_cast<std::size_t>(i - 15)], 7) ^
                                     Rotr(w[static_cast<std::size_t>(i - 15)], 18) ^
                                     (w[static_cast<std::size_t>(i - 15)] >> 3);
            const std::uint32_t s1 = Rotr(w[static_cast<std::size_t>(i - 2)], 17) ^
                                     Rotr(w[static_cast<std::size_t>(i - 2)], 19) ^
                                     (w[static_cast<std::size_t>(i - 2)] >> 10);
            w[static_cast<std::size_t>(i)] =
                w[static_cast<std::size_t>(i - 16)] + s0 +
                w[static_cast<std::size_t>(i - 7)] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::byte, 64> block_{};
    std::size_t block_size_ = 0;
    std::uint64_t total_ = 0;
};

[[nodiscard]] bool IsHex64(std::string_view text) noexcept {
    if (text.size() != 64) return false;
    return std::ranges::all_of(text, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] std::string Lowercase(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// One transfer's scratch state, shared with the libcurl callbacks.
struct TransferContext {
    std::ofstream* file = nullptr;
    Sha256* sha = nullptr;
    std::atomic<std::uint64_t>* received = nullptr;
    std::atomic<bool>* cancel = nullptr;
    std::uint64_t limit = 0;
};

constexpr std::size_t WriteCallback(char* data, std::size_t size, std::size_t count,
                                    void* userdata) {
    auto* context = static_cast<TransferContext*>(userdata);
    const std::size_t bytes = size * count;
    if (context->cancel->load(std::memory_order_relaxed)) return 0;
    // Refuse to write past the size the catalog promised; a truncated or
    // replaced upstream file must not fill the disk.
    if (bytes > context->limit || context->received->load() + bytes > context->limit) {
        return 0;
    }
    context->file->write(data, static_cast<std::streamsize>(bytes));
    if (!*context->file) return 0;
    context->sha->Update(std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), bytes});
    context->received->fetch_add(bytes, std::memory_order_relaxed);
    return bytes;
}

constexpr int ProgressCallback(void* userdata, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                               curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* context = static_cast<TransferContext*>(userdata);
    return context->cancel->load(std::memory_order_relaxed) ? 1 : 0;
}

void EnsureCurlInitialized() {
    static const bool initialized = [] {
        return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }();
    (void)initialized;
}

[[nodiscard]] std::string FormatBytes(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        return std::format("{:.2f} GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    if (bytes >= 1024ull * 1024ull) {
        return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024ull) {
        return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{} B", bytes);
}

} // namespace

std::string Sha256Hex(std::span<const std::byte> bytes) {
    Sha256 hasher;
    hasher.Update(bytes);
    return hasher.HexFinal();
}

bool IsPermissiveLicense(std::string_view spdx) {
    // Commercial use + redistribution allowed, attribution at most. CC BY-SA is
    // intentionally absent: share-alike on the asset is a decision for the host,
    // not something this downloader should make silently.
    return spdx == "CC0-1.0" || spdx == "CC-BY-4.0" || spdx == "CC-BY-3.0" ||
           spdx == "PUBLIC-DOMAIN";
}

bool IsValidDownloadableAsset(const DownloadableAsset& asset, std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };

    if (asset.id.empty()) return fail("missing id");
    if (asset.id.find_first_of("/\\") != std::string::npos) return fail("id has a path separator");
    if (asset.file_name.empty()) return fail("missing file name");
    if (asset.file_name == "." || asset.file_name == "..") return fail("invalid file name");
    if (asset.file_name.find_first_of("/\\") != std::string::npos) {
        return fail("file name has a path separator");
    }
    if (asset.url.rfind("https://", 0) != 0) return fail("URL is not HTTPS");
    if (!IsPermissiveLicense(asset.license)) {
        return fail("license '" + asset.license + "' is not permissive");
    }
    if (asset.size == 0) return fail("missing size");
    if (!IsHex64(Lowercase(asset.sha256))) return fail("checksum is not 64 lowercase hex digits");
    return true;
}

// ── SceneDownloader ─────────────────────────────────────────────────────

struct SceneDownloader::Impl {
    explicit Impl(std::filesystem::path root_path) : root(std::move(root_path)) {}

    ~Impl() {
        cancel.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
    }

    [[nodiscard]] std::filesystem::path PathFor(const DownloadableAsset& asset) const {
        return root / asset.file_name;
    }

    void Fail(std::string message, DownloadState state, const std::string& asset_id) {
        std::scoped_lock lock(mutex);
        active_id = asset_id;
        error = std::move(message);
        outcome = DownloadOutcome{
            .ok = false,
            .asset_id = asset_id,
            .path = {},
            .error = error,
        };
        this->state.store(static_cast<int>(state), std::memory_order_release);
        running.store(false, std::memory_order_release);
    }

    void Run(const DownloadableAsset asset) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) {
            Fail("could not create the download directory: " + ec.message(), DownloadState::Failed,
                 asset.id);
            return;
        }

        const fs::path part = root / (asset.file_name + ".part");
        fs::remove(part, ec);

        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file) {
            Fail("could not open '" + part.string() + "' for writing", DownloadState::Failed,
                 asset.id);
            return;
        }

        EnsureCurlInitialized();
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            file.close();
            fs::remove(part, ec);
            Fail("libcurl could not be initialized", DownloadState::Failed, asset.id);
            return;
        }

        Sha256 sha;
        TransferContext context{
            .file = &file,
            .sha = &sha,
            .received = &received,
            .cancel = &cancel,
            .limit = asset.size,
        };

        curl_easy_setopt(curl, CURLOPT_URL, asset.url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "VulkanEngineV5-SceneExplorer/0.1 (on-demand asset download)");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);

        const CURLcode result = curl_easy_perform(curl);
        long response_code = 0;
        static_cast<void>(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code));
        curl_easy_cleanup(curl);
        file.close();

        if (cancel.load(std::memory_order_relaxed)) {
            fs::remove(part, ec);
            Fail("cancelled", DownloadState::Cancelled, asset.id);
            return;
        }
        if (result != CURLE_OK) {
            fs::remove(part, ec);
            Fail(std::string{"transfer failed: "} + curl_easy_strerror(result),
                 DownloadState::Failed, asset.id);
            return;
        }
        if (response_code != 200) {
            fs::remove(part, ec);
            Fail("server returned HTTP " + std::to_string(response_code), DownloadState::Failed,
                 asset.id);
            return;
        }

        const std::uint64_t written = received.load(std::memory_order_relaxed);
        if (written != asset.size) {
            fs::remove(part, ec);
            Fail("size mismatch: expected " + FormatBytes(asset.size) + ", received " +
                     FormatBytes(written),
                 DownloadState::Failed, asset.id);
            return;
        }

        const std::string digest = sha.HexFinal();
        if (digest != Lowercase(asset.sha256)) {
            fs::remove(part, ec);
            Fail("checksum mismatch: expected " + Lowercase(asset.sha256) + ", got " + digest,
                 DownloadState::Failed, asset.id);
            return;
        }

        const fs::path final_path = PathFor(asset);
        fs::remove(final_path, ec);
        fs::rename(part, final_path, ec);
        if (ec) {
            fs::remove(part, ec);
            Fail("could not install '" + final_path.string() + "': " + ec.message(),
                 DownloadState::Failed, asset.id);
            return;
        }

        WriteLicenseFile(asset, digest);

        {
            std::scoped_lock lock(mutex);
            error.clear();
            outcome = DownloadOutcome{
                .ok = true,
                .asset_id = asset.id,
                .path = final_path,
                .error = {},
            };
        }
        state.store(static_cast<int>(DownloadState::Done), std::memory_order_release);
        running.store(false, std::memory_order_release);
    }

    void WriteLicenseFile(const DownloadableAsset& asset, const std::string& digest) const {
        std::ofstream license(root / (asset.id + ".license.txt"), std::ios::trunc);
        if (!license) return;
        license << asset.name << '\n'
                << "License: " << asset.license << '\n'
                << "Attribution: " << asset.attribution << '\n'
                << "Source: " << asset.source_page << '\n'
                << "Downloaded from: " << asset.url << '\n'
                << "SHA-256: " << digest << '\n';
    }

    std::filesystem::path root;
    std::vector<DownloadableAsset> catalog{};
    std::jthread worker{};
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<std::uint64_t> received{0};
    std::atomic<int> state{static_cast<int>(DownloadState::Idle)};
    mutable std::mutex mutex{};
    std::string error{};
    std::string active_id{};
    std::optional<DownloadOutcome> outcome{};
};

SceneDownloader::SceneDownloader(std::filesystem::path cache_root)
    : impl_(std::make_unique<Impl>(std::move(cache_root))) {}

SceneDownloader::~SceneDownloader() = default;

void SceneDownloader::SetCatalog(std::span<const DownloadableAsset> catalog) {
    impl_->catalog.assign(catalog.begin(), catalog.end());
}

const std::vector<DownloadableAsset>& SceneDownloader::Catalog() const {
    return impl_->catalog;
}

const std::filesystem::path& SceneDownloader::Root() const noexcept {
    return impl_->root;
}

std::filesystem::path SceneDownloader::InstallPath(const DownloadableAsset& asset) const {
    return impl_->PathFor(asset);
}

std::filesystem::path SceneDownloader::LicensePath(const DownloadableAsset& asset) const {
    return impl_->root / (asset.id + ".license.txt");
}

bool SceneDownloader::IsInstalled(const DownloadableAsset& asset) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(impl_->PathFor(asset), ec)) return false;
    const auto size = std::filesystem::file_size(impl_->PathFor(asset), ec);
    return !ec && size == asset.size;
}

std::vector<std::filesystem::path> SceneDownloader::InstalledFiles() const {
    std::vector<std::filesystem::path> files{};
    std::error_code ec;
    if (!std::filesystem::is_directory(impl_->root, ec)) return files;

    for (auto it = std::filesystem::directory_iterator(impl_->root, ec);
         it != std::default_sentinel; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string extension = it->path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (extension == ".glb" || extension == ".gltf") files.push_back(it->path());
    }
    std::ranges::sort(files);
    return files;
}

bool SceneDownloader::Start(const DownloadableAsset& asset) {
    std::string error{};
    if (!IsValidDownloadableAsset(asset, &error)) {
        impl_->Fail("invalid catalog entry: " + error, DownloadState::Failed, asset.id);
        return false;
    }
    if (impl_->running.load(std::memory_order_acquire)) return false;
    if (IsInstalled(asset)) return false;
    if (impl_->worker.joinable()) impl_->worker.join();

    impl_->cancel.store(false, std::memory_order_relaxed);
    impl_->received.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->error.clear();
        impl_->active_id = asset.id;
        impl_->outcome.reset();
    }
    impl_->state.store(static_cast<int>(DownloadState::Running), std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::jthread([impl = impl_.get(), asset] { impl->Run(asset); });
    return true;
}

void SceneDownloader::Cancel() {
    impl_->cancel.store(true, std::memory_order_relaxed);
}

DownloadProgress SceneDownloader::Progress() const {
    DownloadProgress progress{};
    progress.state = static_cast<DownloadState>(impl_->state.load(std::memory_order_acquire));
    progress.bytes_received = impl_->received.load(std::memory_order_relaxed);
    {
        std::scoped_lock lock(impl_->mutex);
        progress.asset_id = impl_->active_id;
        progress.error = impl_->error;
    }
    if (!impl_->catalog.empty()) {
        for (const auto& asset : impl_->catalog) {
            if (asset.id == progress.asset_id) progress.bytes_total = asset.size;
        }
    }
    return progress;
}

std::optional<DownloadOutcome> SceneDownloader::TakeOutcome() {
    std::scoped_lock lock(impl_->mutex);
    auto outcome = std::move(impl_->outcome);
    impl_->outcome.reset();
    return outcome;
}

bool SceneDownloader::Remove(const DownloadableAsset& asset) {
    if (impl_->running.load(std::memory_order_acquire)) {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->active_id == asset.id) return false;
    }
    std::error_code ec;
    std::filesystem::remove(impl_->PathFor(asset), ec);
    std::filesystem::remove(impl_->root / (asset.file_name + ".part"), ec);
    std::filesystem::remove(LicensePath(asset), ec);
    return !ec;
}

} // namespace Examples::Explorer
