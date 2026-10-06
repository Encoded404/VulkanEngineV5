module;

#include <hb.h>
#include <hb-ot.h>

module VulkanEngine.Text.Font;

import std;
import std.compat;

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.FontResource;
import VulkanEngine.FileLoaders.Fonts;

namespace VulkanEngine::Text {

namespace {

// Owns the font bytes for as long as the HarfBuzz blob reading them exists.
// The blob is created read-only over this buffer and releases it through this
// callback, so the memory cannot be freed while any face still points at it,
// regardless of the order in which the owning FontFace is torn down.
struct FontBytesOwner {
    std::shared_ptr<const std::vector<std::byte>> bytes;
};

void DestroyFontBytesOwner(void* user_data) {
    delete static_cast<FontBytesOwner*>(user_data);
}

[[nodiscard]] hb_blob_t* MakeFontBlob(std::shared_ptr<const std::vector<std::byte>> bytes) {
    auto* owner = new FontBytesOwner{std::move(bytes)};
    return hb_blob_create(reinterpret_cast<const char*>(owner->bytes->data()),
                          static_cast<unsigned int>(owner->bytes->size()),
                          HB_MEMORY_MODE_READONLY, owner, DestroyFontBytesOwner);
}

// An OS/2-derived metric, or 0 when the font does not carry it.
[[nodiscard]] std::int32_t OptionalMetric(hb_font_t* font, hb_ot_metrics_tag_t tag) {
    hb_position_t position = 0;
    return hb_ot_metrics_get_position(font, tag, &position)
               ? static_cast<std::int32_t>(position)
               : 0;
}

void ReadVerticalMetrics(hb_font_t* font, FontMetrics& metrics) {
    hb_font_extents_t extents{};
    if (hb_font_get_h_extents(font, &extents)) {
        metrics.ascender = static_cast<std::int32_t>(extents.ascender);
        metrics.descender = static_cast<std::int32_t>(extents.descender);
        metrics.line_gap = static_cast<std::int32_t>(extents.line_gap);
        return;
    }
    // A font with no usable horizontal header still has OS/2 metrics in
    // practice, so fall back to those rather than reporting a zero line box.
    metrics.ascender = OptionalMetric(font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER);
    metrics.descender = OptionalMetric(font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER);
    metrics.line_gap = OptionalMetric(font, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP);
}

} // namespace

std::shared_ptr<FontFace> FontFace::Create(const FontResource& resource, std::uint32_t face_index) {
    if (!resource.HasBytes()) {
        return nullptr;
    }

    const FileLoaders::Fonts::FontContainerInfo container = resource.GetContainerInfo();
    if (!container.IsValid() || face_index >= container.face_count) {
        return nullptr;
    }

    // HarfBuzz reads the sfnt buffer for the whole life of the face, so the face
    // owns its copy. The blob hands the bytes on to the face and then drops its
    // own reference.
    hb_blob_t* blob =
        MakeFontBlob(std::make_shared<const std::vector<std::byte>>(resource.GetBytes()));
    hb_face_t* face = hb_face_create(blob, face_index);
    hb_blob_destroy(blob);
    if (face == nullptr) {
        return nullptr;
    }

    // A face that opens but exposes no glyphs is either the wrong index in a
    // collection or an unusable font; both are failures, not empty fonts.
    if (hb_face_get_glyph_count(face) == 0) {
        hb_face_destroy(face);
        return nullptr;
    }
    hb_face_make_immutable(face);

    hb_font_t* font = hb_font_create(face);
    if (font == nullptr) {
        hb_face_destroy(face);
        return nullptr;
    }

    // Use HarfBuzz's own OpenType implementation. Without it the font has no
    // metrics, no advances and no shaping funcs, and every query silently
    // returns zero.
    hb_ot_font_set_funcs(font);

    const std::uint32_t upem = hb_face_get_upem(face);
    // Pin the scale to units-per-em: every result is then in design units, and
    // none of them depends on a requested pixel size. Callers scale at layout
    // time with ScaleForSize().
    hb_font_set_scale(font, static_cast<int>(upem), static_cast<int>(upem));
    hb_font_make_immutable(font);

    FontMetrics metrics{};
    metrics.units_per_em = upem;
    metrics.glyph_count = hb_face_get_glyph_count(face);
    ReadVerticalMetrics(font, metrics);
    metrics.cap_height = OptionalMetric(font, HB_OT_METRICS_TAG_CAP_HEIGHT);
    metrics.x_height = OptionalMetric(font, HB_OT_METRICS_TAG_X_HEIGHT);
    metrics.underline_position = OptionalMetric(font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET);
    metrics.underline_thickness = OptionalMetric(font, HB_OT_METRICS_TAG_UNDERLINE_SIZE);

    return std::shared_ptr<FontFace>(
        new FontFace(face, font, face_index, resource.GetVersion(), metrics));
}

FontFace::FontFace(hb_face_t* face, hb_font_t* font, std::uint32_t face_index,
                   std::uint32_t resource_version, FontMetrics metrics)
    : face_(face),
      font_(font),
      face_index_(face_index),
      resource_version_(resource_version),
      metrics_(metrics) {}

FontFace::~FontFace() {
    if (font_ != nullptr) {
        hb_font_destroy(font_);
    }
    if (face_ != nullptr) {
        hb_face_destroy(face_);
    }
}

std::uint32_t FontFace::GlyphForCodepoint(std::uint32_t codepoint) const noexcept {
    hb_codepoint_t glyph = 0;
    if (hb_font_get_nominal_glyph(font_, codepoint, &glyph)) {
        return glyph;
    }
    return MissingGlyph();
}

float FontFace::ScaleForSize(float pixel_size) const noexcept {
    if (metrics_.units_per_em == 0) {
        return 0.0f;
    }
    return pixel_size / static_cast<float>(metrics_.units_per_em);
}

GlyphMetrics FontFace::GlyphAtSize(std::uint32_t glyph_id, float pixel_size) const noexcept {
    const float scale = ScaleForSize(pixel_size);

    GlyphMetrics glyph{};
    glyph.glyph_id = glyph_id;
    glyph.advance_x = static_cast<float>(hb_font_get_glyph_h_advance(font_, glyph_id)) * scale;
    glyph.advance_y = static_cast<float>(hb_font_get_glyph_v_advance(font_, glyph_id)) * scale;

    hb_glyph_extents_t extents{};
    if (hb_font_get_glyph_extents(font_, glyph_id, &extents)) {
        // HarfBuzz describes a y-up box as a top edge plus a *negative* height,
        // because the box extends downward from its bearing. Convert to two
        // edges and positive sizes here so no caller has to know that.
        const float left = static_cast<float>(extents.x_bearing) * scale;
        const float top = static_cast<float>(extents.y_bearing) * scale;
        const float right = left + (static_cast<float>(extents.width) * scale);
        const float bottom = top + (static_cast<float>(extents.height) * scale);
        glyph.bearing_x = left;
        glyph.bearing_y = top;
        glyph.width = right - left;
        glyph.height = top - bottom;
    }
    return glyph;
}

} // namespace VulkanEngine::Text
