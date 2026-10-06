module;

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>

module VulkanEngine.Text.FontLibrary;

import std;

namespace VulkanEngine::Text {

namespace {

// Reads the linked FreeType's runtime version through a throwaway library
// handle. FT_Library_Version() is the only way to observe the version of the
// binary actually linked, as opposed to the headers that were compiled
// against; a failure to initialize leaves the runtime fields at zero, which
// FreeTypeHeaderMatchesRuntime() reports as a mismatch.
void ReadFreeTypeRuntimeVersion(LibraryVersions& versions) {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        return;
    }
    FT_Int major = 0;
    FT_Int minor = 0;
    FT_Int patch = 0;
    FT_Library_Version(library, &major, &minor, &patch);
    versions.freetype_runtime_major = static_cast<std::uint32_t>(major);
    versions.freetype_runtime_minor = static_cast<std::uint32_t>(minor);
    versions.freetype_runtime_patch = static_cast<std::uint32_t>(patch);
    FT_Done_FreeType(library);
}

} // namespace

LibraryVersions GetLibraryVersions() {
    LibraryVersions versions{};
    versions.freetype_header_major = FREETYPE_MAJOR;
    versions.freetype_header_minor = FREETYPE_MINOR;
    versions.freetype_header_patch = FREETYPE_PATCH;
    ReadFreeTypeRuntimeVersion(versions);
    hb_version(&versions.harfbuzz_major, &versions.harfbuzz_minor,
               &versions.harfbuzz_micro);
    return versions;
}

std::string FormatLibraryVersions() {
    const LibraryVersions versions = GetLibraryVersions();
    std::string summary = "FreeType " +
        std::to_string(versions.freetype_runtime_major) + "." +
        std::to_string(versions.freetype_runtime_minor) + "." +
        std::to_string(versions.freetype_runtime_patch) +
        ", HarfBuzz " +
        std::to_string(versions.harfbuzz_major) + "." +
        std::to_string(versions.harfbuzz_minor) + "." +
        std::to_string(versions.harfbuzz_micro);
    if (!versions.FreeTypeHeaderMatchesRuntime()) {
        summary += " (header " +
            std::to_string(versions.freetype_header_major) + "." +
            std::to_string(versions.freetype_header_minor) + "." +
            std::to_string(versions.freetype_header_patch) +
            " does not match the linked library)";
    }
    return summary;
}

} // namespace VulkanEngine::Text
