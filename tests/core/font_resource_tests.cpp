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

} // namespace
