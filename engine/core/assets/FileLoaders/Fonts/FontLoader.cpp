module;

module VulkanEngine.FileLoaders.Fonts;

import std;
import std.compat;

import FileLoader.Types;

namespace VulkanEngine::FileLoaders::Fonts {

namespace {

// sfnt files are big-endian throughout.
[[nodiscard]] std::uint8_t ReadU8(FileLoader::ByteSpan bytes, std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint16_t ReadU16(FileLoader::ByteSpan bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(ReadU8(bytes, offset)) << 8) |
        static_cast<std::uint16_t>(ReadU8(bytes, offset + 1)));
}

[[nodiscard]] std::uint32_t ReadU32(FileLoader::ByteSpan bytes, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(ReadU8(bytes, offset)) << 24) |
           (static_cast<std::uint32_t>(ReadU8(bytes, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(ReadU8(bytes, offset + 2)) << 8) |
           static_cast<std::uint32_t>(ReadU8(bytes, offset + 3));
}

[[nodiscard]] constexpr std::uint32_t Tag(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

constexpr std::uint32_t kSfntVersionTrueType = 0x00010000U;
constexpr std::uint32_t kSfntVersionOtto = Tag('O', 'T', 'T', 'O');
constexpr std::uint32_t kSfntVersionAppleTrue = Tag('t', 'r', 'u', 'e');
constexpr std::uint32_t kSfntVersionCollection = Tag('t', 't', 'c', 'f');

// A real font has on the order of ten tables. The cap only exists so a
// corrupt or hostile count cannot demand a huge directory bounds check; it is
// far above anything a genuine font ships.
constexpr std::uint32_t kMaxPlausibleTables = 4096U;
constexpr std::size_t kSfntHeaderSize = 12U;
constexpr std::size_t kTableDirectoryEntrySize = 16U;
constexpr std::size_t kCollectionHeaderSize = 12U;
constexpr std::size_t kCollectionFaceOffsetSize = 4U;

} // namespace

FontContainerInfo InspectFontContainer(FileLoader::ByteSpan bytes) {
    FontContainerInfo info{};

    if (bytes.size() < kSfntHeaderSize) {
        return info;
    }

    const std::uint32_t version = ReadU32(bytes, 0);

    if (version == kSfntVersionCollection) {
        // ttcf: version, face count, then one offset per face. The offsets are
        // not followed here -- opening a face is FreeType's job -- but the
        // offset array itself must be present, or the file is truncated.
        const std::uint32_t face_count = ReadU32(bytes, 8);
        if (face_count == 0 || face_count > 0xFFFFU) {
            return info;
        }
        const std::size_t offsets_end =
            kCollectionHeaderSize + (static_cast<std::size_t>(face_count) * kCollectionFaceOffsetSize);
        if (bytes.size() < offsets_end) {
            return info;
        }
        info.container = FontContainer::TrueTypeCollection;
        info.face_count = static_cast<std::uint16_t>(face_count);
        return info;
    }

    if (version != kSfntVersionTrueType && version != kSfntVersionOtto &&
        version != kSfntVersionAppleTrue) {
        return info;
    }

    const std::uint16_t table_count = ReadU16(bytes, 4);
    if (table_count == 0 || table_count > kMaxPlausibleTables) {
        return info;
    }

    // The table directory must fit in the buffer. This is the cheapest check
    // that separates a whole font from a truncated download.
    const std::size_t directory_end =
        kSfntHeaderSize + (static_cast<std::size_t>(table_count) * kTableDirectoryEntrySize);
    if (bytes.size() < directory_end) {
        return info;
    }

    info.container = version == kSfntVersionOtto ? FontContainer::OpenTypeCff
                                                : FontContainer::TrueType;
    info.face_count = 1;
    info.table_count = table_count;
    return info;
}

std::string_view FontContainerName(FontContainer container) noexcept {
    switch (container) {
    case FontContainer::TrueType:           return "TrueType";
    case FontContainer::OpenTypeCff:        return "OpenType/CFF";
    case FontContainer::TrueTypeCollection: return "TrueType collection";
    case FontContainer::Unknown:            break;
    }
    return "unknown";
}

} // namespace VulkanEngine::FileLoaders::Fonts
