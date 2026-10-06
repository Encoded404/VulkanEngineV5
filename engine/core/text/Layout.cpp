module;

#include <linebreak.h>

module VulkanEngine.Text.Layout;

import std;
import std.compat;

import VulkanEngine.Text.Font;
import VulkanEngine.Text.Shaping;

namespace VulkanEngine::Text {

namespace {

// The line-break opportunity that follows a character, as libunibreak reports
// it in the per-byte class array. LINEBREAK_INDETERMINATE is the end of input
// on a non-EOL character, which is not an opportunity to start a new line.
[[nodiscard]] bool IsBreak(char line_break_class) noexcept {
    return line_break_class == LINEBREAK_MUSTBREAK || line_break_class == LINEBREAK_ALLOWBREAK;
}

// True when the glyph came from an explicit newline. Lato gives U+000A the
// .notdef glyph with a real advance, so a newline would otherwise widen the
// line it terminates; the glyph stays in the line, measured as zero.
[[nodiscard]] bool IsNewlineGlyph(const ShapedRun& run, const ShapedGlyph& glyph) noexcept {
    return glyph.cluster < run.text.size() && run.text[glyph.cluster] == '\n';
}

// Whitespace that a break leaves at the end of a line. It stays in the line's
// glyph range but is not measured, so the space that caused a break cannot push
// the line it ends past max_width.
[[nodiscard]] bool IsTrailingWhitespace(const ShapedRun& run, const ShapedGlyph& glyph) noexcept {
    if (glyph.cluster >= run.text.size()) {
        return false;
    }
    const char byte = run.text[glyph.cluster];
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

// A stretch of the run between two line-break opportunities: the unit wrapping
// can move from one line to the next, because there is no legal break inside
// it. `visible_width` excludes the whitespace that already trails it, which is
// what the greedy decision compares against.
struct Segment {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::size_t first_glyph = 0;
    std::size_t glyph_count = 0;
    float visible_width = 0.0f;
    // True when an explicit newline ends the line *before* this segment, in
    // which case it breaks even with wrapping off and even when the line it
    // ends is empty.
    bool mandatory_break = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace

TextLayout LayoutText(const FontFace& face, const ShapedRun& run, const LayoutOptions& options) {
    TextLayout layout;
    if (run.glyphs.empty()) {
        return layout;
    }

    const float scale = face.ScaleForSize(options.pixel_size);
    const float line_height =
        static_cast<float>(face.Metrics().LineAdvance()) * scale * options.line_height_scale;

    // UAX#14 over the exact bytes the clusters index. The class array is per
    // byte; a multi-byte character carries its class on its last byte and
    // LINEBREAK_INSIDEACHAR on the earlier ones, so the class of the character
    // ending at byte B-1 is what decides whether a line may start at byte B.
    std::vector<char> breaks(run.text.size(), LINEBREAK_NOBREAK);
    if (!run.text.empty()) {
        set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(run.text.data()), run.text.size(),
                            nullptr, breaks.data());
    }

    // The class of the character that ends immediately before glyph
    // `glyph_index`; a break opportunity there means a line may start at this
    // glyph, because its cluster is that byte offset.
    const auto break_class_before = [&](std::size_t glyph_index) {
        const std::uint32_t offset = run.glyphs[glyph_index].cluster;
        if (offset == 0 || offset > run.text.size()) {
            return static_cast<char>(LINEBREAK_NOBREAK);
        }
        return breaks[offset - 1];
    };

    const std::size_t glyph_count = run.glyphs.size();
    std::vector<float> widths(glyph_count, 0.0f);
    std::vector<char> trailing_whitespace(glyph_count, 0);
    for (std::size_t i = 0; i < glyph_count; ++i) {
        widths[i] = IsNewlineGlyph(run, run.glyphs[i])
                        ? 0.0f
                        : run.glyphs[i].advance_x * scale;
        trailing_whitespace[i] = IsTrailingWhitespace(run, run.glyphs[i]) ? 1 : 0;
    }

    // Cut the run into segments at break opportunities. Glyphs that share a
    // cluster describe the same byte offset, so only the first of them can
    // start the line a break at that offset creates.
    std::vector<Segment> segments;
    segments.push_back(Segment{});
    for (std::size_t i = 1; i < glyph_count; ++i) {
        if (run.glyphs[i].cluster == run.glyphs[i - 1].cluster) {
            continue;
        }
        const char line_break_class = break_class_before(i);
        if (!IsBreak(line_break_class)) {
            continue;
        }
        segments.back().glyph_count = i - segments.back().first_glyph;
        Segment next{};
        next.first_glyph = i;
        next.mandatory_break = line_break_class == LINEBREAK_MUSTBREAK;
        segments.push_back(next);
    }
    segments.back().glyph_count = glyph_count - segments.back().first_glyph;

    for (Segment& segment : segments) {
        float width = 0.0f;
        for (std::size_t i = segment.first_glyph; i < segment.first_glyph + segment.glyph_count;
             ++i) {
            width += widths[i];
            if (trailing_whitespace[i] == 0) {
                segment.visible_width = width;
            }
        }
    }

    std::size_t line_start = 0;
    // Every glyph added to the current line, trailing whitespace included:
    // what the wrap decision measures, because whitespace that is not trailing
    // any more (the next segment stayed on this line) is part of the line.
    float line_width = 0.0f;
    // The same without its trailing whitespace: what the line reports.
    float measured_width = 0.0f;

    const auto push_line = [&](std::size_t end) {
        LayoutLine line{};
        line.first_glyph = line_start;
        line.glyph_count = end - line_start;
        line.width = measured_width;
        layout.lines.push_back(line);
        line_start = end;
        line_width = 0.0f;
        measured_width = 0.0f;
    };

    for (const Segment& segment : segments) {
        if (segment.first_glyph > line_start) {
            if (segment.mandatory_break) {
                push_line(segment.first_glyph);
            } else if (options.max_width > 0.0f &&
                       line_width + segment.visible_width > options.max_width) {
                // Greedy wrap at the last legal opportunity, looking at the
                // whole next segment rather than one glyph: a word that fits on
                // a line of its own moves there instead of overhanging this
                // one. A segment wider than max_width is never split -- it
                // simply overflows its line, since there is no hyphenation.
                push_line(segment.first_glyph);
            }
        }
        for (std::size_t i = segment.first_glyph; i < segment.first_glyph + segment.glyph_count;
             ++i) {
            line_width += widths[i];
            if (trailing_whitespace[i] == 0) {
                measured_width = line_width;
            }
        }
    }
    push_line(glyph_count);

    for (LayoutLine& line : layout.lines) {
        layout.width = std::max(layout.width, line.width);
    }

    // The box alignment is computed against: the wrap width when there is one,
    // otherwise the widest line. When an unbreakable line overflows the wrap
    // width, the widest line wins too, or centring and right alignment would
    // push the overflowing line outside its own box.
    const float box_width = std::max(options.max_width, layout.width);
    for (std::size_t i = 0; i < layout.lines.size(); ++i) {
        LayoutLine& line = layout.lines[i];
        line.advance_y = static_cast<float>(i) * line_height;
        switch (options.align) {
            case TextAlign::Left:
                line.offset_x = 0.0f;
                break;
            case TextAlign::Center:
                line.offset_x = (box_width - line.width) * 0.5f;
                break;
            case TextAlign::Right:
                line.offset_x = box_width - line.width;
                break;
        }
    }

    layout.height = static_cast<float>(layout.lines.size()) * line_height;
    return layout;
}

} // namespace VulkanEngine::Text
