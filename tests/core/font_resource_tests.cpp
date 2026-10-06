#include <gtest/gtest.h>

import std;
import std.compat;

import FileLoader.Types;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.FontResource;
import VulkanEngine.FileLoaders.Fonts;

namespace {

using VulkanEngine::FileLoaders::Fonts::FontContainer;
using VulkanEngine::FontResource;
using VulkanEngine::ResourceId;
using VulkanEngine::ResourceManager;

[[nodiscard]] std::filesystem::path TestFontPath() {
    return std::filesystem::path{VKENGINE_TEST_FONT_DIR} / "Lato-Regular.ttf";
}

[[nodiscard]] std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

// A scratch file removed on destruction, so a reload test can point at a real
// path without leaving anything behind.
class TempFontFile final {
public:
    explicit TempFontFile(std::string_view extension) {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("ve5_font_resource_" + std::to_string(stamp) + "_" + std::to_string(rd()) +
                 std::string(extension));
    }

    ~TempFontFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    [[nodiscard]] bool Write(std::span<const std::byte> bytes) const {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file);
    }

private:
    std::filesystem::path path_;
};

TEST(FontResourceTest, LoadsAFontThroughTheResourceManager) {
    const std::vector<std::byte> expected = ReadFileBytes(TestFontPath());
    ASSERT_FALSE(expected.empty()) << "missing fixture " << TestFontPath().string();

    ResourceManager manager;
    auto handle = manager.LoadFromFile<FontResource>(
        TestFontPath(), ResourceManager::LoadSpeed::Instant);

    ASSERT_TRUE(handle.IsValid());
    FontResource* font = handle.Get();
    ASSERT_NE(font, nullptr);
    ASSERT_TRUE(font->IsLoaded());

    // The resource keeps the file's bytes verbatim: both FreeType and HarfBuzz
    // parse this same buffer, so nothing may rewrite it in passing.
    EXPECT_TRUE(font->HasBytes());
    ASSERT_EQ(font->GetBytes().size(), expected.size());
    EXPECT_EQ(font->GetBytes(), expected);
    EXPECT_EQ(font->GetByteSpan().size(), expected.size());

    const auto info = font->GetContainerInfo();
    EXPECT_EQ(info.container, FontContainer::TrueType);
    EXPECT_EQ(info.face_count, 1u);
    EXPECT_EQ(font->GetVersion(), 1u);
}

TEST(FontResourceTest, RejectsABufferThatIsNotAFont) {
    FontResource font(ResourceId{.value = "not-a-font"});

    std::vector<std::byte> junk(256, std::byte{0x5A});
    EXPECT_FALSE(font.Load(FileLoader::ByteBuffer{junk}));
    EXPECT_FALSE(font.HasBytes());
    EXPECT_FALSE(font.GetContainerInfo().IsValid());
    EXPECT_EQ(font.GetVersion(), 0u) << "a rejected load must not look like a version bump";
}

TEST(FontResourceTest, RejectsAnEmptyBuffer) {
    FontResource font(ResourceId{.value = "empty"});

    EXPECT_FALSE(font.Load(FileLoader::ByteBuffer{}));
    EXPECT_FALSE(font.HasBytes());
}

// A truncated download is the realistic failure: a valid sfnt magic and table
// count, but fewer bytes than the directory claims.
TEST(FontResourceTest, RejectsATruncatedFont) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_GT(complete.size(), 64u);

    std::vector<std::byte> truncated(complete.begin(), complete.begin() + 32);

    FontResource font(ResourceId{.value = "truncated"});
    EXPECT_FALSE(font.Load(FileLoader::ByteBuffer{truncated}));
    EXPECT_FALSE(font.HasBytes());
}

// Load(buffer) is the initial-load path, so a rejected buffer leaves the
// resource empty rather than half-populated, and does not look like a version
// bump. Preserving a *working* payload across a failed re-read is the
// hot-reload path's guarantee instead (ReloadFromPath, added with the font
// watcher), which decodes into a temporary before touching the live bytes.
TEST(FontResourceTest, ARejectedBufferLeavesTheResourceEmpty) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_FALSE(complete.empty());

    FontResource font(ResourceId{.value = "reload"});
    ASSERT_TRUE(font.Load(FileLoader::ByteBuffer{complete}));
    ASSERT_TRUE(font.HasBytes());
    ASSERT_EQ(font.GetVersion(), 1u);

    std::vector<std::byte> junk(64, std::byte{0x11});
    EXPECT_FALSE(font.Load(FileLoader::ByteBuffer{junk}));

    EXPECT_FALSE(font.HasBytes());
    EXPECT_FALSE(font.GetContainerInfo().IsValid());
    EXPECT_EQ(font.GetVersion(), 1u) << "a rejected load must not bump the version";
}

TEST(FontResourceTest, UnloadReleasesTheBytes) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_FALSE(complete.empty());

    FontResource font(ResourceId{.value = "unload"});
    ASSERT_TRUE(font.Load(FileLoader::ByteBuffer{complete}));

    font.Unload();
    EXPECT_FALSE(font.IsLoaded());
    EXPECT_FALSE(font.HasBytes());
    EXPECT_FALSE(font.GetContainerInfo().IsValid());
}

// ReloadFromPath is the promise the initial-load test above points at: a valid
// re-read replaces the payload and bumps the version. The replacement file is
// Lato plus trailing padding -- still a readable sfnt container, but a different
// size -- so the test proves the new bytes are the ones being served.
TEST(FontResourceTest, ReloadFromPathServesNewBytesAndBumpsVersion) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_FALSE(complete.empty());

    std::vector<std::byte> padded = complete;
    padded.insert(padded.end(), 16, std::byte{0x00});
    const TempFontFile replacement(".ttf");
    ASSERT_TRUE(replacement.Write(padded));

    FontResource font(ResourceId{.value = "reload-valid"});
    ASSERT_TRUE(font.Load(FileLoader::ByteBuffer{complete}));
    ASSERT_EQ(font.GetVersion(), 1u);
    ASSERT_EQ(font.GetBytes().size(), complete.size());

    ASSERT_TRUE(font.ReloadFromPath(replacement.Path()));
    EXPECT_EQ(font.GetVersion(), 2u) << "a valid reload must bump the version";
    EXPECT_EQ(font.GetBytes().size(), padded.size()) << "the reloaded bytes must be served";
    EXPECT_TRUE(font.GetContainerInfo().IsValid());
}

// A corrupt edit fails the reload and leaves the working bytes and version
// exactly as they were.
TEST(FontResourceTest, ReloadFromPathRejectsGarbageAndKeepsTheWorkingPayload) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_FALSE(complete.empty());

    const std::vector<std::byte> junk(256, std::byte{0x5A});
    const TempFontFile garbage(".ttf");
    ASSERT_TRUE(garbage.Write(junk));

    FontResource font(ResourceId{.value = "reload-garbage"});
    ASSERT_TRUE(font.Load(FileLoader::ByteBuffer{complete}));
    ASSERT_EQ(font.GetVersion(), 1u);

    EXPECT_FALSE(font.ReloadFromPath(garbage.Path()));
    EXPECT_TRUE(font.HasBytes());
    EXPECT_EQ(font.GetBytes(), complete) << "a rejected reload must not touch the bytes";
    EXPECT_EQ(font.GetVersion(), 1u) << "a rejected reload must not bump the version";
    EXPECT_EQ(font.GetContainerInfo().container, FontContainer::TrueType);
}

// A truncated font (valid magic, directory beyond the buffer) is rejected the
// same way.
TEST(FontResourceTest, ReloadFromPathRejectsATruncatedFont) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_GT(complete.size(), 64u);
    const std::vector<std::byte> truncated(complete.begin(), complete.begin() + 32);
    const TempFontFile file(".ttf");
    ASSERT_TRUE(file.Write(truncated));

    FontResource font(ResourceId{.value = "reload-truncated"});
    ASSERT_TRUE(font.Load(FileLoader::ByteBuffer{complete}));

    EXPECT_FALSE(font.ReloadFromPath(file.Path()));
    EXPECT_EQ(font.GetBytes(), complete);
    EXPECT_EQ(font.GetVersion(), 1u);
}

// A missing file is a no-op: the live payload is untouched.
TEST(FontResourceTest, ReloadFromPathMissingFileIsANoOp) {
    const std::vector<std::byte> complete = ReadFileBytes(TestFontPath());
    ASSERT_FALSE(complete.empty());

    FontResource font(ResourceId{.value = "reload-missing"});
    ASSERT_TRUE(font.Load(FileLoader::ByteBuffer{complete}));

    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "ve5_font_resource_does_not_exist.ttf";
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    EXPECT_FALSE(font.ReloadFromPath(missing));
    EXPECT_EQ(font.GetBytes(), complete);
    EXPECT_EQ(font.GetVersion(), 1u);
}

} // namespace
