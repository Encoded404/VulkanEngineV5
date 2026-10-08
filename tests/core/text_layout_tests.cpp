#include <gtest/gtest.h>

import std;
import std.compat;

import FileLoader.Types;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.FontResource;
import VulkanEngine.Text.Font;
import VulkanEngine.Text.Shaping;
import VulkanEngine.Text.Layout;

namespace {

using VulkanEngine::FontResource;
using VulkanEngine::ResourceHandle;
using VulkanEngine::ResourceManager;
using VulkanEngine::Text::FontFace;
using VulkanEngine::Text::LayoutOptions;
using VulkanEngine::Text::LayoutText;
using VulkanEngine::Text::ShapedRun;
using VulkanEngine::Text::ShapeText;
using VulkanEngine::Text::TextAlign;
using VulkanEngine::Text::TextLayout;
using VulkanEngine::Text::ToPhysicalPixels;

constexpr float kPixelSize = 16.0f;

[[nodiscard]] std::filesystem::path TestFontPath() {
    return std::filesystem::path{VKENGINE_TEST_FONT_DIR} / "Lato-Regular.ttf";
}

[[nodiscard]] LayoutOptions OptionsFor(float max_width, TextAlign align = TextAlign::Left,
                                       float line_height_scale = 1.0f) {
    LayoutOptions options{};
    options.pixel_size = kPixelSize;
    options.max_width = max_width;
    options.align = align;
    options.line_height_scale = line_height_scale;
    return options;
}

// Width of a glyph range in pixels, measured straight off the run and with
// newline glyphs dropped. Computed here rather than read from the layout so the
// assertions do not simply restate what the code under test decided.
[[nodiscard]] float RangeWidth(const FontFace& face, const ShapedRun& run, std::size_t first,
                              std::size_t count) {
    const float scale = face.ScaleForSize(kPixelSize);
    float width = 0.0f;
    for (std::size_t i = first; i < first + count; ++i) {
        const auto& glyph = run.glyphs[i];
        const bool newline = glyph.cluster < run.text.size() && run.text[glyph.cluster] == '\n';
        if (!newline) {
            width += glyph.advance_x * scale;
        }
    }
    return width;
}

// Sum of every glyph advance scaled to pixels, accumulated glyph by glyph in
// the same order the layout accumulates widths. Only used on strings with no
// newline and no trailing whitespace; the identical arithmetic is what lets the
// exact-fit test sit exactly on the boundary instead of an epsilon off it.
[[nodiscard]] float RunWidth(const FontFace& face, const ShapedRun& run) {
    const float scale = face.ScaleForSize(kPixelSize);
    float width = 0.0f;
    for (const auto& glyph : run.glyphs) {
        width += glyph.advance_x * scale;
    }
    return width;
}

class TextLayoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        handle_ = manager_.LoadFromFile<FontResource>(
            TestFontPath(), ResourceManager::LoadSpeed::Instant);
        ASSERT_TRUE(handle_.IsValid());
        FontResource* resource = handle_.Get();
        ASSERT_NE(resource, nullptr);
        ASSERT_TRUE(resource->IsLoaded());

        face_ = FontFace::Create(*resource);
        ASSERT_NE(face_, nullptr);
    }

    ResourceManager manager_;
    ResourceHandle<FontResource> handle_;
    std::shared_ptr<FontFace> face_;
};

// A width just wider than the widest word forces one word per line: every pair
// of words is wider than that, so the greedy wrapper has no choice. "alpha
// bravo charlie delta echo" therefore lays out as five lines, one per word, and
// each line keeps the space that ended it.
TEST_F(TextLayoutTest, WrapsAtBreakOpportunities) {
    const std::string text = "alpha bravo charlie delta echo";
    const std::vector<std::string> words{"alpha", "bravo", "charlie", "delta", "echo"};
    const ShapedRun run = ShapeText(*face_, text);
    ASSERT_EQ(run.glyphs.size(), text.size());

    float widest = 0.0f;
    for (const std::string& word : words) {
        widest = std::max(widest, RangeWidth(*face_, run, text.find(word), word.size()));
    }
    const float max_width = widest + 1.0f;

    const TextLayout layout = LayoutText(*face_, run, OptionsFor(max_width));
    ASSERT_EQ(layout.lines.size(), words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::size_t first = text.find(words[i]);
        EXPECT_EQ(layout.lines[i].first_glyph, first);
        // The word, plus the space that ended it (except for the last line).
        const std::size_t expected_count =
            words[i].size() + (i + 1 < words.size() ? 1u : 0u);
        EXPECT_EQ(layout.lines[i].glyph_count, expected_count);
        EXPECT_FLOAT_EQ(layout.lines[i].width, RangeWidth(*face_, run, first, words[i].size()));
        EXPECT_LE(layout.lines[i].width, max_width);
        EXPECT_FLOAT_EQ(layout.lines[i].offset_x, 0.0f);
    }

    // The lines partition the run: nothing is dropped and nothing is counted
    // twice.
    std::size_t covered = 0;
    for (const auto& line : layout.lines) {
        covered += line.glyph_count;
    }
    EXPECT_EQ(covered, run.glyphs.size());
    EXPECT_FLOAT_EQ(layout.width, widest);
}

TEST_F(TextLayoutTest, DoesNotWrapWithoutAMaxWidth) {
    constexpr std::string_view kText = "The quick brown fox";
    const ShapedRun run = ShapeText(*face_, kText);

    const TextLayout layout = LayoutText(*face_, run, OptionsFor(0.0f));
    ASSERT_EQ(layout.lines.size(), 1u);
    EXPECT_EQ(layout.lines[0].first_glyph, 0u);
    EXPECT_EQ(layout.lines[0].glyph_count, run.glyphs.size());
    EXPECT_FLOAT_EQ(layout.lines[0].width, RunWidth(*face_, run));
    EXPECT_FLOAT_EQ(layout.width, RunWidth(*face_, run));
    EXPECT_FLOAT_EQ(layout.lines[0].offset_x, 0.0f);

    const float line_height =
        static_cast<float>(face_->Metrics().LineAdvance()) * face_->ScaleForSize(kPixelSize);
    EXPECT_FLOAT_EQ(layout.height, line_height);
}

// There is no hyphenation: a word with no legal break inside it is never split,
// even when it is far wider than the box. It moves onto a line of its own and
// overflows it.
TEST_F(TextLayoutTest, OverlongWordOverflowsItsOwnLine) {
    const std::string text = "a antidisestablishmentarianism";
    const ShapedRun run = ShapeText(*face_, text);
    const std::size_t word_start = text.find("antidisestablishmentarianism");
    const float max_width = RangeWidth(*face_, run, 0, word_start) + 5.0f;

    const TextLayout layout = LayoutText(*face_, run, OptionsFor(max_width));
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.lines[0].first_glyph, 0u);
    EXPECT_EQ(layout.lines[0].glyph_count, word_start);
    EXPECT_EQ(layout.lines[1].first_glyph, word_start);
    EXPECT_EQ(layout.lines[1].glyph_count, run.glyphs.size() - word_start);
    EXPECT_GT(layout.lines[1].width, max_width);
    EXPECT_FLOAT_EQ(layout.lines[1].width,
                    RangeWidth(*face_, run, word_start, run.glyphs.size() - word_start));
    EXPECT_LE(layout.lines[0].width, max_width);
}

// Alignment places each line inside the box, and the box is the wrap width when
// there is one and the widest line otherwise. The expected offsets are computed
// from the lines' own widths.
TEST_F(TextLayoutTest, AlignsLinesInTheirBox) {
    constexpr std::string_view kText = "Hello\nHi";
    const ShapedRun run = ShapeText(*face_, kText);
    const std::size_t second_line = 6; // "Hello" is five glyphs plus the newline
    const float first_width = RangeWidth(*face_, run, 0, 5);
    const float second_width = RangeWidth(*face_, run, second_line, 2);
    ASSERT_GT(first_width, second_width);

    // No wrap width: the box is the widest line, so the shorter line is inset.
    const TextLayout left = LayoutText(*face_, run, OptionsFor(0.0f, TextAlign::Left));
    const TextLayout center = LayoutText(*face_, run, OptionsFor(0.0f, TextAlign::Center));
    const TextLayout right = LayoutText(*face_, run, OptionsFor(0.0f, TextAlign::Right));
    ASSERT_EQ(left.lines.size(), 2u);
    ASSERT_EQ(center.lines.size(), 2u);
    ASSERT_EQ(right.lines.size(), 2u);

    EXPECT_FLOAT_EQ(left.lines[0].offset_x, 0.0f);
    EXPECT_FLOAT_EQ(left.lines[1].offset_x, 0.0f);
    EXPECT_FLOAT_EQ(center.lines[0].offset_x, 0.0f);
    EXPECT_FLOAT_EQ(center.lines[1].offset_x, (first_width - second_width) * 0.5f);
    EXPECT_FLOAT_EQ(right.lines[0].offset_x, 0.0f);
    EXPECT_FLOAT_EQ(right.lines[1].offset_x, first_width - second_width);

    // With a wrap width wider than any line, the box is that width instead.
    const float box_width = first_width + 50.0f;
    const TextLayout boxed_center =
        LayoutText(*face_, run, OptionsFor(box_width, TextAlign::Center));
    const TextLayout boxed_right = LayoutText(*face_, run, OptionsFor(box_width, TextAlign::Right));
    ASSERT_EQ(boxed_center.lines.size(), 2u);
    ASSERT_EQ(boxed_right.lines.size(), 2u);
    EXPECT_FLOAT_EQ(boxed_center.lines[0].offset_x, (box_width - first_width) * 0.5f);
    EXPECT_FLOAT_EQ(boxed_center.lines[1].offset_x, (box_width - second_width) * 0.5f);
    EXPECT_FLOAT_EQ(boxed_right.lines[0].offset_x, box_width - first_width);
    EXPECT_FLOAT_EQ(boxed_right.lines[1].offset_x, box_width - second_width);

    // An overflowing line is wider than the wrap width, so the box is that
    // line's width; right-aligning it must not push it out of its own box.
    const std::string overflowing = "antidisestablishmentarianism";
    const ShapedRun long_run = ShapeText(*face_, overflowing);
    const TextLayout overflowing_layout =
        LayoutText(*face_, long_run, OptionsFor(10.0f, TextAlign::Right));
    ASSERT_EQ(overflowing_layout.lines.size(), 1u);
    EXPECT_GT(overflowing_layout.width, 10.0f);
    EXPECT_FLOAT_EQ(overflowing_layout.lines[0].offset_x, 0.0f);
}

// Explicit newlines break without a wrap width, an empty line between two of
// them survives, and a run that ends with one does not grow a phantom line
// after it.
TEST_F(TextLayoutTest, BreaksOnExplicitNewlines) {
    constexpr std::string_view kText = "abc\ndef";
    const ShapedRun run = ShapeText(*face_, kText);
    const TextLayout layout = LayoutText(*face_, run, OptionsFor(0.0f));

    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.lines[0].first_glyph, 0u);
    EXPECT_EQ(layout.lines[0].glyph_count, 4u) << "the newline glyph ends the first line";
    // The newline is shaped as Lato's .notdef, so it must not be measured;
    // the line's width is the letters alone.
    EXPECT_FLOAT_EQ(layout.lines[0].width, RangeWidth(*face_, run, 0, 3));
    EXPECT_EQ(layout.lines[1].first_glyph, 4u);
    EXPECT_EQ(layout.lines[1].glyph_count, 3u);
    EXPECT_FLOAT_EQ(layout.lines[1].width, RangeWidth(*face_, run, 4, 3));

    const TextLayout empty_between = LayoutText(*face_, ShapeText(*face_, "a\n\nb"),
                                                OptionsFor(0.0f));
    ASSERT_EQ(empty_between.lines.size(), 3u);
    EXPECT_FLOAT_EQ(empty_between.lines[1].width, 0.0f) << "the middle line is empty";

    const TextLayout leading = LayoutText(*face_, ShapeText(*face_, "\nab"), OptionsFor(0.0f));
    ASSERT_EQ(leading.lines.size(), 2u);
    EXPECT_FLOAT_EQ(leading.lines[0].width, 0.0f);

    // "ab\n" is one line, not two: the newline terminates the line it ends.
    const TextLayout trailing = LayoutText(*face_, ShapeText(*face_, "ab\n"), OptionsFor(0.0f));
    ASSERT_EQ(trailing.lines.size(), 1u);
    EXPECT_EQ(trailing.lines[0].glyph_count, 3u);
    EXPECT_FLOAT_EQ(trailing.lines[0].width, RangeWidth(*face_, ShapeText(*face_, "ab\n"), 0, 2));
}

TEST_F(TextLayoutTest, EmptyInputYieldsNoLines) {
    const ShapedRun empty = ShapeText(*face_, "");
    const TextLayout layout = LayoutText(*face_, empty, OptionsFor(100.0f));

    EXPECT_TRUE(layout.lines.empty());
    EXPECT_FLOAT_EQ(layout.width, 0.0f);
    EXPECT_FLOAT_EQ(layout.height, 0.0f);
}

// Line height is the face's own line advance scaled to pixels and multiplied by
// the caller's scale, and the block's height counts the lines, not the gaps.
TEST_F(TextLayoutTest, MeasuresLineHeightFromTheFaceMetrics) {
    constexpr float kLineHeightScale = 1.5f;
    const ShapedRun run = ShapeText(*face_, "a\nb\nc");

    const TextLayout layout = LayoutText(*face_, run, OptionsFor(0.0f, TextAlign::Left, kLineHeightScale));
    ASSERT_EQ(layout.lines.size(), 3u);

    const float expected_line_height = static_cast<float>(face_->Metrics().LineAdvance()) *
                                       face_->ScaleForSize(kPixelSize) * kLineHeightScale;
    EXPECT_GT(expected_line_height, 0.0f);
    for (std::size_t i = 0; i < layout.lines.size(); ++i) {
        EXPECT_FLOAT_EQ(layout.lines[i].advance_y, static_cast<float>(i) * expected_line_height);
    }
    EXPECT_FLOAT_EQ(layout.height, 3.0f * expected_line_height);
    // One glyph per line, and 'c' is the widest of the three, so it sets the
    // block width.
    EXPECT_FLOAT_EQ(layout.width, std::max({RangeWidth(*face_, run, 0, 1),
                                           RangeWidth(*face_, run, 2, 1),
                                           RangeWidth(*face_, run, 4, 1)}));
}

// The boundary is strict: a line that fills the box exactly is not wrapped, and
// one half a pixel over is. "ab c" has a single break opportunity, after the
// space, so this isolates the comparison.
TEST_F(TextLayoutTest, ALineThatExactlyFitsDoesNotWrap) {
    constexpr std::string_view kText = "ab c";
    const ShapedRun run = ShapeText(*face_, kText);
    const float full_width = RunWidth(*face_, run);

    const TextLayout exact = LayoutText(*face_, run, OptionsFor(full_width));
    ASSERT_EQ(exact.lines.size(), 1u) << "a line exactly as wide as the box still fits";
    EXPECT_FLOAT_EQ(exact.lines[0].width, full_width);
    EXPECT_FLOAT_EQ(exact.width, full_width);

    const TextLayout tight = LayoutText(*face_, run, OptionsFor(full_width - 0.5f));
    ASSERT_EQ(tight.lines.size(), 2u);
    EXPECT_EQ(tight.lines[0].glyph_count, 3u) << "the space stays with the line it ends";
    EXPECT_FLOAT_EQ(tight.lines[0].width, RangeWidth(*face_, run, 0, 2));
    EXPECT_EQ(tight.lines[1].first_glyph, 3u);
    EXPECT_FLOAT_EQ(tight.lines[1].width, RangeWidth(*face_, run, 3, 1));
}

// The whitespace that triggers a break stays in the line's glyph range but is
// not measured: it cannot push that line past max_width and it cannot become an
// extra line of its own.
TEST_F(TextLayoutTest, TrailingWhitespaceAtABreakIsNotMeasured) {
    constexpr std::string_view kText = "hello world ";
    const ShapedRun run = ShapeText(*face_, kText);
    const float before_world = RangeWidth(*face_, run, 0, 6);
    const float world = RangeWidth(*face_, run, 6, 5);

    const TextLayout layout = LayoutText(*face_, run, OptionsFor(before_world + world - 1.0f));
    ASSERT_EQ(layout.lines.size(), 2u) << "the trailing space must not add a third line";
    EXPECT_FLOAT_EQ(layout.lines[0].width, RangeWidth(*face_, run, 0, 5));
    EXPECT_LE(layout.lines[0].width, before_world + world - 1.0f);
    EXPECT_EQ(layout.lines[1].glyph_count, 6u) << "the final space is still in the line";
    EXPECT_FLOAT_EQ(layout.lines[1].width, world);
    EXPECT_FLOAT_EQ(layout.width, std::max(RangeWidth(*face_, run, 0, 5), world));
}

// ── Logical points -> physical pixels ───────────────────────────────────────
//
// The scale conversion is the seam that lets a caller state a UI size once and
// get the same apparent size on a 1.0 output and on a 1.5 one. It is asserted
// separately from any rasterization because the failure it prevents -- text that
// silently shrinks on a scaled display -- is invisible on an unscaled one.

TEST(ToPhysicalPixelsTest, ScaleOneIsTheIdentity) {
    const auto request = ToPhysicalPixels(18.0f, 24.0f, 110.0f, 460.0f, 1.0f);
    EXPECT_FLOAT_EQ(request.pixel_size, 18.0f);
    EXPECT_FLOAT_EQ(request.x, 24.0f);
    EXPECT_FLOAT_EQ(request.y, 110.0f);
    EXPECT_FLOAT_EQ(request.max_width, 460.0f);
}

TEST(ToPhysicalPixelsTest, FractionalScaleScalesSizeOriginAndWrapWidth) {
    // 1.5 is the actual scale of the fractional-scaling panel this was written
    // against, so it is the case that has to be right.
    const auto request = ToPhysicalPixels(18.0f, 24.0f, 110.0f, 460.0f, 1.5f);
    EXPECT_FLOAT_EQ(request.pixel_size, 27.0f);
    EXPECT_FLOAT_EQ(request.x, 36.0f);
    EXPECT_FLOAT_EQ(request.y, 165.0f);
    EXPECT_FLOAT_EQ(request.max_width, 690.0f);
}

TEST(ToPhysicalPixelsTest, IntegerScaleIsExact) {
    const auto request = ToPhysicalPixels(14.0f, 10.0f, 20.0f, 0.0f, 2.0f);
    EXPECT_FLOAT_EQ(request.pixel_size, 28.0f);
    EXPECT_FLOAT_EQ(request.x, 20.0f);
    EXPECT_FLOAT_EQ(request.y, 40.0f);
}

// "No wrapping" is the absence of a distance, not a distance of zero: scaling it
// would turn every unwrapped request into a wrapped one at width 0.
TEST(ToPhysicalPixelsTest, ZeroWrapWidthStaysUnwrappedAtEveryScale) {
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const auto request = ToPhysicalPixels(16.0f, 0.0f, 0.0f, 0.0f, scale);
        EXPECT_FLOAT_EQ(request.max_width, 0.0f) << "scale " << scale;
    }
}

// A platform that cannot answer must not erase the text. A literal zero scale
// would collapse the pixel size and every glyph would vanish.
TEST(ToPhysicalPixelsTest, NonPositiveScaleFallsBackToOne) {
    for (const float scale : {0.0f, -1.0f}) {
        const auto request = ToPhysicalPixels(20.0f, 5.0f, 7.0f, 100.0f, scale);
        EXPECT_FLOAT_EQ(request.pixel_size, 20.0f) << "scale " << scale;
        EXPECT_FLOAT_EQ(request.x, 5.0f) << "scale " << scale;
        EXPECT_FLOAT_EQ(request.y, 7.0f) << "scale " << scale;
        EXPECT_FLOAT_EQ(request.max_width, 100.0f) << "scale " << scale;
    }
}

// The point of the conversion: a request expressed in points occupies the same
// fraction of the window at every scale. If this drifts, UI text changes size
// when the display scale does.
TEST(ToPhysicalPixelsTest, ProportionalSizeIsScaleInvariant) {
    constexpr float kPointSize = 18.0f;
    constexpr float kWindowPoints = 720.0f;
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const auto request = ToPhysicalPixels(kPointSize, 0.0f, 0.0f, 0.0f, scale);
        const float fraction = request.pixel_size / (kWindowPoints * scale);
        EXPECT_NEAR(fraction, kPointSize / kWindowPoints, 1e-6f) << "scale " << scale;
    }
}

} // namespace
