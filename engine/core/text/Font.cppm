module;

#include <hb.h>

export module VulkanEngine.Text.Font;

import std;
import std.compat;

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.FontResource;

export namespace VulkanEngine::Text {

// Metrics in font design units (y up: ascender positive, descender negative),
// taken from the same HarfBuzz face that shapes the text. Layout must not mix
// in a second source of metrics: line height and advances have to come from the
// engine that produced the advances, or wrapped lines drift from shaped glyphs.
//
// Cap height, x height and the underline metrics come from the OS/2 table and
// are absent from fonts that omit them; they are reported as 0 rather than
// guessed.
struct FontMetrics {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t units_per_em = 0;
    std::int32_t ascender = 0;
    std::int32_t descender = 0;
    std::int32_t line_gap = 0;
    std::int32_t cap_height = 0;
    std::int32_t x_height = 0;
    std::int32_t underline_position = 0;
    std::int32_t underline_thickness = 0;
    std::uint32_t glyph_count = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // Distance between consecutive baselines, in design units.
    [[nodiscard]] std::int32_t LineAdvance() const noexcept {
        return ascender - descender + line_gap;
    }
};

// One glyph's ink box and advances at a requested pixel size.
//
// Coordinates are y-up, matching the font's design space: `bearing_y` is the
// TOP edge of the box measured up from the baseline, and `width`/`height` are
// positive extents. (HarfBuzz reports the box as a bearing plus a negative
// height; that convention stops here.)
struct GlyphMetrics {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t glyph_id = 0;
    float advance_x = 0.0f;
    float advance_y = 0.0f;
    float bearing_x = 0.0f;
    float bearing_y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// An opened font face, shared and immutable.
//
// The face is built once and then never mutated, which is what makes it safe to
// use from many threads at the same time: HarfBuzz's object-lifecycle calls are
// thread-safe, and an object that has been marked immutable can be read
// concurrently. Shaping and layout therefore run on worker threads against one
// shared face instead of a per-thread copy.
//
// HarfBuzz handles appear in this interface deliberately. Shaping *is* the
// other half of this face, and hiding the font behind an opaque wrapper would
// only move the same pointer through a cast while making the shaping layer
// impossible to write. Modules that do not shape text do not import this one.
//
// Scale is pinned to the face's units-per-em, so every advance, bearing and
// metric is reported in design units and no result depends on the requested
// pixel size. That is what lets one shaped run be cached and reused at every
// size; callers scale with ScaleForSize().
class FontFace {
public:
    // Returns nullptr when the resource holds no bytes, when the container
    // reports fewer faces than `face_index`, or when HarfBuzz opens the face
    // but finds no glyphs in it (a bad index or an unusable font).
    //
    // The face takes its own copy of the font bytes: HarfBuzz must be able to
    // read them for as long as the face lives, and tying that to the resource's
    // lifetime instead would make face lifetime depend on resource eviction.
    // One buffer per opened face is the whole cost.
    [[nodiscard]] static std::shared_ptr<FontFace> Create(const FontResource& resource,
                                                         std::uint32_t face_index = 0);

    ~FontFace();

    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;
    FontFace(FontFace&&) = delete;
    FontFace& operator=(FontFace&&) = delete;

    [[nodiscard]] const FontMetrics& Metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::uint32_t FaceIndex() const noexcept { return face_index_; }
    // The version of the resource this face was opened from, so caches keyed on
    // a face can tell when a reloaded font has replaced it.
    [[nodiscard]] std::uint32_t ResourceVersion() const noexcept { return resource_version_; }

    // The glyph id for `codepoint`, or MissingGlyph() when the font has none.
    [[nodiscard]] std::uint32_t GlyphForCodepoint(std::uint32_t codepoint) const noexcept;

    // Glyph 0, the .notdef box. Always resolvable, so callers can render a
    // visible placeholder rather than dropping the character.
    [[nodiscard]] static constexpr std::uint32_t MissingGlyph() noexcept { return 0U; }

    // Design units to pixels for a requested pixel size.
    [[nodiscard]] float ScaleForSize(float pixel_size) const noexcept;

    [[nodiscard]] GlyphMetrics GlyphAtSize(std::uint32_t glyph_id, float pixel_size) const noexcept;

    // Raw handles for the shaping layer. Both are immutable.
    [[nodiscard]] hb_font_t* HarfBuzzFont() const noexcept { return font_; }
    [[nodiscard]] hb_face_t* HarfBuzzFace() const noexcept { return face_; }

private:
    FontFace(hb_face_t* face, hb_font_t* font, std::uint32_t face_index,
             std::uint32_t resource_version, FontMetrics metrics);

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    hb_face_t* face_ = nullptr;
    hb_font_t* font_ = nullptr;
    std::uint32_t face_index_ = 0;
    std::uint32_t resource_version_ = 0;
    FontMetrics metrics_{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Text
