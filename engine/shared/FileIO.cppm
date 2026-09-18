module;

export module VulkanShared.FileIO;

import std;
import std.compat;

export namespace VulkanShared::FileIO {

// Paths are taken as std::filesystem::path rather than std::string_view: on
// Windows the native path representation is wide (wchar_t), so callers passing
// path::native()/path::c_str() cannot convert to a narrow string_view. path
// accepts narrow input (string literals, std::string) on every platform, so
// existing call sites keep working unchanged.
inline std::vector<std::byte> ReadBinary(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<std::byte> data(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return data;
}

inline void WriteBinary(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

inline void AtomicWrite(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!f) { std::filesystem::remove(tmp); return; }
    }
    std::filesystem::rename(tmp, path);
}

} // namespace VulkanShared::FileIO
