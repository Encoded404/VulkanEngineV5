module;

export module VulkanEngine.Text.FontLibrary;

import std;

export namespace VulkanEngine::Text {

// Versions of the text stack this binary is linked against.
//
// FreeType reports a header version and a runtime version separately. They are
// equal for a normal vcpkg build and differ only when the linked library is not
// the one the headers came from, which is exactly the case worth reporting.
// HarfBuzz exposes its runtime version directly.
//
// msdfgen has no version API at all, so it is deliberately absent here; its
// link is proven by the MTSDF generation test rather than by a version string.
struct LibraryVersions {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t freetype_header_major = 0;
    std::uint32_t freetype_header_minor = 0;
    std::uint32_t freetype_header_patch = 0;
    std::uint32_t freetype_runtime_major = 0;
    std::uint32_t freetype_runtime_minor = 0;
    std::uint32_t freetype_runtime_patch = 0;
    std::uint32_t harfbuzz_major = 0;
    std::uint32_t harfbuzz_minor = 0;
    std::uint32_t harfbuzz_micro = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // False only when the runtime FreeType differs from the headers, or when
    // no FreeType library could be initialized at all.
    [[nodiscard]] bool FreeTypeHeaderMatchesRuntime() const noexcept {
        return freetype_runtime_major != 0 &&
               freetype_header_major == freetype_runtime_major &&
               freetype_header_minor == freetype_runtime_minor &&
               freetype_header_patch == freetype_runtime_patch;
    }
};

// Initializes and tears down a temporary FreeType library to read the runtime
// version. Process-global font state lives in the library pool, not here, so
// this call has no lifetime coupling with the rest of the engine.
[[nodiscard]] LibraryVersions GetLibraryVersions();

// One-line summary for startup diagnostics, e.g. "FreeType 2.13.3, HarfBuzz
// 14.3.0", with an explicit warning suffix on a header/runtime mismatch.
[[nodiscard]] std::string FormatLibraryVersions();

} // namespace VulkanEngine::Text
