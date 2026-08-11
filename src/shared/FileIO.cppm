module;

export module VulkanShared.FileIO;

import std;
import std.compat;

export namespace VulkanShared::FileIO {

inline std::vector<std::byte> ReadBinary(std::string_view path) {
    std::ifstream f(std::string(path), std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<std::byte> data(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return data;
}

inline void WriteBinary(std::string_view path, std::span<const std::byte> data) {
    std::ofstream f(std::string(path), std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

inline void AtomicWrite(std::string_view path, std::span<const std::byte> data) {
    auto tmp = std::string(path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!f) { std::filesystem::remove(tmp); return; }
    }
    std::filesystem::rename(tmp, path);
}

} // namespace VulkanShared::FileIO
