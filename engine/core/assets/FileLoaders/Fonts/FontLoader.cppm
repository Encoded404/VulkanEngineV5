module;

export module VulkanEngine.FileLoaders.Fonts;

import std;
import std.compat;

import FileLoader.Types;

export namespace VulkanEngine::FileLoaders::Fonts {

// The sfnt container flavours the engine accepts. A font file may hold one face
// (TrueType/OpenType) or a collection of them.
//
// This is a container check, not a font validation: it answers "is this
// plausibly an sfnt font, and how many faces does it advertise" so the resource
// layer can reject obvious garbage and report a useful count. Whether the
// outlines, tables, and checksums are actually sound is FreeType's and
// HarfBuzz's job when a face is opened, and duplicating that here would only
// create a second, weaker parser to keep in sync.
enum class FontContainer : std::uint8_t {
    Unknown = 0,        // not an sfnt container, or too short to tell
    TrueType,           // 0x00010000 or 'true': TrueType outlines (glyf/loca)
    OpenTypeCff,        // 'OTTO': PostScript/CFF outlines
    TrueTypeCollection, // 'ttcf': several faces sharing one file
};

struct FontContainerInfo {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    FontContainer container = FontContainer::Unknown;
    // 1 for a single-face container; the advertised face count for a
    // collection; 0 when the container is unknown.
    std::uint16_t face_count = 0;
    // Table directory entry count for a single-face container, 0 for a
    // collection (whose per-face tables live after the face offsets).
    std::uint32_t table_count = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return container != FontContainer::Unknown;
    }
};

// Inspects the sfnt header of `bytes`. Never throws and never reads past
// `bytes`; every truncation is reported as Unknown.
[[nodiscard]] FontContainerInfo InspectFontContainer(FileLoader::ByteSpan bytes);

// Human-readable container name, for diagnostics.
[[nodiscard]] std::string_view FontContainerName(FontContainer container) noexcept;

} // namespace VulkanEngine::FileLoaders::Fonts
