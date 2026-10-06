#include <gtest/gtest.h>

import std;
import std.compat;

import FileLoader.Types;
import VulkanEngine.FileLoaders.Fonts;

namespace {

using VulkanEngine::FileLoaders::Fonts::FontContainer;
using VulkanEngine::FileLoaders::Fonts::FontContainerInfo;

constexpr std::uint32_t Tag(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

void WriteU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8) & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xFFU);
}

void WriteU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 24) & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFU);
    bytes[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFU);
    bytes[offset + 3] = static_cast<std::byte>(value & 0xFFU);
}

// A single-face sfnt header plus a full (zeroed) table directory. Zeroed table
// records are intentional: the container inspector must not care about table
// contents, only that the directory is present and self-consistent.
std::vector<std::byte> MakeSingleFaceFont(std::uint32_t version, std::uint16_t table_count) {
    std::vector<std::byte> bytes(12U + (static_cast<std::size_t>(table_count) * 16U), std::byte{0});
    WriteU32(bytes, 0, version);
    WriteU16(bytes, 4, table_count);
    return bytes;
}

std::vector<std::byte> MakeCollection(std::uint32_t face_count) {
    std::vector<std::byte> bytes(12U + (static_cast<std::size_t>(face_count) * 4U), std::byte{0});
    WriteU32(bytes, 0, Tag('t', 't', 'c', 'f'));
    WriteU32(bytes, 4, 0x00010000U);
    WriteU32(bytes, 8, face_count);
    return bytes;
}

FontContainerInfo Inspect(const std::vector<std::byte>& bytes) {
    return VulkanEngine::FileLoaders::Fonts::InspectFontContainer(
        FileLoader::ByteSpan{bytes.data(), bytes.size()});
}

TEST(FontLoaderTest, AcceptsSingleFaceSfntContainers) {
    const auto truetype = Inspect(MakeSingleFaceFont(0x00010000U, 12));
    EXPECT_EQ(truetype.container, FontContainer::TrueType);
    EXPECT_EQ(truetype.face_count, 1u);
    EXPECT_EQ(truetype.table_count, 12u);
    EXPECT_TRUE(truetype.IsValid());

    const auto otto = Inspect(MakeSingleFaceFont(Tag('O', 'T', 'T', 'O'), 9));
    EXPECT_EQ(otto.container, FontContainer::OpenTypeCff);
    EXPECT_EQ(otto.face_count, 1u);
    EXPECT_EQ(otto.table_count, 9u);

    // Apple's legacy TrueType tag is still an accepted sfnt flavour.
    const auto apple = Inspect(MakeSingleFaceFont(Tag('t', 'r', 'u', 'e'), 5));
    EXPECT_EQ(apple.container, FontContainer::TrueType);
    EXPECT_EQ(apple.face_count, 1u);
}

TEST(FontLoaderTest, CountsFacesInACollection) {
    const auto info = Inspect(MakeCollection(3));
    EXPECT_EQ(info.container, FontContainer::TrueTypeCollection);
    EXPECT_EQ(info.face_count, 3u);
    EXPECT_EQ(info.table_count, 0u) << "a collection's tables are per-face";
}

TEST(FontLoaderTest, RejectsBuffersThatAreNotFonts) {
    // Nothing at all.
    EXPECT_EQ(Inspect({}).container, FontContainer::Unknown);

    // Shorter than an sfnt header.
    EXPECT_EQ(Inspect(std::vector<std::byte>(8, std::byte{0})).container, FontContainer::Unknown);

    // A recognisable but wrong magic.
    EXPECT_EQ(Inspect(MakeSingleFaceFont(Tag('X', 'X', 'X', 'X'), 4)).container,
              FontContainer::Unknown);

    // A plausible magic with no tables.
    EXPECT_EQ(Inspect(MakeSingleFaceFont(0x00010000U, 0)).container, FontContainer::Unknown);

    // An absurd table count cannot pass the directory bounds check.
    EXPECT_EQ(Inspect(MakeSingleFaceFont(0x00010000U, 0xFFFFU)).container,
              FontContainer::Unknown);
}

TEST(FontLoaderTest, RejectsTruncatedContainers) {
    // Directory cut short: the count says twelve tables, the bytes carry two.
    auto truncated = MakeSingleFaceFont(0x00010000U, 12);
    truncated.resize(12U + (2U * 16U));
    EXPECT_EQ(Inspect(truncated).container, FontContainer::Unknown);

    // Collection header with no room for the face offsets it advertises.
    auto short_collection = MakeCollection(4);
    short_collection.resize(12U + 4U);
    EXPECT_EQ(Inspect(short_collection).container, FontContainer::Unknown);

    // A collection with no faces is not a collection.
    EXPECT_EQ(Inspect(MakeCollection(0)).container, FontContainer::Unknown);
}

// The inspector must agree with a real font, not only with synthetic headers.
TEST(FontLoaderTest, AcceptsTheVendoredTestFont) {
    const std::filesystem::path path =
        std::filesystem::path{VKENGINE_TEST_FONT_DIR} / "Lato-Regular.ttf";

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(file) << "cannot open " << path.string();
    const std::streamsize size = file.tellg();
    ASSERT_GT(size, 0);
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    ASSERT_TRUE(file.read(reinterpret_cast<char*>(bytes.data()), size));

    const auto info = Inspect(bytes);
    EXPECT_EQ(info.container, FontContainer::TrueType);
    EXPECT_EQ(info.face_count, 1u);
    EXPECT_GT(info.table_count, 0u);
    EXPECT_LT(info.table_count, 64u);
}

TEST(FontLoaderTest, NamesEveryContainer) {
    for (const FontContainer container :
         {FontContainer::Unknown, FontContainer::TrueType, FontContainer::OpenTypeCff,
          FontContainer::TrueTypeCollection}) {
        EXPECT_FALSE(VulkanEngine::FileLoaders::Fonts::FontContainerName(container).empty());
    }
}

} // namespace
