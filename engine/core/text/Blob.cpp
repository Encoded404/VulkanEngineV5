module;

#include <hb.h>
#include <hb-gpu.h>

module VulkanEngine.Text.Blob;

import std;
import std.compat;

import VulkanEngine.Text.Font;

namespace VulkanEngine::Text {

namespace {

// boost-style hash_combine, same as the shaping, glyph and MSDF caches. The hash
// only chooses a bucket; equality decides whether two keys are the same entry.
[[nodiscard]] std::size_t MixHash(std::size_t seed, std::size_t value) noexcept {
    return seed ^ (value + static_cast<std::size_t>(0x9E3779B97F4A7C15ULL) + (seed << 6U) +
                   (seed >> 2U));
}

// Encodes `glyph_id` through the store's one reused encoder.
//
// This is outside the cache lock -- encoding is the expensive part and holding
// the cache mutex across it would serialize callers that want different glyphs.
// The encoder itself is a single mutable scratch object, so the store's encoder
// mutex is what keeps two callers from interleaving their draw callbacks; the
// cache lock is deliberately not held here.
[[nodiscard]] std::shared_ptr<const GlyphBlob> EncodeGlyph(hb_gpu_draw_t* draw,
                                                           std::mutex& encoder_mutex,
                                                           const FontFace& face,
                                                           std::uint32_t glyph_id) {
    const std::scoped_lock lock(encoder_mutex);
    hb_gpu_draw_clear(draw);
    hb_gpu_draw_glyph(draw, face.HarfBuzzFont(), glyph_id);

    hb_glyph_extents_t extents{};
    hb_blob_t* blob = hb_gpu_draw_encode(draw, &extents);
    auto glyph = std::make_shared<GlyphBlob>();
    glyph->glyph_id = glyph_id;
    glyph->x_bearing = extents.x_bearing;
    glyph->y_bearing = extents.y_bearing;
    glyph->width = extents.width;
    glyph->height = extents.height;
    if (blob != nullptr) {
        unsigned int length = 0;
        const char* data = hb_blob_get_data(blob, &length);
        if (data != nullptr && length > 0) {
            glyph->bytes.resize(length);
            std::memcpy(glyph->bytes.data(), data, length);
        }
        // The blob goes back to the encoder rather than to hb_blob_destroy: the
        // encoder reuses the buffer, which is how a per-glyph encode stays cheap.
        hb_gpu_draw_recycle_blob(draw, blob);
    }
    return glyph;
}

} // namespace

// Owns the one hb_gpu_draw_t the store reuses and the mutex that serializes it.
struct GlyphBlobEncoder::Impl {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    hb_gpu_draw_t* draw = nullptr;
    std::mutex encode_mutex;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    Impl() : draw(hb_gpu_draw_create_or_fail()) {}
    ~Impl() {
        if (draw != nullptr) {
            hb_gpu_draw_destroy(draw);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

std::size_t GlyphBlobEncoder::KeyHash::operator()(const Key& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.face_id);
    hash = MixHash(hash, std::hash<std::uint32_t>{}(key.resource_version));
    hash = MixHash(hash, std::hash<std::uint32_t>{}(key.glyph_id));
    return hash;
}

GlyphBlobEncoder::GlyphBlobEncoder(std::size_t max_cached_glyphs)
    : impl_(std::make_unique<Impl>()), max_entries_(max_cached_glyphs) {}

GlyphBlobEncoder::~GlyphBlobEncoder() = default;

std::shared_ptr<const GlyphBlob> GlyphBlobEncoder::Get(const FontFace& face,
                                                       std::uint32_t glyph_id,
                                                       float pixel_size) {
    // Size independent by construction: the blob is in font design units, so the
    // requested pixel size is deliberately not part of the key. Name it once so
    // the intent is impossible to miss.
    static_cast<void>(pixel_size);
    if (impl_->draw == nullptr) {
        return nullptr;
    }

    const Key key{face.UniqueId(), face.ResourceVersion(), glyph_id};
    {
        const std::scoped_lock lock(mutex_);
        const auto found = index_.find(key);
        if (found != index_.end()) {
            ++hits_;
            entries_.splice(entries_.begin(), entries_, found->second);
            return found->second->blob;
        }
    }

    auto blob = EncodeGlyph(impl_->draw, impl_->encode_mutex, face, glyph_id);
    if (blob == nullptr) {
        return nullptr;
    }

    const std::scoped_lock lock(mutex_);
    ++encodes_;
    if (const auto existing = index_.find(key); existing != index_.end()) {
        // Two threads raced on one key: the loser reuses the winner's entry.
        ++hits_;
        entries_.splice(entries_.begin(), entries_, existing->second);
        return existing->second->blob;
    }
    if (max_entries_ == 0) {
        return blob;
    }

    Entry entry;
    entry.key = key;
    entry.blob = blob;
    entries_.push_front(std::move(entry));
    index_.emplace(entries_.front().key, entries_.begin());
    while (entries_.size() > max_entries_) {
        index_.erase(entries_.back().key);
        entries_.pop_back();
    }
    return blob;
}

std::size_t GlyphBlobEncoder::Size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

std::uint64_t GlyphBlobEncoder::HitCount() const {
    const std::scoped_lock lock(mutex_);
    return hits_;
}

std::uint64_t GlyphBlobEncoder::EncodeCount() const {
    const std::scoped_lock lock(mutex_);
    return encodes_;
}

void GlyphBlobEncoder::Clear() {
    const std::scoped_lock lock(mutex_);
    entries_.clear();
    index_.clear();
}

std::optional<std::uint64_t> BlobRangeAllocator::Allocate(std::uint64_t size) {
    if (size == 0) {
        return std::nullopt;
    }
    // The shader indexes the buffer in 8-byte units, so every range starts and
    // ends on an 8-byte boundary; rounding the request up is what guarantees it.
    const std::uint64_t rounded = ((size + kAlignment - 1U) / kAlignment) * kAlignment;

    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < rounded) {
            continue;
        }
        const std::uint64_t offset = it->first;
        const std::uint64_t remaining = it->second - rounded;
        free_.erase(it);
        if (remaining > 0) {
            free_.emplace(offset + rounded, remaining);
        }
        live_.emplace(offset, rounded);
        used_bytes_ += rounded;
        return offset;
    }

    const std::uint64_t offset = high_water_;
    high_water_ += rounded;
    live_.emplace(offset, rounded);
    used_bytes_ += rounded;
    return offset;
}

void BlobRangeAllocator::Free(std::uint64_t offset) {
    const auto found = live_.find(offset);
    if (found == live_.end()) {
        return;
    }
    std::uint64_t start = found->first;
    std::uint64_t size = found->second;
    live_.erase(found);
    used_bytes_ -= size;

    // Coalesce with the neighbouring free block on the right, then the one on
    // the left, so a run of frees becomes one reusable range instead of a trail
    // of fragments that no larger blob can fit.
    const auto right = free_.find(start + size);
    if (right != free_.end()) {
        size += right->second;
        free_.erase(right);
    }
    const auto left = free_.upper_bound(start);
    if (left != free_.begin()) {
        auto previous = std::prev(left);
        if (previous->first + previous->second == start) {
            start = previous->first;
            size += previous->second;
            free_.erase(previous);
        }
    }
    free_.emplace(start, size);
}

void BlobRangeAllocator::Reset() noexcept {
    free_.clear();
    live_.clear();
    high_water_ = 0;
    used_bytes_ = 0;
}

} // namespace VulkanEngine::Text
