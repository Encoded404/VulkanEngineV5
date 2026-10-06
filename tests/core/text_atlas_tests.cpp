#include <gtest/gtest.h>

import std;
import std.compat;

import VulkanEngine.Text.Atlas;

namespace {

using VulkanEngine::Text::AtlasConfig;
using VulkanEngine::Text::AtlasInsert;
using VulkanEngine::Text::AtlasRect;
using VulkanEngine::Text::GlyphAtlas;
using VulkanEngine::Text::GlyphSlot;

// Two rectangles overlap only when they share interior area: touching edges is
// not an overlap, and a zero-area rectangle overlaps nothing.
[[nodiscard]] bool Overlaps(const AtlasRect& a, const AtlasRect& b) {
    return a.x < static_cast<std::uint64_t>(b.x) + b.width &&
           b.x < static_cast<std::uint64_t>(a.x) + a.width &&
           a.y < static_cast<std::uint64_t>(b.y) + b.height &&
           b.y < static_cast<std::uint64_t>(a.y) + a.height;
}

[[nodiscard]] bool Inside(const AtlasRect& rect, std::uint32_t page_width,
                          std::uint32_t page_height) {
    return static_cast<std::uint64_t>(rect.x) + rect.width <= page_width &&
           static_cast<std::uint64_t>(rect.y) + rect.height <= page_height;
}

// The inner region of a padded slot: what the caller's glyph pixels actually
// cover inside the rectangle the atlas handed back.
[[nodiscard]] AtlasRect InnerRect(const AtlasRect& slot, std::uint32_t padding) {
    return AtlasRect{slot.x + padding, slot.y + padding, slot.width - (padding * 2),
                     slot.height - (padding * 2)};
}

[[nodiscard]] bool Covers(const AtlasRect& outer, const AtlasRect& inner) {
    return outer.x <= inner.x && outer.y <= inner.y &&
           static_cast<std::uint64_t>(outer.x) + outer.width >=
               static_cast<std::uint64_t>(inner.x) + inner.width &&
           static_cast<std::uint64_t>(outer.y) + outer.height >=
               static_cast<std::uint64_t>(inner.y) + inner.height;
}

// The smallest distance between two rectangles along either axis; 0 when their
// extents touch or cross on both axes.
[[nodiscard]] std::uint64_t GapOnAxis(std::uint32_t a_start, std::uint32_t a_size,
                                      std::uint32_t b_start, std::uint32_t b_size) {
    const std::uint64_t a_end = static_cast<std::uint64_t>(a_start) + a_size;
    const std::uint64_t b_end = static_cast<std::uint64_t>(b_start) + b_size;
    if (b_start >= a_end) {
        return b_start - a_end;
    }
    if (a_start >= b_end) {
        return a_start - b_end;
    }
    return 0;
}

// Lato's shape is irrelevant here: the atlas is pure bookkeeping, so every test
// that is not about the page geometry uses the 16x16 padding-free configuration
// and exact quadrant fills.
[[nodiscard]] AtlasConfig QuadConfig() {
    return AtlasConfig{.page_width = 16,
                       .page_height = 16,
                       .padding = 0,
                       .max_pages = 1};
}

TEST(TextAtlasTest, PacksThousandsOfRectsWithoutOverlaps) {
    constexpr std::size_t kCount = 3000;
    constexpr std::uint32_t kPageWidth = 512;
    constexpr std::uint32_t kPageHeight = 512;
    constexpr std::size_t kMaxPages = 16;

    GlyphAtlas atlas{AtlasConfig{.page_width = kPageWidth,
                                 .page_height = kPageHeight,
                                 .padding = 1,
                                 .max_pages = kMaxPages}};

    // Deterministic, varied sizes: the invariant has to hold for the awkward
    // mixes, not just for one uniform size.
    std::vector<std::uint32_t> widths;
    std::vector<std::uint32_t> heights;
    widths.reserve(kCount);
    heights.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        widths.push_back(3U + static_cast<std::uint32_t>(i % 19));
        heights.push_back(3U + static_cast<std::uint32_t>((i * 7) % 23));
    }

    std::size_t inserted = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        const AtlasInsert result =
            atlas.Insert(static_cast<std::uint64_t>(i) + 1, widths[i], heights[i]);
        EXPECT_NE(result, AtlasInsert::TooLarge);
        if (result == AtlasInsert::Inserted) {
            ++inserted;
        }
    }

    // The configuration is sized so every glyph is resident: an eviction would
    // invalidate the per-key checks below.
    EXPECT_EQ(inserted, kCount);
    EXPECT_EQ(atlas.EvictionCount(), 0u);
    EXPECT_EQ(atlas.GlyphCount(), kCount);
    EXPECT_LE(atlas.PageCount(), kMaxPages);

    const auto entries = atlas.Entries();
    ASSERT_EQ(entries.size(), kCount);

    for (const auto& [key, slot] : entries) {
        EXPECT_TRUE(Inside(slot.rect, kPageWidth, kPageHeight))
            << "key " << key << " escaped its page";
        EXPECT_GE(slot.rect.width, widths[key - 1] + 2);
        EXPECT_GE(slot.rect.height, heights[key - 1] + 2);

        // Every resident glyph stays resolvable, and resolves to the slot the
        // enumeration reported.
        const auto found = atlas.Find(key);
        ASSERT_TRUE(found.has_value()) << "key " << key << " vanished";
        EXPECT_EQ(found->page, slot.page);
        EXPECT_EQ(found->rect.x, slot.rect.x);
        EXPECT_EQ(found->rect.y, slot.rect.y);
    }

    // The core invariant, hard: no two rectangles on one page may overlap.
    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[i].second.page != entries[j].second.page) {
                continue;
            }
            EXPECT_FALSE(Overlaps(entries[i].second.rect, entries[j].second.rect))
                << "keys " << entries[i].first << " and " << entries[j].first
                << " overlap on page " << entries[i].second.page;
        }
    }
}

TEST(TextAtlasTest, AddsThePaddingAsAGutterOnEverySide) {
    constexpr std::uint32_t kPadding = 2;
    GlyphAtlas atlas{AtlasConfig{.page_width = 128,
                                 .page_height = 128,
                                 .padding = kPadding,
                                 .max_pages = 2}};

    ASSERT_EQ(atlas.Insert(1, 10, 6), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(2, 7, 9), AtlasInsert::Inserted);

    const auto first = atlas.Find(1);
    const auto second = atlas.Find(2);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    // A requested w x h consumes (w + padding) x (h + padding) or more, and the
    // gutter is symmetric: the inner region is exactly the request.
    EXPECT_EQ(first->rect.width, 10U + (2U * kPadding));
    EXPECT_EQ(first->rect.height, 6U + (2U * kPadding));
    EXPECT_EQ(second->rect.width, 7U + (2U * kPadding));
    EXPECT_EQ(second->rect.height, 9U + (2U * kPadding));
    EXPECT_EQ(InnerRect(first->rect, kPadding).width, 10U);
    EXPECT_EQ(InnerRect(first->rect, kPadding).height, 6U);

    // Neighbouring glyphs are separated by at least the padding, so a bilinear
    // tap at one glyph's edge cannot reach the next glyph's ink.
    const AtlasRect a = InnerRect(first->rect, kPadding);
    const AtlasRect b = InnerRect(second->rect, kPadding);
    EXPECT_FALSE(Overlaps(a, b));
    const std::uint64_t gap_x = GapOnAxis(a.x, a.width, b.x, b.width);
    const std::uint64_t gap_y = GapOnAxis(a.y, a.height, b.y, b.height);
    EXPECT_TRUE(gap_x >= kPadding || gap_y >= kPadding)
        << "gutter between neighbouring glyphs is smaller than the padding";
}

TEST(TextAtlasTest, SpillsToANewPageWhenTheCurrentOneIsFull) {
    GlyphAtlas atlas{AtlasConfig{.page_width = 32,
                                 .page_height = 32,
                                 .padding = 0,
                                 .max_pages = 4}};

    ASSERT_EQ(atlas.Insert(0, 32, 32), AtlasInsert::Inserted);
    EXPECT_EQ(atlas.PageCount(), 1U);

    // Page 0 is exactly full, so the request has to spill.
    ASSERT_EQ(atlas.Insert(1, 16, 16), AtlasInsert::Inserted);
    EXPECT_EQ(atlas.PageCount(), 2U);
    EXPECT_EQ(atlas.EvictionCount(), 0u);

    const auto first = atlas.Find(0);
    const auto second = atlas.Find(1);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->page, second->page);

    // Page 1 holds a quadrant and two strips; another full page does not fit
    // there, so a third page appears.
    ASSERT_EQ(atlas.Insert(2, 32, 32), AtlasInsert::Inserted);
    EXPECT_EQ(atlas.PageCount(), 3U);
}

TEST(TextAtlasTest, NeverGrowsPastMaxPages) {
    GlyphAtlas atlas{AtlasConfig{.page_width = 16,
                                 .page_height = 16,
                                 .padding = 0,
                                 .max_pages = 2}};

    ASSERT_EQ(atlas.Insert(0, 16, 16), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(1, 16, 16), AtlasInsert::Inserted);
    EXPECT_EQ(atlas.PageCount(), 2U);

    // Both pages are full and the ceiling is reached, so the third insert
    // evicts instead of adding a page.
    EXPECT_EQ(atlas.Insert(2, 16, 16), AtlasInsert::Evicted);
    EXPECT_EQ(atlas.PageCount(), 2U);
    EXPECT_EQ(atlas.EvictionCount(), 1u);
}

TEST(TextAtlasTest, EvictsTheLeastRecentlyTouchedGlyph) {
    GlyphAtlas atlas{QuadConfig()};
    ASSERT_EQ(atlas.Insert(1, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(2, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(3, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(4, 8, 8), AtlasInsert::Inserted);
    EXPECT_EQ(atlas.GlyphCount(), 4U);

    // Recency order is 4, 3, 2, 1 with 1 the oldest.
    EXPECT_EQ(atlas.Insert(5, 8, 8), AtlasInsert::Evicted);
    EXPECT_FALSE(atlas.Find(1).has_value()) << "the oldest glyph must be the victim";
    EXPECT_TRUE(atlas.Find(2).has_value());
    EXPECT_TRUE(atlas.Find(3).has_value());
    EXPECT_TRUE(atlas.Find(4).has_value());
    EXPECT_TRUE(atlas.Find(5).has_value());
    EXPECT_EQ(atlas.GlyphCount(), 4U);
    EXPECT_EQ(atlas.EvictionCount(), 1u);
}

TEST(TextAtlasTest, TouchChangesWhichGlyphIsEvicted) {
    GlyphAtlas atlas{QuadConfig()};
    ASSERT_EQ(atlas.Insert(1, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(2, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(3, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(4, 8, 8), AtlasInsert::Inserted);

    // Use 1 again; now 2 is the oldest.
    atlas.Touch(1);
    EXPECT_EQ(atlas.Insert(5, 8, 8), AtlasInsert::Evicted);
    EXPECT_TRUE(atlas.Find(1).has_value()) << "touched glyph must be spared";
    EXPECT_FALSE(atlas.Find(2).has_value());
}

// Find() is a lookup, not a use. If it updated recency, eviction would depend on
// query order and no test could name the victim.
TEST(TextAtlasTest, FindDoesNotChangeRecency) {
    GlyphAtlas atlas{QuadConfig()};
    ASSERT_EQ(atlas.Insert(1, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(2, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(3, 8, 8), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(4, 8, 8), AtlasInsert::Inserted);

    EXPECT_TRUE(atlas.Find(1).has_value());
    EXPECT_EQ(atlas.Insert(5, 8, 8), AtlasInsert::Evicted);
    EXPECT_FALSE(atlas.Find(1).has_value()) << "Find must not have rescued key 1";
}

TEST(TextAtlasTest, ReportsTooLargeAndChangesNothing) {
    GlyphAtlas atlas{AtlasConfig{.page_width = 32,
                                 .page_height = 32,
                                 .padding = 1,
                                 .max_pages = 2}};
    ASSERT_EQ(atlas.Insert(1, 10, 10), AtlasInsert::Inserted);
    const std::size_t pages_before = atlas.PageCount();
    const std::size_t glyphs_before = atlas.GlyphCount();
    const auto slot_before = atlas.Find(1);
    ASSERT_TRUE(slot_before.has_value());

    // Padding counts towards the limit: 31 + 2 > 32 even though 31 < 32.
    EXPECT_EQ(atlas.Insert(2, 31, 31), AtlasInsert::TooLarge);
    EXPECT_EQ(atlas.Insert(3, 33, 5), AtlasInsert::TooLarge);
    EXPECT_EQ(atlas.Insert(4, 5, 33), AtlasInsert::TooLarge);

    EXPECT_EQ(atlas.PageCount(), pages_before);
    EXPECT_EQ(atlas.GlyphCount(), glyphs_before);
    EXPECT_EQ(atlas.EvictionCount(), 0u);
    EXPECT_FALSE(atlas.Find(2).has_value());
    EXPECT_FALSE(atlas.Find(3).has_value());
    EXPECT_FALSE(atlas.Find(4).has_value());

    // The failed requests left the resident glyph exactly where it was.
    const auto slot_after = atlas.Find(1);
    ASSERT_TRUE(slot_after.has_value());
    EXPECT_EQ(slot_after->page, slot_before->page);
    EXPECT_EQ(slot_after->rect.x, slot_before->rect.x);
    EXPECT_EQ(slot_after->rect.y, slot_before->rect.y);

    // A request that does fit the page, padding included, is accepted.
    EXPECT_EQ(atlas.Insert(5, 30, 30), AtlasInsert::Inserted);
}

TEST(TextAtlasTest, ReInsertingAKeyKeepsItsSlot) {
    GlyphAtlas atlas{AtlasConfig{.page_width = 128,
                                 .page_height = 128,
                                 .padding = 1,
                                 .max_pages = 2}};
    ASSERT_EQ(atlas.Insert(7, 10, 10), AtlasInsert::Inserted);
    const auto first = atlas.Find(7);
    ASSERT_TRUE(first.has_value());

    // Same size: same rectangle, no second entry, and the region is invalidated
    // because the caller is going to rewrite those pixels.
    EXPECT_EQ(atlas.Insert(7, 10, 10), AtlasInsert::Replaced);
    const auto again = atlas.Find(7);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->page, first->page);
    EXPECT_EQ(again->rect.x, first->rect.x);
    EXPECT_EQ(again->rect.y, first->rect.y);
    EXPECT_EQ(again->rect.width, first->rect.width);
    EXPECT_EQ(again->rect.height, first->rect.height);
    EXPECT_EQ(atlas.GlyphCount(), 1U);
    EXPECT_EQ(atlas.EvictionCount(), 0u);
    EXPECT_TRUE(atlas.DirtyRect(first->page).has_value());

    // A smaller request also reuses the bigger slot rather than moving.
    EXPECT_EQ(atlas.Insert(7, 4, 4), AtlasInsert::Replaced);
    const auto smaller = atlas.Find(7);
    ASSERT_TRUE(smaller.has_value());
    EXPECT_EQ(smaller->rect.x, first->rect.x);
    EXPECT_EQ(smaller->rect.width, first->rect.width);

    // A request the slot cannot hold is replaced in place of a new allocation.
    EXPECT_EQ(atlas.Insert(7, 40, 40), AtlasInsert::Replaced);
    const auto bigger = atlas.Find(7);
    ASSERT_TRUE(bigger.has_value());
    EXPECT_EQ(bigger->rect.width, 42U);
    EXPECT_EQ(bigger->rect.height, 42U);
    EXPECT_EQ(atlas.GlyphCount(), 1U);
}

TEST(TextAtlasTest, EraseFreesTheSlotForReuse) {
    GlyphAtlas atlas{AtlasConfig{.page_width = 32,
                                 .page_height = 32,
                                 .padding = 0,
                                 .max_pages = 1}};
    ASSERT_EQ(atlas.Insert(1, 16, 16), AtlasInsert::Inserted);
    ASSERT_TRUE(atlas.Erase(1));
    EXPECT_FALSE(atlas.Erase(1));
    EXPECT_FALSE(atlas.Find(1).has_value());
    EXPECT_EQ(atlas.GlyphCount(), 0U);
    EXPECT_EQ(atlas.PageCount(), 1U);
    EXPECT_EQ(atlas.EvictionCount(), 0u);

    // Coalescing puts the freed quadrant back together with its strips, so the
    // page can hold a glyph the size of the whole page again.
    EXPECT_EQ(atlas.Insert(2, 32, 32), AtlasInsert::Inserted);
    EXPECT_TRUE(atlas.Find(2).has_value());
}

TEST(TextAtlasTest, TracksDirtyRegionsPerPage) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 64;
    GlyphAtlas atlas{AtlasConfig{.page_width = kWidth,
                                 .page_height = kHeight,
                                 .padding = 1,
                                 .max_pages = 4}};

    EXPECT_EQ(atlas.DirtyRect(0), std::nullopt) << "no page exists yet";

    ASSERT_EQ(atlas.Insert(1, 8, 8), AtlasInsert::Inserted);
    const auto first = atlas.Find(1);
    ASSERT_TRUE(first.has_value());
    const auto initial = atlas.DirtyRect(first->page);
    ASSERT_TRUE(initial.has_value());
    EXPECT_TRUE(Covers(*initial, first->rect));
    // A page nothing has ever been uploaded for is entirely dirty.
    EXPECT_EQ(initial->width, kWidth);
    EXPECT_EQ(initial->height, kHeight);
    EXPECT_EQ(atlas.DirtyRect(first->page + 1), std::nullopt) << "no such page";

    atlas.ClearDirty(first->page);
    EXPECT_EQ(atlas.DirtyRect(first->page), std::nullopt);

    ASSERT_EQ(atlas.Insert(2, 12, 6), AtlasInsert::Inserted);
    const auto second = atlas.Find(2);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->page, first->page);
    const auto after_second = atlas.DirtyRect(second->page);
    ASSERT_TRUE(after_second.has_value());
    EXPECT_TRUE(Covers(*after_second, second->rect));

    ASSERT_EQ(atlas.Insert(3, 5, 5), AtlasInsert::Inserted);
    const auto third = atlas.Find(3);
    ASSERT_TRUE(third.has_value());
    const auto after_third = atlas.DirtyRect(third->page);
    ASSERT_TRUE(after_third.has_value());
    // The union covers every write since the clear, not just the latest one.
    EXPECT_TRUE(Covers(*after_third, second->rect));
    EXPECT_TRUE(Covers(*after_third, third->rect));

    atlas.ClearAllDirty();
    EXPECT_EQ(atlas.DirtyRect(first->page), std::nullopt);
    EXPECT_EQ(atlas.DirtyRect(second->page), std::nullopt);
}

TEST(TextAtlasTest, ResetDropsPagesGlyphsAndDirtyState) {
    GlyphAtlas atlas{AtlasConfig{.page_width = 32,
                                 .page_height = 32,
                                 .padding = 0,
                                 .max_pages = 1}};
    ASSERT_EQ(atlas.Insert(1, 16, 16), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(2, 16, 16), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(3, 16, 16), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(4, 16, 16), AtlasInsert::Inserted);
    ASSERT_EQ(atlas.Insert(5, 16, 16), AtlasInsert::Evicted);
    ASSERT_EQ(atlas.EvictionCount(), 1u);
    ASSERT_EQ(atlas.PageCount(), 1U);
    ASSERT_EQ(atlas.GlyphCount(), 4U);

    atlas.Reset();
    EXPECT_EQ(atlas.PageCount(), 0U);
    EXPECT_EQ(atlas.GlyphCount(), 0U);
    EXPECT_EQ(atlas.DirtyRect(0), std::nullopt);
    EXPECT_FALSE(atlas.Find(4).has_value());
    EXPECT_FALSE(atlas.Find(5).has_value());
    // A lifetime statistic, like the shaping cache's hit count: Reset() does not
    // erase how the atlas has performed.
    EXPECT_EQ(atlas.EvictionCount(), 1u);

    // A reset atlas is a fresh one.
    EXPECT_EQ(atlas.Insert(4, 16, 16), AtlasInsert::Inserted);
    EXPECT_EQ(atlas.PageCount(), 1U);
    EXPECT_TRUE(atlas.DirtyRect(0).has_value());
}

} // namespace
