module;

#include <linebreak.h>

export module VulkanEngine.Text.Layout;

import std;
import std.compat;

import VulkanEngine.Text.Font;
import VulkanEngine.Text.Shaping;

export namespace VulkanEngine::Text {

enum class TextAlign {
    Left,
    Center,
    Right,
};

// How a shaped run is wrapped and placed in its box.
struct LayoutOptions {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // Pixel size the design-unit advances are scaled by. The run itself is
    // size-independent, so this is the only place a size enters.
    float pixel_size = 16.0f;
    // Wrapping width in pixels. 0 means "no wrapping": only explicit newlines
    // break lines.
    float max_width = 0.0f;
    TextAlign align = TextAlign::Left;
    // Multiple of FontMetrics::LineAdvance() (scaled to pixels) between
    // consecutive baselines.
    float line_height_scale = 1.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// One laid-out line, as a contiguous range of the run's glyphs.
struct LayoutLine {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::size_t first_glyph = 0;
    std::size_t glyph_count = 0;
    // Measured width in pixels, excluding the whitespace that triggered the
    // break at the end of the line.
    float width = 0.0f;
    // Horizontal position of the line inside its box, in pixels; the alignment
    // offset is already folded in.
    float offset_x = 0.0f;
    // Distance from the top of the text block to this line's baseline, in
    // pixels.
    float advance_y = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct TextLayout {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<LayoutLine> lines;
    // Width of the widest line in pixels.
    float width = 0.0f;
    // lines.size() * scaled LineAdvance() * line_height_scale.
    float height = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Wraps and aligns a shaped run.
//
// Break opportunities come from UAX#14 over the run's source text and are
// mapped onto glyphs through their cluster values, which are byte offsets into
// that same text. An explicit newline always breaks, with or without wrapping.
// Wrapping is greedy and only ever happens at an allowed opportunity, so an
// unbreakable segment wider than max_width overflows onto its own line instead
// of being split mid-word: there is no hyphenation here.
[[nodiscard]] TextLayout LayoutText(const FontFace& face, const ShapedRun& run,
                                    const LayoutOptions& options = {});

} // namespace VulkanEngine::Text
