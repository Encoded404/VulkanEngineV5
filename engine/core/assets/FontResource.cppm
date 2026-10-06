module;

export module VulkanEngine.ResourceSystem.FontResource;

import std;
import std.compat;

import FileLoader.Types;

import VulkanEngine.ResourceSystem;
import VulkanEngine.FileLoaders.Fonts;

export namespace VulkanEngine {

// A font file held as the raw bytes the file provided.
//
// The bytes are kept verbatim rather than copied into a structure because
// FreeType and HarfBuzz both want the original sfnt buffer: the face is created
// from this memory (FreeType) and the same memory backs the HarfBuzz blob, so a
// face opened from this resource borrows no lifetime it does not own. Parsing
// the container here is limited to what the resource layer needs -- a validity
// gate and a face count -- and deliberately does not duplicate FreeType's
// validation.
class FontResource final : public Resource {
public:
    explicit FontResource(ResourceId id);

    [[nodiscard]] const std::vector<std::byte>& GetBytes() const noexcept;
    [[nodiscard]] FileLoader::ByteSpan GetByteSpan() const noexcept;
    [[nodiscard]] FileLoaders::Fonts::FontContainerInfo GetContainerInfo() const noexcept;
    [[nodiscard]] bool HasBytes() const noexcept;
    // Bumped on every successful (re)load, so a glyph cache can detect that a
    // face it was built from is no longer the current one.
    [[nodiscard]] std::uint32_t GetVersion() const noexcept { return version_; }

    // Re-reads the font from `path` for hot reload.
    //
    // Unlike DoLoadFromBuffer (the initial-load path, which resets first), this
    // reads and validates into a temporary and only then adopts the bytes and
    // bumps the version. A garbage, truncated or missing file therefore returns
    // false and leaves the previous payload and version exactly as they were --
    // a bad edit in an editor can never destroy the font a running frame is
    // using.
    [[nodiscard]] bool ReloadFromPath(const std::filesystem::path& path);

protected:
    bool DoLoad() override;
    bool DoUnload() override;
    bool DoLoadFromBuffer(const FileLoader::ByteBuffer& buf) override;

private:
    void Reset() noexcept;

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<std::byte> bytes_{};
    FileLoaders::Fonts::FontContainerInfo container_{};
    std::uint32_t version_ = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine
