module;

export module VulkanEngine.Text.Blob;

import std;
import std.compat;

import VulkanEngine.Text.Font;

export namespace VulkanEngine::Text {

// One glyph's encoded hb-gpu blob.
//
// The blob is the compact representation the hb-gpu (Slug) fragment shader
// decodes: a sequence of 8-byte units, each unit four signed 16-bit values, that
// spells out the glyph's band structure and quadratic curves. It is encoded in
// the font's *design units*, which is what makes it size independent: the same
// bytes serve every pixel size, every world scale, every rotation and every
// distance, because the shader reconstructs coverage analytically from the
// design-space geometry and the current em size. Nothing in the blob mentions a
// resolution.
//
// `bytes` is empty for a glyph with no ink (a space, or a glyph the encoder
// cannot draw). The extents are in design units with a y-up baseline, exactly
// the space HarfBuzz's `hb_font_get_glyph_extents` reports once the face's scale
// is pinned to its units-per-em: `x_bearing` is the ink box's left edge,
// `y_bearing` its top edge measured up from the baseline, `width` the positive
// horizontal extent and `height` the negative vertical extent (so `y_bearing +
// height` is the box's bottom).
struct GlyphBlob {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t glyph_id = 0;
    std::int32_t x_bearing = 0;
    std::int32_t y_bearing = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::byte> bytes;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool Empty() const noexcept { return bytes.empty(); }

    // The blob's length in the shader's addressable units. The blob format is a
    // sequence of 8-byte units (four 16-bit values each), and every offset the
    // shader computes -- and every range allocator offset -- is an index into
    // those units. This is the measurement that rules out the 16-byte `int4`
    // stride the upstream HLSL header comment suggests: encoded lengths are
    // always a multiple of 8 and are NOT always a multiple of 16.
    [[nodiscard]] std::uint64_t UnitCount() const noexcept {
        return static_cast<std::uint64_t>(bytes.size()) / 8U;
    }
};

// Packs blobs into one buffer at 8-byte-aligned, never-overlapping offsets.
//
// The blob format is a sequence of 8-byte units, so the allocator's unit is the
// byte and its alignment is 8: a blob placed at any other offset would make the
// shader's unit indexing read a misaligned word. The allocator is a pure integer
// structure with no device state, which is what lets the packing rules be proven
// device-free.
//
// Fitting is first-fit over the freed ranges, with adjacent ranges coalesced on
// release, so a freed range is reusable by the next allocation that fits it
// rather than being stranded behind a monotone high-water mark. When nothing
// free fits, the range is carved off the end and the capacity grows by exactly
// the request (the GPU buffer rounds that up to whatever it wants to allocate).
//
// Not thread-safe; the buffer that owns one serializes access to it.
class BlobRangeAllocator {
public:
    // The alignment every blob offset observes. Fixed rather than a parameter:
    // the blob format's unit is 8 bytes and there is no reason to hand out any
    // other offset.
    static constexpr std::uint64_t kAlignment = 8;

    // Reserves `size` bytes, rounded up to the alignment, and returns the byte
    // offset. Returns nullopt only for a zero-byte request; a space's blob is
    // zero bytes and must never be given a range.
    [[nodiscard]] std::optional<std::uint64_t> Allocate(std::uint64_t size);

    // Returns a range previously handed out. An unknown offset is ignored, so a
    // double release cannot corrupt the free list.
    void Free(std::uint64_t offset);

    // Drops every live range and every free block; the next allocation starts
    // again at offset 0.
    void Reset() noexcept;

    // Bytes handed out so far, including freed ranges below the high-water mark.
    // This is the size the owning GPU buffer must be able to hold.
    [[nodiscard]] std::uint64_t Capacity() const noexcept { return high_water_; }
    // Bytes currently reserved by live ranges (not freed).
    [[nodiscard]] std::uint64_t UsedBytes() const noexcept { return used_bytes_; }
    [[nodiscard]] std::size_t LiveCount() const noexcept { return live_.size(); }
    // Free ranges waiting to be reused.
    [[nodiscard]] std::size_t FreeBlockCount() const noexcept { return free_.size(); }

private:
    // offset -> size, both 8-byte aligned.
    std::map<std::uint64_t, std::uint64_t> free_{};
    std::map<std::uint64_t, std::uint64_t> live_{};
    std::uint64_t high_water_ = 0;
    std::uint64_t used_bytes_ = 0;
};

// Encodes glyph outlines into hb-gpu blobs, with a bounded LRU cache in front.
//
// One hb_gpu_draw_t encoder is created, reused for every glyph and destroyed
// with the store; that is why the type is neither copyable nor movable. The
// encoder is a mutable scratch object, so encodes are serialized by the store's
// own encoder mutex -- concurrent callers still get the cache's thread safety,
// they just cannot encode two glyphs literally at the same instant.
//
// The cache key is (FontFace::UniqueId(), FontFace::ResourceVersion(), glyph id)
// and deliberately *not* a size. The blob is in font design units, so an entry
// is valid at every pixel size; a size in the key would hold one identical blob
// per requested size and defeat the entire reason the Slug path exists. The key
// uses exact fields rather than a bare hash, the entry is a shared_ptr so a blob
// a caller holds survives eviction and Clear(), and eviction is a bounded LRU.
//
// Thread-safe.
class GlyphBlobEncoder {
public:
    // `max_cached_glyphs == 0` disables caching: every call encodes afresh and
    // the result is still handed out (and still owned by the caller).
    explicit GlyphBlobEncoder(std::size_t max_cached_glyphs = 1024);
    ~GlyphBlobEncoder();

    GlyphBlobEncoder(const GlyphBlobEncoder&) = delete;
    GlyphBlobEncoder& operator=(const GlyphBlobEncoder&) = delete;
    GlyphBlobEncoder(GlyphBlobEncoder&&) = delete;
    GlyphBlobEncoder& operator=(GlyphBlobEncoder&&) = delete;

    // The blob for (face, glyph id), encoding it on first use. Returns nullptr
    // only when the encoder cannot draw the glyph at all; a glyph with no ink
    // (a space) returns a non-null blob whose bytes are empty, so a caller can
    // still ask for its extents and skip it.
    //
    // `pixel_size` is accepted for call-site symmetry with GlyphRasterizer::Get
    // and MsdfGenerator::Get and is intentionally NOT part of the cache key: the
    // blob is size independent. Two requests for the same glyph at two different
    // sizes are one encode and one entry.
    [[nodiscard]] std::shared_ptr<const GlyphBlob> Get(const FontFace& face,
                                                       std::uint32_t glyph_id,
                                                       float pixel_size = 0.0f);

    [[nodiscard]] std::size_t Size() const;
    // Lifetime hit count. Clear() does not reset it, matching the other caches.
    [[nodiscard]] std::uint64_t HitCount() const;
    // How many times a glyph was actually encoded. The count that proves the
    // cache is keyed without a size.
    [[nodiscard]] std::uint64_t EncodeCount() const;
    // Drops every cached blob. Blobs already handed out stay valid.
    void Clear();

private:
    struct Key {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        std::uint64_t face_id = 0;
        std::uint32_t resource_version = 0;
        std::uint32_t glyph_id = 0;
        // NOLINTEND(misc-non-private-member-variables-in-classes)

        [[nodiscard]] bool operator==(const Key& other) const noexcept {
            return face_id == other.face_id && resource_version == other.resource_version &&
                   glyph_id == other.glyph_id;
        }
    };

    struct KeyHash {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };

    struct Entry {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        Key key;
        std::shared_ptr<const GlyphBlob> blob;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    // The encoder's opaque state: it owns the hb_gpu_draw_t and the mutex that
    // serializes encodes without exposing either header here.
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    mutable std::mutex mutex_;
    // Most recently used entry first; the back is the eviction candidate.
    std::list<Entry> entries_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
    std::size_t max_entries_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t encodes_ = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Text
