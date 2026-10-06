module;

// The atlas is deliberately device-free: it is rectangle bookkeeping and nothing
// else, so it can be tested without a Vulkan device and reused by any backend.
// Deciding where a glyph's pixels go and which region changed is the whole job;
// uploading them is a later commit's problem.

export module VulkanEngine.Text.Atlas;

import std;
import std.compat;

export namespace VulkanEngine::Text {

// One rectangle inside one atlas page, in texels.
//
// A rectangle handed out by GlyphAtlas::Insert is a *padded slot*, not the ink
// box: the glyph's own pixels belong in the inner region inset by
// AtlasConfig::padding on every side. The gutter exists because a page is
// sampled with bilinear filtering, and a sample at the edge of one glyph reads
// the four texels around it -- if a neighbour's ink sat directly against it, the
// edge would blend in the neighbour's coverage. Padding every side by at least
// one texel means the 2x2 kernel around any edge texel only ever reaches the
// glyph's own outline and its own gutter.
struct AtlasRect {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Where a glyph's bitmap lives: which page, and which slot on that page.
struct GlyphSlot {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t page = 0;
    AtlasRect rect{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// What one texel of a page means to whoever samples it.
//
// The pure atlas stores rectangles and never pixels, so this is not a property
// the allocator needs. The GPU uploader does need it: the format picks the page
// image format and how many bytes a texel costs, which is the difference between
// copying a glyph's A8 coverage and copying its four MSDF channels. A8 is the
// hinted-coverage page the FreeType rasterizer feeds; Rgba8 is the MSDF page
// (rgb = the three distance channels, a = the true single-channel distance).
//
// Both are sampled as *linear*: a distance field read through an sRGB view is a
// different field, and the A8 coverage page has always been linear.
enum class AtlasPageFormat : std::uint8_t {
    A8,
    Rgba8,
};

// Bytes one texel occupies in a page of `format`. A full-page composition and a
// precise region copy both scale every texel count by this.
[[nodiscard]] constexpr std::uint32_t BytesPerTexel(AtlasPageFormat format) noexcept {
    return format == AtlasPageFormat::Rgba8 ? 4U : 1U;
}

// One atlas entry's pixels in its page's format: tightly packed, row-major,
// top-down, `width * height * BytesPerTexel(format)` bytes.
//
// This is the currency the GPU uploader's byte source hands back. It is the
// reason the uploader does not need to know whether a rectangle holds FreeType
// coverage or an MSDF: either way it is just bytes in a page, and the page
// format says how to read them. Keeping it here -- next to the atlas, which
// decides *where* bytes go -- is deliberate; the atlas itself still never owns
// pixels.
struct AtlasBitmap {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool Empty() const noexcept {
        return width == 0 || height == 0 || bytes.empty();
    }
};

// What an insertion actually did, so a caller can react. The atlas never
// silently drops an entry a caller believes is resident: a key that is reported
// Inserted, Replaced or Evicted is findable afterwards; only TooLarge leaves the
// caller without a slot, and it changes nothing.
enum class AtlasInsert {
    // A new key was given a slot.
    Inserted,
    // The key was already resident and keeps a slot. The old rectangle is reused
    // when it still fits the request, so the caller must rewrite its pixels.
    Replaced,
    // Every page was full, so the least-recently-used glyph was dropped and its
    // space (coalesced with whatever free space touched it) was reused. The page
    // is invalidated as a whole: a caller that does not track dirty rectangles
    // can safely re-upload all of it.
    Evicted,
    // The padded request does not fit an empty page. Nothing changed: no entry,
    // no page and no dirty rectangle.
    TooLarge,
};

struct AtlasConfig {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t page_width = 1024;
    std::uint32_t page_height = 1024;
    // Texels of clear space added on *every* side of a requested rectangle, so a
    // requested w x h consumes (w + 2 * padding) x (h + 2 * padding). 0 disables
    // the gutter and is only appropriate for nearest-neighbour sampling.
    std::uint32_t padding = 1;
    // Hard ceiling on pages. When they are all full and no free rectangle fits,
    // the atlas evicts instead of growing past this.
    std::size_t max_pages = 8;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// A growable, page-based rectangle allocator for glyph bitmaps.
//
// The atlas is deterministic by construction. Insertion reuses an existing page
// while a free rectangle fits it, spills to a new page when none does, and only
// when max_pages is reached does it evict. Eviction is least-recently-used over
// the recency list Touch() maintains; Find() deliberately does not touch, so a
// lookup cannot change which glyph is evicted next and the behaviour stays
// testable to the exact key. Ties in the packing search are broken by page, then
// y, then x, so the same sequence of calls always produces the same layout.
//
// Not thread-safe on its own: a caller that shares one atlas across worker
// threads must serialize access. That is deliberate -- the rasterizer that owns
// one has a lock anyway, and pushing that lock in here would only hide it.
class GlyphAtlas {
public:
    explicit GlyphAtlas(AtlasConfig config = {});

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    GlyphAtlas(GlyphAtlas&&) = delete;
    GlyphAtlas& operator=(GlyphAtlas&&) = delete;

    // Reserves a slot for `key`, requesting a `width x height` ink region (which
    // the padding is then added around). Re-inserting a resident key reports
    // Replaced and keeps its rectangle when that rectangle still fits; when it
    // no longer fits, the old slot is released and a fresh one is allocated.
    //
    // `key` is opaque to the atlas; the caller decides what identifies a bitmap
    // (the rasterizer uses the face identity, glyph id, quantized size and
    // hinting mode). Its only requirement is that a different bitmap never
    // shares a key.
    [[nodiscard]] AtlasInsert Insert(std::uint64_t key, std::uint32_t width, std::uint32_t height);

    // The slot currently holding `key`, or nullopt. Does not change recency.
    [[nodiscard]] std::optional<GlyphSlot> Find(std::uint64_t key) const;

    // Marks `key` as the most recently used entry. A no-op for an unknown key.
    void Touch(std::uint64_t key);

    // Drops `key`, returning its space to its page and invalidating that space.
    // Returns false when the key was not resident.
    [[nodiscard]] bool Erase(std::uint64_t key);

    [[nodiscard]] std::size_t PageCount() const noexcept { return pages_.size(); }
    [[nodiscard]] std::size_t GlyphCount() const noexcept { return entries_.size(); }

    // The configuration the atlas was built with. An uploader needs page_width/
    // page_height to size a page image and padding to compute the inner region a
    // glyph's pixels belong in, and neither is derivable from the slots alone.
    [[nodiscard]] const AtlasConfig& Config() const noexcept { return config_; }

    // Lifetime count of evicted glyphs. Reset() does not clear it, the same way
    // ShapingCache::Clear() does not clear its hit count.
    [[nodiscard]] std::uint64_t EvictionCount() const noexcept { return evictions_; }

    // The bounding box of every rectangle written to `page` since the page's
    // dirty state was last cleared, or nullopt when the page is clean or does
    // not exist. A freshly created page is entirely dirty, because the uploader
    // has never seen any of it. A later uploader clears the region it copied so
    // the next frame only moves what actually changed.
    [[nodiscard]] std::optional<AtlasRect> DirtyRect(std::uint32_t page) const;

    void ClearDirty(std::uint32_t page);
    void ClearAllDirty();

    // Every resident (key, slot), ordered by page, then y, then x. A caller that
    // re-uploads a whole page needs the live rectangles on it, and the tests use
    // it to check the allocator's core invariant directly.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, GlyphSlot>> Entries() const;

    // Drops every page and entry, including dirty state. EvictionCount() is a
    // lifetime statistic and is not reset.
    void Reset();

private:
    // One page's free space is a list of disjoint rectangles. Allocation takes a
    // best fit (smallest leftover area) and guillotine-splits it; freeing
    // coalesces neighbours back together so a page does not fragment forever.
    struct Page {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        std::vector<AtlasRect> free_rects;
        std::optional<AtlasRect> dirty;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    struct Entry {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        std::uint32_t page = 0;
        AtlasRect rect{};
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    [[nodiscard]] std::optional<std::size_t> BestFreeIndex(std::uint32_t page, std::uint64_t width,
                                                           std::uint64_t height,
                                                           std::uint64_t& leftover) const;
    // Takes the global best fit across existing pages, or nullopt when none has
    // a free rectangle for the request. Only called when the request fits a page.
    [[nodiscard]] std::optional<GlyphSlot> TakeBestFit(std::uint64_t width, std::uint64_t height);
    void AllocateNewPage();
    [[nodiscard]] bool EvictOne(std::uint64_t width, std::uint64_t height);
    void AddEntry(std::uint64_t key, std::uint32_t page, const AtlasRect& rect);
    void FreeRect(std::uint32_t page, const AtlasRect& rect);
    void UnionDirty(std::uint32_t page, const AtlasRect& rect);
    void MarkPageDirty(std::uint32_t page);
    void TouchInternal(std::uint64_t key);
    bool EraseInternal(std::uint64_t key);

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    AtlasConfig config_;
    std::vector<Page> pages_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    // Most recently used key first; the back is the global eviction candidate.
    std::list<std::uint64_t> lru_;
    std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> lru_index_;
    std::uint64_t evictions_ = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Text
