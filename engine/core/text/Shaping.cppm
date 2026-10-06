module;

#include <hb.h>

export module VulkanEngine.Text.Shaping;

import std;
import std.compat;

import VulkanEngine.Text.Font;

export namespace VulkanEngine::Text {

// One glyph as HarfBuzz positioned it, in font design units.
//
// The face's scale is pinned to its units-per-em, so nothing here is scaled to
// a pixel size: advances and offsets are the font's own numbers and the same
// run is reusable at every size. Layout multiplies by
// FontFace::ScaleForSize() once, for one requested size.
struct ShapedGlyph {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t glyph_id = 0;
    float advance_x = 0.0f;
    float advance_y = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    // Byte offset into the input UTF-8 text of the first character this glyph
    // came from. Retained (rather than replaced by an index) because layout has
    // to map UAX#14 break opportunities, which are byte offsets, onto glyph
    // ranges. The offset is always a character start, never a continuation
    // byte, and the values are non-decreasing in visual order.
    std::uint32_t cluster = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// A shaped, positioned run of one text line's worth of characters.
//
// `text` is the exact input the clusters index. It is kept because a run is the
// only thing layout receives, and break opportunities can only be computed over
// the bytes the clusters point into: substituting a different string would
// silently reinterpret every cluster offset.
struct ShapedRun {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<ShapedGlyph> glyphs;
    std::string text;
    float total_advance_x = 0.0f;
    float total_advance_y = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool Empty() const noexcept { return glyphs.empty(); }
};

enum class TextDirection {
    LeftToRight,
    RightToLeft,
};

// Shaping options that change the output. Both OpenType switches are applied as
// HarfBuzz feature strings rather than by pre-processing the text, so the
// shaping engine stays the single owner of what a run looks like.
struct ShapeOptions {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    TextDirection direction = TextDirection::LeftToRight;
    // `kern` in GPOS. Disabling it is what makes a run equal the sum of its
    // isolated glyph advances.
    bool kerning = true;
    // `liga` and `clig` in GSUB. Disabling them keeps every character its own
    // glyph, which is what a caller needs when it must map glyphs back to
    // characters one to one.
    bool ligatures = true;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Shapes `utf8` with `face`, returning design-unit advances and offsets.
//
// Characters the font does not cover are not dropped: HarfBuzz emits glyph 0
// (.notdef) for them and it stays in the run, so layout keeps a slot for every
// character and the caller can substitute a visible placeholder. Dropping them
// here would make a missing glyph silently shift the rest of the line.
[[nodiscard]] ShapedRun ShapeText(const FontFace& face, std::string_view utf8,
                                  const ShapeOptions& options = {});

// A bounded, thread-safe cache of shaped runs.
//
// The key is the face's identity together with its resource version, the exact
// text, and every option that changes shaping. It is deliberately not a hash of
// the text: two different strings that collide would then be handed each
// other's glyphs, which is a silent correctness failure rather than a
// performance one. The hash only picks a bucket; equality compares the bytes.
//
// Entries are shared pointers, so a run handed to a caller stays valid after
// the entry is evicted or the cache is cleared, and no caller can mutate a run
// another caller is reading. Eviction is least-recently-used, so the cache has
// a hard ceiling instead of growing with the number of distinct strings a
// frame happens to draw.
//
// Every method is safe to call from any thread. Shaping happens outside the
// lock, because it is the expensive part and holding the mutex across it would
// serialize the workers the cache exists to feed.
class ShapingCache {
public:
    // `max_entries == 0` disables caching: every call shapes afresh and the
    // cache stays empty.
    explicit ShapingCache(std::size_t max_entries = 512);

    [[nodiscard]] std::shared_ptr<const ShapedRun> Shape(const FontFace& face, std::string_view utf8,
                                                         const ShapeOptions& options = {});

    // Drops every entry. HitCount() is a lifetime statistic and is not reset,
    // so a flush does not erase how the cache has performed.
    void Clear();

    [[nodiscard]] std::size_t Size() const;
    [[nodiscard]] std::uint64_t HitCount() const;

private:
    struct Key {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        const FontFace* face = nullptr;
        std::uint32_t resource_version = 0;
        std::string text;
        TextDirection direction = TextDirection::LeftToRight;
        bool kerning = true;
        bool ligatures = true;
        // NOLINTEND(misc-non-private-member-variables-in-classes)

        [[nodiscard]] bool operator==(const Key& other) const noexcept {
            return face == other.face && resource_version == other.resource_version &&
                   direction == other.direction && kerning == other.kerning &&
                   ligatures == other.ligatures && text == other.text;
        }
    };

    struct KeyHash {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };

    using Entry = std::pair<Key, std::shared_ptr<const ShapedRun>>;

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    mutable std::mutex mutex_;
    // Most recently used entry first; the back is the eviction candidate.
    std::list<Entry> entries_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
    std::size_t max_entries_ = 0;
    std::uint64_t hits_ = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Text
