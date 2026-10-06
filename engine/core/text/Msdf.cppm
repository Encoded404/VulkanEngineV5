module;

// The msdfgen and HarfBuzz headers stay out of this interface on purpose: the
// adapter below is opaque and the only engine-facing types are ours, so a
// consumer of the MSDF store does not have to compile either third-party header.

export module VulkanEngine.Text.Msdf;

import std;
import std.compat;

import VulkanEngine.Text.Atlas;
import VulkanEngine.Text.Font;

export namespace VulkanEngine::Text {

// Snaps a requested field size to the nearest 1/8 px, the same rule and for the
// same reason as GlyphRaster::QuantizePixelSize: a DPI-scaled UI asks for
// continuously varying sizes, and a cache keyed on the raw float would hold a
// separate field for every value that happens to be requested. Zero (and
// anything non-positive) stays zero, which callers use as "do not generate".
[[nodiscard]] float QuantizeFieldSize(float field_pixel_size) noexcept;

// The shape of one generated field.
//
// `field_pixel_size` is the em size the field is generated at, in texels: it
// sets both the resolution of the distance field and the atlas footprint of a
// glyph. `range` is the signed distance the field represents on either side of
// the outline, in field texels; the world-space shader reconstructs coverage
// from it, so the uniform it uses must be the value a field was generated with.
struct MsdfConfig {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    float field_pixel_size = 32.0f;
    double range = 4.0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

inline constexpr MsdfConfig kDefaultMsdfConfig{};

// Which msdfgen edge kind a HarfBuzz draw callback produced. Exported so the
// outline adapter's callback mapping can be asserted without exposing msdfgen.
enum class OutlineEdgeKind : std::uint8_t {
    Line,
    Quadratic,
    Cubic,
};

// One contour of a drawn glyph, as the callbacks built it.
struct OutlineContour {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<OutlineEdgeKind> edges;
    // True when HarfBuzz emitted close_path for this contour.
    bool closed = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct GlyphOutline {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<OutlineContour> contours;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] std::size_t EdgeCount() const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return contours.empty(); }
};

// Adapts HarfBuzz's glyph draw output into msdfgen outline edges, reusing one
// hb_draw_funcs_t for every glyph.
//
// HarfBuzz's draw callbacks are the outline source because the face's scale is
// pinned to its units-per-em: every point arrives in design units, y-up, which
// is exactly msdfgen's shape space, so no transform is needed and
// shape.inverseYAxis stays false. The callbacks are the same outline the shaping
// and FreeType paths see, so the generated field cannot drift from the metrics
// GlyphAtSize reports.
//
// The adapter owns only the callback table, which is made immutable once and is
// then safe to use from any number of threads at the same time; each Draw()
// supplies its own draw_data. Draw() and Describe() are const.
class HarfBuzzOutlineAdapter {
public:
    HarfBuzzOutlineAdapter();
    ~HarfBuzzOutlineAdapter();

    HarfBuzzOutlineAdapter(const HarfBuzzOutlineAdapter&) = delete;
    HarfBuzzOutlineAdapter& operator=(const HarfBuzzOutlineAdapter&) = delete;
    HarfBuzzOutlineAdapter(HarfBuzzOutlineAdapter&&) = delete;
    HarfBuzzOutlineAdapter& operator=(HarfBuzzOutlineAdapter&&) = delete;

    // The contour/edge description HarfBuzz draws for `glyph_id`. nullopt when
    // the face has no outline for it (an inkless or invalid glyph).
    [[nodiscard]] std::optional<GlyphOutline> Describe(const FontFace& face,
                                                       std::uint32_t glyph_id) const;

    // Opaque implementation state. Public only so the implementation unit's
    // field-generation helper can name it; it is incomplete here and defined in
    // that unit, which is what keeps <hb.h> and <msdfgen.h> private.
    struct Impl;

private:
    // The generator drives the same adapter internals Draw() uses; the opaque
    // state stays private to the implementation unit either way.
    friend class MsdfGenerator;

    std::unique_ptr<Impl> impl_;
};

// One glyph's generated multi-channel field.
//
// `pixels` is RGBA8 in the page format's channel order: rgb = the three
// multi-channel distance channels, a = the true single-channel signed distance.
// Four channels are stored rather than three because the true distance in alpha
// is what keeps the reconstruction sharp when the glyph is drawn smaller than
// its field (small or distant world text); WorldTextPass's fragment shader
// consumes it, so no channel is shipped unused.
//
// `left`/`top` place the field's top-left corner relative to the pen origin in
// the same y-down bitmap sense RasterGlyph uses (the pen is (0, 0), +y grows
// down, so `top` is negative for ink above the baseline). The field includes the
// distance range as padding on every side, so the ink box sits inset by
// ceil(range) texels.
struct MsdfGlyph {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t glyph_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float left = 0.0f;
    float top = 0.0f;
    // The glyph's horizontal advance at `field` scale (the pixel size the field
    // was generated at), not at any requested draw size.
    float advance_x = 0.0f;
    // The distance range the field was generated with, in field texels.
    float range = 0.0f;
    std::vector<std::uint8_t> pixels;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool Empty() const noexcept { return width == 0 || height == 0; }
};

// Generates multi-channel distance fields from HarfBuzz outlines and packs them
// into a GlyphAtlas, with a bounded LRU cache in front.
//
// Generation mirrors GlyphRasterizer exactly, one layer over: the key is the
// face identity and resource version, the glyph id, the *quantized* field size
// and the exact distance range, so two requests compare equal only when they
// would produce the same bytes; entries are shared pointers, so a field a caller
// holds stays valid after eviction or Clear(); and eviction releases the atlas
// slot so the atlas tracks the live set. The cache hands back fields for inkless
// glyphs too (a zero-sized MsdfGlyph), but reserves no slot for them.
//
// The atlas is Rgba8: the generated field's four channels are the page's texels.
// BitmapForAtlasKey() is what GlyphAtlasGpu's byte source calls, which is how
// the same uploader serves both this page format and the FreeType A8 page.
//
// Thread-safe. Generation runs outside the lock (it is the expensive part); the
// atlas is only touched under the lock.
class MsdfGenerator {
public:
    // The page format every field is generated for.
    static constexpr AtlasPageFormat kPageFormat = AtlasPageFormat::Rgba8;
    // Channels per texel in that format: rgb = MSDF, a = true SDF.
    static constexpr std::uint32_t kChannels = 4;

    // `max_cached_glyphs == 0` disables caching: every call generates afresh and
    // no atlas slot is reserved.
    explicit MsdfGenerator(AtlasConfig atlas_config = {}, std::size_t max_cached_glyphs = 1024);

    MsdfGenerator(const MsdfGenerator&) = delete;
    MsdfGenerator& operator=(const MsdfGenerator&) = delete;
    MsdfGenerator(MsdfGenerator&&) = delete;
    MsdfGenerator& operator=(MsdfGenerator&&) = delete;

    // Generates (or looks up) the field for (face, glyph, config) and returns
    // where its pixels belong. nullopt for an inkless glyph, a zero field size,
    // a glyph HarfBuzz cannot draw, or a field the atlas cannot hold. Get()
    // still returns the generated field in those cases.
    [[nodiscard]] std::optional<GlyphSlot> Generate(const FontFace& face, std::uint32_t glyph_id,
                                                    const MsdfConfig& config = {});

    // The field for (face, glyph, config), or nullptr when it cannot be
    // generated. The result survives eviction and Clear().
    [[nodiscard]] std::shared_ptr<const MsdfGlyph> Get(const FontFace& face, std::uint32_t glyph_id,
                                                       const MsdfConfig& config = {});

    // The atlas the generated fields live in.
    [[nodiscard]] const GlyphAtlas& Atlas() const noexcept { return atlas_; }

    // Mutable access for the uploader, which clears a page's dirty region once
    // its bytes have been copied. The caller must not generate concurrently.
    [[nodiscard]] GlyphAtlas& MutableAtlas() noexcept { return atlas_; }

    // The pixels packed into the rectangle an atlas key is keyed by, in the page
    // format, or nullptr when no live entry uses that key. This is the byte
    // source GlyphAtlasGpu resolves a whole-page rewrite through.
    [[nodiscard]] std::shared_ptr<const AtlasBitmap> BitmapForAtlasKey(
        std::uint64_t atlas_key) const;

    [[nodiscard]] std::size_t Size() const;
    // Lifetime hit count. Clear() does not reset it, matching ShapingCache.
    [[nodiscard]] std::uint64_t HitCount() const;
    // Drops every cached field and releases the atlas slots they hold.
    void Clear();
    // Drops every cached field *and* every atlas page, so the CPU atlas is
    // rebuilt from zero (a font reload retires the GPU pages wholesale).
    void Reset();

private:
    struct Key {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        std::uint64_t face_id = 0;
        std::uint32_t resource_version = 0;
        std::uint32_t glyph_id = 0;
        // Already quantized, so two requests for the same eighth-pixel compare
        // exactly equal rather than approximately.
        float field_pixel_size = 0.0f;
        // The range is compared as an exact bit pattern: it changes the bytes,
        // so an approximate match would be a different field.
        std::uint64_t range_bits = 0;
        // NOLINTEND(misc-non-private-member-variables-in-classes)

        [[nodiscard]] bool operator==(const Key& other) const noexcept {
            return face_id == other.face_id && resource_version == other.resource_version &&
                   glyph_id == other.glyph_id && field_pixel_size == other.field_pixel_size &&
                   range_bits == other.range_bits;
        }
    };

    struct KeyHash {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };

    struct Entry {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        Key key;
        std::shared_ptr<const MsdfGlyph> glyph;
        // The same field as an AtlasBitmap, so the uploader's byte source can
        // hand it back without repacking on every upload.
        std::shared_ptr<const AtlasBitmap> bitmap;
        // nullopt for an inkless glyph or one the atlas could not hold.
        std::optional<GlyphSlot> slot;
        // The atlas key this entry reserved, unique for the entry's life.
        std::uint64_t atlas_key = 0;
        bool in_atlas = false;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    struct Result {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        std::shared_ptr<const MsdfGlyph> glyph;
        std::optional<GlyphSlot> slot;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    [[nodiscard]] Result GetOrGenerate(const FontFace& face, std::uint32_t glyph_id,
                                       const MsdfConfig& config);

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    HarfBuzzOutlineAdapter adapter_;
    GlyphAtlas atlas_;
    mutable std::mutex mutex_;
    // Most recently used entry first; the back is the eviction candidate.
    std::list<Entry> entries_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
    std::size_t max_entries_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t next_atlas_key_ = 1;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Text
