module;

#include <hb.h>

module VulkanEngine.Text.Shaping;

import std;
import std.compat;

import VulkanEngine.Text.Font;

namespace VulkanEngine::Text {

namespace {

// The most features any current option combination asks for: -kern, -liga,
// -clig.
constexpr std::size_t kMaxFeatures = 3;

struct BufferDeleter {
    void operator()(hb_buffer_t* buffer) const noexcept { hb_buffer_destroy(buffer); }
};

using BufferPtr = std::unique_ptr<hb_buffer_t, BufferDeleter>;

[[nodiscard]] hb_direction_t ToHarfBuzzDirection(TextDirection direction) {
    return direction == TextDirection::RightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
}

// Parses one HarfBuzz feature string; a string HarfBuzz cannot parse is a
// programming error in this file and is left out rather than aborting shaping.
void AddFeature(hb_feature_t* features, std::size_t& count, std::string_view spec) {
    if (count >= kMaxFeatures) {
        return;
    }
    if (hb_feature_from_string(spec.data(), static_cast<int>(spec.size()), &features[count]) != 0) {
        ++count;
    }
}

// Turns the options into OpenType feature overrides. Only *disabled* features
// need an entry: `kern`, `liga` and `clig` are on by default in HarfBuzz, so
// silently dropping the overrides would make "off" behave like "on".
[[nodiscard]] std::size_t BuildFeatures(const ShapeOptions& options,
                                        std::array<hb_feature_t, kMaxFeatures>& features) {
    std::size_t count = 0;
    if (!options.kerning) {
        AddFeature(features.data(), count, "-kern");
    }
    if (!options.ligatures) {
        AddFeature(features.data(), count, "-liga");
        AddFeature(features.data(), count, "-clig");
    }
    return count;
}

// boost-style hash_combine. The hash is only a bucket selector: equality is
// what decides whether two keys are the same entry, so a collision costs a
// comparison and never a wrong run.
[[nodiscard]] std::size_t MixHash(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + static_cast<std::size_t>(0x9E3779B97F4A7C15ULL) + (seed << 6U) +
                   (seed >> 2U));
}

} // namespace

ShapedRun ShapeText(const FontFace& face, std::string_view utf8, const ShapeOptions& options) {
    ShapedRun run;
    run.text.assign(utf8);
    if (utf8.empty()) {
        return run;
    }

    BufferPtr buffer{hb_buffer_create()};
    hb_buffer_add_utf8(buffer.get(), utf8.data(), static_cast<int>(utf8.size()), 0,
                       static_cast<int>(utf8.size()));
    // Set the direction before guessing: HarfBuzz derives script and language
    // from the text and keeps an already-decided direction, so an explicit
    // RightToLeft survives the guess.
    hb_buffer_set_direction(buffer.get(), ToHarfBuzzDirection(options.direction));
    hb_buffer_guess_segment_properties(buffer.get());
    // Character-level clusters: every glyph reports the byte offset of the
    // character it came from, which is exactly what mapping UAX#14 break
    // offsets onto glyph ranges needs. The default grapheme level is coarser.
    hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

    std::array<hb_feature_t, kMaxFeatures> features{};
    const std::size_t feature_count = BuildFeatures(options, features);

    hb_shape(face.HarfBuzzFont(), buffer.get(), features.data(),
             static_cast<unsigned int>(feature_count));

    unsigned int glyph_count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer.get(), &glyph_count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer.get(), &glyph_count);

    run.glyphs.reserve(glyph_count);
    for (unsigned int i = 0; i < glyph_count; ++i) {
        ShapedGlyph glyph{};
        glyph.glyph_id = infos[i].codepoint;
        glyph.advance_x = static_cast<float>(positions[i].x_advance);
        glyph.advance_y = static_cast<float>(positions[i].y_advance);
        glyph.offset_x = static_cast<float>(positions[i].x_offset);
        glyph.offset_y = static_cast<float>(positions[i].y_offset);
        glyph.cluster = infos[i].cluster;
        run.total_advance_x += glyph.advance_x;
        run.total_advance_y += glyph.advance_y;
        run.glyphs.push_back(glyph);
    }
    return run;
}

ShapingCache::ShapingCache(std::size_t max_entries) : max_entries_(max_entries) {}

std::size_t ShapingCache::KeyHash::operator()(const Key& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.face_id);
    hash = MixHash(hash, std::hash<std::uint32_t>{}(key.resource_version));
    hash = MixHash(hash, std::hash<std::string>{}(key.text));
    hash = MixHash(hash, std::hash<int>{}(static_cast<int>(key.direction)));
    hash = MixHash(hash, std::hash<bool>{}(key.kerning));
    hash = MixHash(hash, std::hash<bool>{}(key.ligatures));
    return hash;
}

std::shared_ptr<const ShapedRun> ShapingCache::Shape(const FontFace& face, std::string_view utf8,
                                                     const ShapeOptions& options) {
    Key key{face.UniqueId(),
            face.ResourceVersion(),
            std::string(utf8),
            options.direction,
            options.kerning,
            options.ligatures};

    {
        const std::scoped_lock lock(mutex_);
        const auto found = index_.find(key);
        if (found != index_.end()) {
            ++hits_;
            entries_.splice(entries_.begin(), entries_, found->second);
            return found->second->second;
        }
    }

    // Shape without the lock. Two threads racing on the same key simply shape
    // it twice and the loser reuses the winner's entry below.
    auto run = std::make_shared<const ShapedRun>(ShapeText(face, utf8, options));
    if (max_entries_ == 0) {
        return run;
    }

    const std::scoped_lock lock(mutex_);
    const auto existing = index_.find(key);
    if (existing != index_.end()) {
        ++hits_;
        entries_.splice(entries_.begin(), entries_, existing->second);
        return existing->second->second;
    }

    entries_.push_front(Entry{std::move(key), run});
    index_.emplace(entries_.front().first, entries_.begin());
    while (entries_.size() > max_entries_) {
        index_.erase(entries_.back().first);
        entries_.pop_back();
    }
    return run;
}

void ShapingCache::Clear() {
    const std::scoped_lock lock(mutex_);
    index_.clear();
    entries_.clear();
}

void ShapingCache::InvalidateFace(std::uint64_t face_id) {
    const std::scoped_lock lock(mutex_);
    // Erase through the index's iterators so the list and the map stay in step;
    // a run already returned to a caller is kept alive by its shared_ptr.
    for (auto it = index_.begin(); it != index_.end();) {
        if (it->first.face_id != face_id) {
            ++it;
            continue;
        }
        entries_.erase(it->second);
        it = index_.erase(it);
    }
}

std::size_t ShapingCache::Size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

std::uint64_t ShapingCache::HitCount() const {
    const std::scoped_lock lock(mutex_);
    return hits_;
}

} // namespace VulkanEngine::Text
