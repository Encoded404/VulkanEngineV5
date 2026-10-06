module;

module VulkanEngine.Text.Atlas;

import std;
import std.compat;

namespace VulkanEngine::Text {

namespace {

[[nodiscard]] std::uint64_t RectArea(std::uint64_t width, std::uint64_t height) noexcept {
    return width * height;
}

[[nodiscard]] std::uint64_t RectArea(const AtlasRect& rect) noexcept {
    return RectArea(rect.width, rect.height);
}

// Grows `target` into the bounding box of `target` and `rect`. 64-bit math
// throughout: page coordinates are 32-bit, but the sums of two of them are not
// guaranteed to be, and a wrapped comparison could silently let two rectangles
// overlap.
void UnionInto(std::optional<AtlasRect>& target, const AtlasRect& rect) noexcept {
    if (!target) {
        target = rect;
        return;
    }
    const AtlasRect current = *target;
    const std::uint64_t left = std::min<std::uint64_t>(current.x, rect.x);
    const std::uint64_t top = std::min<std::uint64_t>(current.y, rect.y);
    const std::uint64_t right =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(current.x) + current.width,
                                static_cast<std::uint64_t>(rect.x) + rect.width);
    const std::uint64_t bottom =
        std::max<std::uint64_t>(static_cast<std::uint64_t>(current.y) + current.height,
                                static_cast<std::uint64_t>(rect.y) + rect.height);
    target = AtlasRect{static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(top),
                       static_cast<std::uint32_t>(right - left),
                       static_cast<std::uint32_t>(bottom - top)};
}

// Fuses two free rectangles that share a full edge into one, so a page whose
// glyphs have been erased does not stay carved into slivers that fit nothing.
[[nodiscard]] bool TryMerge(const AtlasRect& a, const AtlasRect& b, AtlasRect& out) noexcept {
    const std::uint64_t merged_width = static_cast<std::uint64_t>(a.width) + b.width;
    const std::uint64_t merged_height = static_cast<std::uint64_t>(a.height) + b.height;
    if (a.x == b.x && a.width == b.width) {
        if (static_cast<std::uint64_t>(a.y) + a.height == b.y) {
            out = AtlasRect{a.x, a.y, a.width, static_cast<std::uint32_t>(merged_height)};
            return true;
        }
        if (static_cast<std::uint64_t>(b.y) + b.height == a.y) {
            out = AtlasRect{a.x, b.y, a.width, static_cast<std::uint32_t>(merged_height)};
            return true;
        }
    }
    if (a.y == b.y && a.height == b.height) {
        if (static_cast<std::uint64_t>(a.x) + a.width == b.x) {
            out = AtlasRect{a.x, a.y, static_cast<std::uint32_t>(merged_width), a.height};
            return true;
        }
        if (static_cast<std::uint64_t>(b.x) + b.width == a.x) {
            out = AtlasRect{b.x, a.y, static_cast<std::uint32_t>(merged_width), a.height};
            return true;
        }
    }
    return false;
}

void Coalesce(std::vector<AtlasRect>& free_rects) {
    bool merged = true;
    while (merged && free_rects.size() > 1) {
        merged = false;
        for (std::size_t i = 0; i < free_rects.size() && !merged; ++i) {
            for (std::size_t j = i + 1; j < free_rects.size(); ++j) {
                AtlasRect merged_rect{};
                if (TryMerge(free_rects[i], free_rects[j], merged_rect)) {
                    free_rects[i] = merged_rect;
                    free_rects.erase(free_rects.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
    }
}

} // namespace

GlyphAtlas::GlyphAtlas(AtlasConfig config) : config_(config) {}

AtlasInsert GlyphAtlas::Insert(std::uint64_t key, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t padding = config_.padding;
    const std::uint64_t slot_width = static_cast<std::uint64_t>(width) + (padding * 2ULL);
    const std::uint64_t slot_height = static_cast<std::uint64_t>(height) + (padding * 2ULL);

    // TooLarge leaves the atlas untouched, so it is decided before the
    // re-insert path below can release an existing slot.
    if (slot_width > config_.page_width || slot_height > config_.page_height) {
        return AtlasInsert::TooLarge;
    }

    bool replaced = false;
    if (const auto existing = entries_.find(key); existing != entries_.end()) {
        const Entry entry = existing->second;
        if (static_cast<std::uint64_t>(entry.rect.width) >= slot_width &&
            static_cast<std::uint64_t>(entry.rect.height) >= slot_height) {
            // The resident rectangle still holds the request. Keep it, refresh
            // recency, and invalidate it because the caller is rewriting pixels.
            TouchInternal(key);
            UnionDirty(entry.page, entry.rect);
            return AtlasInsert::Replaced;
        }
        // It no longer fits, so the old slot is released and a new one taken.
        EraseInternal(key);
        replaced = true;
    }

    bool evicted = false;
    while (true) {
        if (auto slot = TakeBestFit(slot_width, slot_height)) {
            AddEntry(key, slot->page, slot->rect);
            UnionDirty(slot->page, slot->rect);
            if (replaced) {
                return AtlasInsert::Replaced;
            }
            return evicted ? AtlasInsert::Evicted : AtlasInsert::Inserted;
        }
        if (pages_.size() < config_.max_pages) {
            AllocateNewPage();
            continue;
        }
        if (!EvictOne(slot_width, slot_height)) {
            // Nothing left to evict and no page may be added. This is only
            // reachable with max_pages == 0, because a request that fits one
            // page fits an emptied page.
            return AtlasInsert::TooLarge;
        }
        evicted = true;
    }
}

std::optional<GlyphSlot> GlyphAtlas::Find(std::uint64_t key) const {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return GlyphSlot{found->second.page, found->second.rect};
}

void GlyphAtlas::Touch(std::uint64_t key) {
    TouchInternal(key);
}

bool GlyphAtlas::Erase(std::uint64_t key) {
    return EraseInternal(key);
}

std::optional<AtlasRect> GlyphAtlas::DirtyRect(std::uint32_t page) const {
    if (page >= pages_.size()) {
        return std::nullopt;
    }
    return pages_[page].dirty;
}

void GlyphAtlas::ClearDirty(std::uint32_t page) {
    if (page < pages_.size()) {
        pages_[page].dirty.reset();
    }
}

void GlyphAtlas::ClearAllDirty() {
    for (auto& page : pages_) {
        page.dirty.reset();
    }
}

std::vector<std::pair<std::uint64_t, GlyphSlot>> GlyphAtlas::Entries() const {
    std::vector<std::pair<std::uint64_t, GlyphSlot>> result;
    result.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        result.emplace_back(key, GlyphSlot{entry.page, entry.rect});
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) {
        if (a.second.page != b.second.page) {
            return a.second.page < b.second.page;
        }
        if (a.second.rect.y != b.second.rect.y) {
            return a.second.rect.y < b.second.rect.y;
        }
        if (a.second.rect.x != b.second.rect.x) {
            return a.second.rect.x < b.second.rect.x;
        }
        return a.first < b.first;
    });
    return result;
}

void GlyphAtlas::Reset() {
    pages_.clear();
    entries_.clear();
    lru_.clear();
    lru_index_.clear();
}

std::optional<std::size_t> GlyphAtlas::BestFreeIndex(std::uint32_t page, std::uint64_t width,
                                                     std::uint64_t height,
                                                     std::uint64_t& leftover) const {
    const auto& free_rects = pages_[page].free_rects;
    std::optional<std::size_t> best;
    std::uint64_t best_leftover = 0;
    for (std::size_t i = 0; i < free_rects.size(); ++i) {
        const AtlasRect& rect = free_rects[i];
        if (rect.width < width || rect.height < height) {
            continue;
        }
        const std::uint64_t candidate = RectArea(rect) - RectArea(width, height);
        const bool tighter =
            !best || candidate < best_leftover ||
            (candidate == best_leftover &&
             (rect.y < free_rects[*best].y ||
              (rect.y == free_rects[*best].y && rect.x < free_rects[*best].x)));
        if (tighter) {
            best = i;
            best_leftover = candidate;
        }
    }
    if (best) {
        leftover = best_leftover;
    }
    return best;
}

std::optional<GlyphSlot> GlyphAtlas::TakeBestFit(std::uint64_t width, std::uint64_t height) {
    std::optional<std::uint32_t> best_page;
    std::optional<std::size_t> best_index;
    std::uint64_t best_leftover = 0;
    for (std::uint32_t page = 0; page < pages_.size(); ++page) {
        std::uint64_t leftover = 0;
        const auto index = BestFreeIndex(page, width, height, leftover);
        if (!index) {
            continue;
        }
        if (!best_index || leftover < best_leftover) {
            best_page = page;
            best_index = index;
            best_leftover = leftover;
        }
    }
    if (!best_index) {
        return std::nullopt;
    }

    auto& free_rects = pages_[*best_page].free_rects;
    const AtlasRect source = free_rects[*best_index];
    free_rects.erase(free_rects.begin() + static_cast<std::ptrdiff_t>(*best_index));

    // A zero-area request still takes a texel out of the free list, so the
    // remainder strips below are never degenerate rectangles.
    const std::uint64_t layout_width = std::max<std::uint64_t>(width, 1);
    const std::uint64_t layout_height = std::max<std::uint64_t>(height, 1);
    const AtlasRect slot{source.x, source.y, static_cast<std::uint32_t>(width),
                         static_cast<std::uint32_t>(height)};

    // Guillotine split: the strip to the right is only as tall as the slot and
    // the strip below runs the full width of the free rectangle. They are
    // disjoint, and together with the slot they exactly cover the source.
    if (source.width > layout_width) {
        free_rects.push_back(
            AtlasRect{static_cast<std::uint32_t>(source.x + layout_width), source.y,
                      static_cast<std::uint32_t>(source.width - layout_width),
                      static_cast<std::uint32_t>(layout_height)});
    }
    if (source.height > layout_height) {
        free_rects.push_back(
            AtlasRect{source.x, static_cast<std::uint32_t>(source.y + layout_height), source.width,
                      static_cast<std::uint32_t>(source.height - layout_height)});
    }
    return GlyphSlot{*best_page, slot};
}

void GlyphAtlas::AllocateNewPage() {
    Page page;
    page.free_rects.push_back(AtlasRect{0, 0, config_.page_width, config_.page_height});
    // Nothing has ever been uploaded for this page, so all of it is new.
    page.dirty = AtlasRect{0, 0, config_.page_width, config_.page_height};
    pages_.push_back(std::move(page));
}

bool GlyphAtlas::EvictOne(std::uint64_t width, std::uint64_t height) {
    if (entries_.empty()) {
        return false;
    }

    // The candidate on each page is that page's least recently used glyph. A
    // candidate that already fits the request is preferred, and among those the
    // tightest fit wins, because spending a large rectangle on a small glyph
    // throws away the space a later large glyph needs.
    std::optional<std::uint64_t> best_key;
    std::uint64_t best_leftover = 0;
    std::unordered_set<std::uint32_t> seen_pages;
    for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
        const auto entry = entries_.find(*it);
        if (entry == entries_.end()) {
            continue;
        }
        // Iterating from the back, the first key seen for a page is that page's
        // least recently used glyph.
        if (!seen_pages.insert(entry->second.page).second) {
            continue;
        }
        if (entry->second.rect.width < width || entry->second.rect.height < height) {
            continue;
        }
        const std::uint64_t leftover = RectArea(entry->second.rect) - RectArea(width, height);
        if (!best_key || leftover < best_leftover ||
            (leftover == best_leftover && *it < *best_key)) {
            best_key = *it;
            best_leftover = leftover;
        }
    }

    // No single resident rectangle fits. Drop the globally least recently used
    // glyph and let the caller's loop retry: coalescing its space with whatever
    // free space touched it is the only way a request larger than every resident
    // rectangle can ever be satisfied.
    const std::uint64_t victim = best_key ? *best_key : lru_.back();
    const std::uint32_t page = entries_.at(victim).page;
    EraseInternal(victim);
    ++evictions_;
    // The page's contents changed under the caller, so invalidate all of it: a
    // caller that reacts to the Evicted result by re-uploading the page is then
    // correct even if it ignores DirtyRect().
    MarkPageDirty(page);
    return true;
}

void GlyphAtlas::AddEntry(std::uint64_t key, std::uint32_t page, const AtlasRect& rect) {
    entries_[key] = Entry{page, rect};
    // Defensive: a key must own exactly one recency node.
    if (const auto old = lru_index_.find(key); old != lru_index_.end()) {
        lru_.erase(old->second);
        lru_index_.erase(old);
    }
    lru_.push_front(key);
    lru_index_[key] = lru_.begin();
}

void GlyphAtlas::FreeRect(std::uint32_t page, const AtlasRect& rect) {
    pages_[page].free_rects.push_back(rect);
    Coalesce(pages_[page].free_rects);
}

void GlyphAtlas::UnionDirty(std::uint32_t page, const AtlasRect& rect) {
    UnionInto(pages_[page].dirty, rect);
}

void GlyphAtlas::MarkPageDirty(std::uint32_t page) {
    pages_[page].dirty = AtlasRect{0, 0, config_.page_width, config_.page_height};
}

void GlyphAtlas::TouchInternal(std::uint64_t key) {
    const auto found = lru_index_.find(key);
    if (found == lru_index_.end()) {
        return;
    }
    // Splice keeps every other iterator (including the one stored in the index)
    // valid, so the recency list can be reordered under the index for free.
    lru_.splice(lru_.begin(), lru_, found->second);
}

bool GlyphAtlas::EraseInternal(std::uint64_t key) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return false;
    }
    const Entry entry = found->second;
    entries_.erase(found);
    if (const auto lru = lru_index_.find(key); lru != lru_index_.end()) {
        lru_.erase(lru->second);
        lru_index_.erase(lru);
    }
    FreeRect(entry.page, entry.rect);
    // The slot now holds whatever the caller left there, so the region it covers
    // is no longer known-good for whoever samples the page.
    UnionDirty(entry.page, entry.rect);
    return true;
}

} // namespace VulkanEngine::Text
