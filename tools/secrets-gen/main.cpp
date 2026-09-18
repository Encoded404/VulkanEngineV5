// ─────────────────────────────────────────────────────────────────────────────
// secrets-gen: build-time secrets sealer.
//
// Discovers every file in a per-example secrets folder, seals each with
// DataCipher's versioned AEAD envelope, and emits a C++ module that exposes
// runtime accessors. The sealing key ring is embedded too, because the client
// has to open the blobs; treat this as obfuscation-with-integrity, not secrecy.
//
// The envelope format lives in engine/security/cipher_format.hpp, shared with the
// runtime VulkanSecurity target, so generator and runtime cannot drift.
//
// Files whose name starts with '.' and the manifest/README are skipped, so the
// keyring can live inside the same folder without sealing itself.
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "monocypher.h"
#include "engine/security/cipher_format.hpp"

namespace fs = std::filesystem;
namespace fmt = ::vkengine::security::format;

namespace {

constexpr std::uint8_t kDefaultKeyId = 1;
constexpr fmt::CipherVariant kDefaultVariant = fmt::CipherVariant::XChaCha20Poly1305;

struct Options {
    fs::path in_dir{};
    fs::path out_file{};
    fs::path keyring_file{};
    std::string module_name{};
    std::string namespace_name{};
    fmt::CipherVariant default_variant = kDefaultVariant;
    std::uint8_t default_key_id = kDefaultKeyId;
};

struct ManifestEntry {
    fmt::CipherVariant variant = kDefaultVariant;
    std::uint8_t key_id = kDefaultKeyId;
    std::string aad{};
};

struct SealedEntry {
    std::string name;
    std::string aad;
    fmt::CipherVariant variant = kDefaultVariant;
    std::uint8_t key_id = kDefaultKeyId;
    std::vector<std::uint8_t> sealed;
};

struct KeyMaterial {
    std::uint8_t id = 0;
    std::array<std::uint8_t, fmt::kKeyBytes> bytes{};
};

[[nodiscard]] bool ParseVariant(std::string_view text, fmt::CipherVariant& out) {
    if (text == "1") { out = fmt::CipherVariant::XChaCha20Poly1305; return true; }
    if (text == "2") { out = fmt::CipherVariant::ChaCha20Poly1305Ietf; return true; }
    return false;
}

[[nodiscard]] bool IsHex(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

[[nodiscard]] std::uint8_t HexNibble(char c) {
    if (c >= '0' && c <= '9') { return static_cast<std::uint8_t>(c - '0'); }
    if (c >= 'a' && c <= 'f') { return static_cast<std::uint8_t>(c - 'a' + 10); }
    return static_cast<std::uint8_t>(c - 'A' + 10);
}

[[nodiscard]] std::optional<std::array<std::uint8_t, fmt::kKeyBytes>> ParseHexKey(std::string_view text) {
    constexpr std::size_t kHexLen = fmt::kKeyBytes * 2;
    if (text.size() != kHexLen || !IsHex(text)) {
        return std::nullopt;
    }
    std::array<std::uint8_t, fmt::kKeyBytes> key{};
    for (std::size_t i = 0; i < fmt::kKeyBytes; ++i) {
        key[i] = static_cast<std::uint8_t>((HexNibble(text[i * 2]) << 4) | HexNibble(text[i * 2 + 1]));
    }
    return key;
}

[[nodiscard]] std::string Trim(std::string_view line) {
    std::size_t begin = 0;
    while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
        ++begin;
    }
    std::size_t end = line.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
        --end;
    }
    return std::string{line.substr(begin, end - begin)};
}

// First non-empty, non-comment line, trimmed. Sealed text files are allowed to
// carry comments (the committed templates do), so the value is the first line
// that is not a comment rather than the whole file.
[[nodiscard]] std::string FirstContentLine(std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
        const std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            return trimmed;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

[[nodiscard]] std::vector<KeyMaterial> LoadKeyRing(const fs::path& path, bool& ok) {
    std::vector<KeyMaterial> keys;
    ok = false;
    std::ifstream stream(path);
    if (!stream) {
        return keys;
    }

    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        std::istringstream fields(trimmed);
        std::string id_text;
        std::string key_text;
        fields >> id_text >> key_text;
        const int id = std::atoi(id_text.c_str());
        const std::optional<std::array<std::uint8_t, fmt::kKeyBytes>> key = ParseHexKey(key_text);
        if (id < 0 || id > 255 || !key.has_value()) {
            std::cerr << "secrets-gen: bad keyring line: " << trimmed << "\n";
            return {};
        }
        keys.push_back(KeyMaterial{static_cast<std::uint8_t>(id), *key});
    }
    ok = !keys.empty();
    return keys;
}

[[nodiscard]] std::map<std::string, ManifestEntry> LoadManifest(const fs::path& path) {
    std::map<std::string, ManifestEntry> manifest;
    std::ifstream stream(path);
    if (!stream) {
        return manifest;
    }

    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        std::istringstream fields(trimmed);
        std::string name;
        std::string variant_text;
        std::string key_text;
        std::string aad;
        fields >> name;
        ManifestEntry entry{};
        if (fields >> variant_text) {
            if (!ParseVariant(variant_text, entry.variant)) {
                std::cerr << "secrets-gen: manifest: unknown variant '" << variant_text
                          << "' for " << name << "\n";
            }
        }
        if (fields >> key_text) {
            entry.key_id = static_cast<std::uint8_t>(std::atoi(key_text.c_str()));
        }
        if (fields >> aad) {
            entry.aad = aad;
        }
        manifest[name] = entry;
    }
    return manifest;
}

// Deterministic nonce: BLAKE2b over the logical inputs. Identical inputs reuse
// the same nonce, which is safe (same message) and keeps generated artifacts
// byte-reproducible so the generated module does not churn the build.
[[nodiscard]] std::vector<std::uint8_t> DeriveNonce(const SealedEntry& entry,
                                                    std::span<const std::uint8_t> plaintext,
                                                    std::size_t nonce_bytes) {
    std::vector<std::uint8_t> input;
    input.reserve(entry.name.size() + plaintext.size() + 4);
    input.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(entry.variant) & 0xFFU));
    input.push_back(static_cast<std::uint8_t>((static_cast<std::uint16_t>(entry.variant) >> 8) & 0xFFU));
    input.push_back(entry.key_id);
    input.insert(input.end(), entry.name.begin(), entry.name.end());
    input.insert(input.end(), plaintext.begin(), plaintext.end());

    std::array<std::uint8_t, 32> digest{};
    crypto_blake2b(digest.data(), digest.size(), input.data(), input.size());

    std::vector<std::uint8_t> nonce(nonce_bytes);
    std::memcpy(nonce.data(), digest.data(), nonce_bytes);
    return nonce;
}

[[nodiscard]] std::vector<std::uint8_t> ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

void EmitBytes(std::ostream& out, std::span<const std::uint8_t> bytes, std::string_view indent) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i % 12 == 0) {
            out << indent;
        }
        out << "std::byte{0x";
        const char* hex = "0123456789abcdef";
        out << hex[(bytes[i] >> 4) & 0xF] << hex[bytes[i] & 0xF] << "},";
        if (i % 12 == 11 || i + 1 == bytes.size()) {
            out << "\n";
        }
    }
    if (bytes.empty()) {
        out << indent << "// empty\n";
    }
}

[[nodiscard]] std::string Escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

[[nodiscard]] int Generate(const Options& options,
                           const std::vector<KeyMaterial>& keys,
                           const std::vector<SealedEntry>& entries,
                           bool embedded_keyring) {
    std::error_code ec;
    if (!options.out_file.parent_path().empty()) {
        fs::create_directories(options.out_file.parent_path(), ec);
    }

    std::ofstream out(options.out_file, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "secrets-gen: cannot write " << options.out_file << "\n";
        return 1;
    }

    out << "// GENERATED by secrets-gen. Do not edit and do not commit.\n";
    out << "// Sealed from the per-example secrets folder.\n";
    out << "module;\n\n";
    out << "export module " << options.module_name << ";\n\n";
    out << "import std;\n\n";
    out << "import VulkanEngine.DataCipher;\n\n";

    // ── embedded key ring + sealed blobs ──
    // Module-scope anonymous namespace: internal linkage, so it cannot sit
    // inside the exported namespace (that is a compile error).
    out << "namespace {\n\n";
    out << "struct KeyEntry { std::uint8_t id; std::array<std::byte, "
        << fmt::kKeyBytes << "> bytes; };\n";
    out << "constexpr std::array<KeyEntry, " << keys.size() << "> kKeys = {{\n";
    for (const KeyMaterial& key : keys) {
        out << "    {" << static_cast<unsigned>(key.id) << ", {";
        for (std::size_t i = 0; i < key.bytes.size(); ++i) {
            if (i > 0) { out << ", "; }
            out << "std::byte{0x" << "0123456789abcdef"[(key.bytes[i] >> 4) & 0xF]
                << "0123456789abcdef"[key.bytes[i] & 0xF] << "}";
        }
        out << "}},\n";
    }
    out << "}};\n\n";

    // ── sealed blobs ──
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out << "constexpr std::array<std::byte, " << entries[i].sealed.size()
            << "> kBlob" << i << " = {\n";
        EmitBytes(out, entries[i].sealed, "    ");
        out << "};\n\n";
    }

    out << "} // namespace\n\n";

    // ── public interface ──
    out << "export namespace " << options.namespace_name << " {\n\n";
    out << "struct Entry {\n";
    out << "    std::string_view name;\n";
    out << "    std::string_view aad;\n";
    out << "    std::uint16_t cipher_variant;\n";
    out << "    std::uint8_t key_id;\n";
    out << "    std::span<const std::byte> sealed;\n";
    out << "};\n\n";

    out << "inline constexpr std::array<Entry, " << entries.size() << "> kEntries = {{\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const SealedEntry& entry = entries[i];
        out << "    {\"" << Escape(entry.name) << "\", \"" << Escape(entry.aad) << "\", "
            << static_cast<unsigned>(static_cast<std::uint16_t>(entry.variant)) << ", "
            << static_cast<unsigned>(entry.key_id) << ", std::span<const std::byte>(kBlob"
            << i << ")},\n";
    }
    out << "}};\n\n";

    out << "[[nodiscard]] inline std::span<const Entry> Entries() noexcept { return kEntries; }\n\n";
    out << "[[nodiscard]] inline const Entry* Find(std::string_view name) noexcept {\n";
    out << "    for (const Entry& entry : kEntries) {\n";
    out << "        if (entry.name == name) { return &entry; }\n";
    out << "    }\n";
    out << "    return nullptr;\n";
    out << "}\n\n";

    out << "[[nodiscard]] inline VulkanEngine::Security::KeyRing BuildKeyRing() {\n";
    out << "    VulkanEngine::Security::KeyRing ring;\n";
    out << "    for (const KeyEntry& key : kKeys) {\n";
    out << "        ring.Add(key.id, key.bytes);\n";
    out << "    }\n";
    out << "    return ring;\n";
    out << "}\n\n";

    out << "[[nodiscard]] inline std::optional<std::vector<std::byte>> GetRaw(std::string_view name) {\n";
    out << "    static const VulkanEngine::Security::KeyRing ring = BuildKeyRing();\n";
    out << "    const Entry* entry = Find(name);\n";
    out << "    if (entry == nullptr) { return std::nullopt; }\n";
    out << "    const std::span<const std::byte> aad{reinterpret_cast<const std::byte*>(entry->aad.data()), entry->aad.size()};\n";
    out << "    return VulkanEngine::Security::Open(ring, entry->sealed, aad);\n";
    out << "}\n\n";

    out << "[[nodiscard]] inline std::optional<std::string> GetString(std::string_view name) {\n";
    out << "    const std::optional<std::vector<std::byte>> raw = GetRaw(name);\n";
    out << "    if (!raw.has_value()) { return std::nullopt; }\n";
    out << "    return std::string{reinterpret_cast<const char*>(raw->data()), raw->size()};\n";
    out << "}\n\n";

    out << "[[nodiscard]] inline std::optional<std::int64_t> GetInt(std::string_view name) {\n";
    out << "    const std::optional<std::string> text = GetString(name);\n";
    out << "    if (!text.has_value()) { return std::nullopt; }\n";
    out << "    std::int64_t value = 0;\n";
    out << "    const auto [ptr, ec] = std::from_chars(text->data(), text->data() + text->size(), value);\n";
    out << "    if (ec != std::errc{} || ptr != text->data() + text->size()) { return std::nullopt; }\n";
    out << "    return value;\n";
    out << "}\n\n";

    out << "} // namespace " << options.namespace_name << "\n";
    out << "\n// keyring embedded: " << (embedded_keyring ? "yes" : "no") << "\n";
    return 0;
}

[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0xF]);
        out.push_back(kDigits[byte & 0xF]);
    }
    return out;
}

// The client pins the server's public key. Keeping that key as a separate
// hand-copied file is a footgun: any server that starts with a fresh identity
// stops matching. When the server's private identity is present in the folder,
// derive the public half from it so the two cannot disagree.
[[nodiscard]] std::optional<std::string> DeriveServerPublicKeyHex(const fs::path& identity_path) {
    if (!fs::exists(identity_path)) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> contents = ReadFile(identity_path);
    const std::string text = Trim(std::string_view{
        reinterpret_cast<const char*>(contents.data()), contents.size()});
    const std::optional<std::array<std::uint8_t, fmt::kKeyBytes>> secret = ParseHexKey(text);
    if (!secret.has_value()) {
        std::cerr << "secrets-gen: " << identity_path.string()
                  << " is not a 32-byte hex identity; ignoring it\n";
        return std::nullopt;
    }
    std::array<std::uint8_t, fmt::kKeyBytes> public_key{};
    crypto_x25519_public_key(public_key.data(), secret->data());
    return ToHex(public_key);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    bool have_keyring_path = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) { return false; }
            out = argv[++i];
            return true;
        };
        std::string value;
        if (arg == "--in") {
            if (!next(value)) { return 2; }
            options.in_dir = value;
        } else if (arg == "--out") {
            if (!next(value)) { return 2; }
            options.out_file = value;
        } else if (arg == "--keyring") {
            if (!next(value)) { return 2; }
            options.keyring_file = value;
            have_keyring_path = true;
        } else if (arg == "--module") {
            if (!next(value)) { return 2; }
            options.module_name = value;
        } else if (arg == "--namespace") {
            if (!next(value)) { return 2; }
            options.namespace_name = value;
        } else if (arg == "--default-key-id") {
            if (!next(value)) { return 2; }
            options.default_key_id = static_cast<std::uint8_t>(std::atoi(value.c_str()));
        } else if (arg == "--default-variant") {
            if (!next(value)) { return 2; }
            if (!ParseVariant(value, options.default_variant)) {
                std::cerr << "secrets-gen: unknown --default-variant " << value << "\n";
                return 2;
            }
        } else {
            std::cerr << "secrets-gen: unknown argument " << arg << "\n";
            return 2;
        }
    }

    if (options.out_file.empty() || options.module_name.empty() || options.namespace_name.empty()) {
        std::cerr << "secrets-gen: --out, --module and --namespace are required\n";
        return 2;
    }

    bool keyring_ok = false;
    std::vector<KeyMaterial> keys;
    if (have_keyring_path) {
        keys = LoadKeyRing(options.keyring_file, keyring_ok);
    }

    std::vector<SealedEntry> entries;
    const std::map<std::string, ManifestEntry> manifest =
        options.in_dir.empty() ? std::map<std::string, ManifestEntry>{}
                              : LoadManifest(options.in_dir / "manifest.txt");

    if (keyring_ok && !options.in_dir.empty() && fs::exists(options.in_dir)) {
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(options.in_dir)) {
            if (!entry.is_regular_file()) { continue; }
            const std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.' || name == "manifest.txt" || name == "README.md") {
                continue;
            }
            // Committed templates and docs are not secrets.
            if (name.ends_with(".example") || name.ends_with(".md")) {
                continue;
            }
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());

        const std::string pubkey_name = "leaderboard_server_pubkey.txt";
        const std::optional<std::string> derived_pubkey =
            DeriveServerPublicKeyHex(options.in_dir / ".server_identity");
        bool saw_pubkey_file = false;

        const auto seal_one = [&](const std::string& name, const std::string& aad,
                                  fmt::CipherVariant variant, std::uint8_t key_id,
                                  const std::vector<std::uint8_t>& plaintext)
            -> std::optional<SealedEntry> {
            const fmt::CipherSpec* spec = fmt::GetSpec(variant);
            if (spec == nullptr || variant == fmt::CipherVariant::None) {
                std::cerr << "secrets-gen: unusable variant for " << name << "\n";
                return std::nullopt;
            }
            const auto key_it = std::find_if(keys.begin(), keys.end(),
                                             [&](const KeyMaterial& k) { return k.id == key_id; });
            if (key_it == keys.end()) {
                std::cerr << "secrets-gen: no key id " << static_cast<unsigned>(key_id)
                          << " for " << name << "\n";
                return std::nullopt;
            }

            SealedEntry entry;
            entry.name = name;
            entry.aad = aad;
            entry.variant = variant;
            entry.key_id = key_id;
            const std::vector<std::uint8_t> nonce = DeriveNonce(entry, plaintext, spec->nonce_bytes);
            const std::span<const std::uint8_t> aad_bytes{
                reinterpret_cast<const std::uint8_t*>(entry.aad.data()), entry.aad.size()};
            const std::span<const std::uint8_t> key_bytes(key_it->bytes);
            entry.sealed = fmt::Seal(variant, key_id, key_bytes, nonce, plaintext, aad_bytes);
            if (entry.sealed.empty()) {
                std::cerr << "secrets-gen: failed to seal " << name << "\n";
                return std::nullopt;
            }
            return entry;
        };

        for (const fs::path& file : files) {
            const std::string name = file.filename().string();
            fmt::CipherVariant variant = options.default_variant;
            std::uint8_t key_id = options.default_key_id;
            std::string aad = name;

            if (const auto it = manifest.find(name); it != manifest.end()) {
                variant = it->second.variant;
                key_id = it->second.key_id;
                if (!it->second.aad.empty()) { aad = it->second.aad; }
            }

            std::vector<std::uint8_t> plaintext = ReadFile(file);
            if (name == pubkey_name) {
                saw_pubkey_file = true;
                // An explicit file is a deliberate pin and always wins; the
                // identity is only a convenience when there is no file.
                const std::string provided = FirstContentLine(std::string_view{
                    reinterpret_cast<const char*>(plaintext.data()), plaintext.size()});
                if (derived_pubkey.has_value() && provided != *derived_pubkey) {
                    std::cerr << "secrets-gen: warning: " << pubkey_name
                              << " (" << provided.substr(0, 16)
                              << "...) differs from the public key of .server_identity ("
                              << derived_pubkey->substr(0, 16)
                              << "...); pinning the explicit file, so the server must use a "
                                 "different identity\n";
                } else {
                    std::cerr << "secrets-gen: pinning " << pubkey_name << " ("
                              << provided.substr(0, 16) << "...)\n";
                }
                // Normalise away any trailing newline or stray whitespace so the
                // runtime hex parse always sees exactly 64 characters.
                plaintext.assign(provided.begin(), provided.end());
            }

            std::optional<SealedEntry> entry = seal_one(name, aad, variant, key_id, plaintext);
            if (!entry.has_value()) {
                return 1;
            }
            entries.push_back(std::move(*entry));
        }

        if (derived_pubkey.has_value() && !saw_pubkey_file) {
            const std::vector<std::uint8_t> plaintext(derived_pubkey->begin(), derived_pubkey->end());
            std::optional<SealedEntry> entry = seal_one(pubkey_name, pubkey_name,
                                                        options.default_variant,
                                                        options.default_key_id, plaintext);
            if (!entry.has_value()) {
                return 1;
            }
            std::cerr << "secrets-gen: derived " << pubkey_name << " from .server_identity\n";
            entries.push_back(std::move(*entry));
        }
    } else if (!have_keyring_path || options.in_dir.empty() || !fs::exists(options.in_dir)) {
        std::cerr << "secrets-gen: no usable secrets folder/keyring; emitting stub module\n";
    } else if (!keyring_ok) {
        std::cerr << "secrets-gen: keyring missing or empty; emitting stub module\n";
    }

    return Generate(options, keys, entries, keyring_ok && !entries.empty());
}
