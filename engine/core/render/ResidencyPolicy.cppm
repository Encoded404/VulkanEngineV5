module;

export module VulkanEngine.ResidencyPolicy;

import std;

export namespace VulkanEngine::Residency {

// A texture the residency set knows about. `id` is an opaque key (the callers
// use the resource id's hash or an index); `bytes` is the device bytes it
// occupies; `last_use_frame` orders eviction (oldest first); `fallback_bound`
// marks a reservation whose real binding has not published yet, which cannot
// be "evicted" and must be skipped; `pinned` marks resources that must never be
// evicted (the fallback, camera targets, anything the app owns).
struct ResidentTexture {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint64_t id{0};
    std::uint64_t bytes{0};
    std::uint64_t last_use_frame{0};
    bool fallback_bound{false};
    bool pinned{false};
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct EvictionPlan {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<std::uint64_t> evict{};   // ids to evict, least-recently-used first
    std::uint64_t evicted_bytes{0};
    std::uint64_t resident_bytes{0};      // resident bytes after the plan applies
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Pure LRU eviction: choose whole textures to drop until the resident set fits
// `budget_bytes`. `used_bytes` is the set's current committed byte total.
// Candidates are ordered by ascending last-use frame (a stable tie-break by id
// keeps the plan deterministic); a candidate younger than
// `min_residency_frames` is never chosen, so a texture that was just uploaded
// is not evicted before a frame has had the chance to sample it. A
// `fallback_bound` or `pinned` texture is never evicted. `budget_bytes == 0`
// disables eviction entirely.
[[nodiscard]] inline EvictionPlan PlanEviction(std::span<const ResidentTexture> textures,
                                               std::uint64_t used_bytes,
                                               std::uint64_t budget_bytes,
                                               std::uint64_t current_frame,
                                               std::uint64_t min_residency_frames = 0) {
    EvictionPlan plan{};
    plan.resident_bytes = used_bytes;
    if (budget_bytes == 0 || used_bytes <= budget_bytes) {
        return plan;
    }

    std::vector<const ResidentTexture*> candidates;
    candidates.reserve(textures.size());
    for (const auto& texture : textures) {
        if (texture.pinned || texture.fallback_bound || texture.bytes == 0) {
            continue;
        }
        if (current_frame < texture.last_use_frame ||
            current_frame - texture.last_use_frame < min_residency_frames) {
            continue;
        }
        candidates.push_back(&texture);
    }
    std::ranges::sort(candidates, [](const ResidentTexture* a, const ResidentTexture* b) {
        if (a->last_use_frame != b->last_use_frame) {
            return a->last_use_frame < b->last_use_frame;
        }
        return a->id < b->id;
    });

    for (const ResidentTexture* texture : candidates) {
        if (plan.resident_bytes <= budget_bytes) {
            break;
        }
        plan.evict.push_back(texture->id);
        plan.resident_bytes -= texture->bytes;
        plan.evicted_bytes += texture->bytes;
    }
    return plan;
}

// Device-free bookkeeping half of the residency registry: tracks which
// resources are resident and their byte/age state. The device-touching half
// (Textures::TextureResidency) wraps this and applies plans through the
// bindless manager. Kept separate so the accounting and eviction ordering are
// unit-testable without a device.
class ResidencySet {
public:
    struct Entry {
        ResidentTexture texture{};
    };

    // Inserts or replaces the entry for `id`. A reservation starts
    // `fallback_bound`; `MarkResident` clears that flag and sets the byte size.
    void Track(std::uint64_t id, std::uint64_t bytes, std::uint64_t frame,
               bool pinned = false) {
        Entry& entry = entries_[id];
        entry.texture.id = id;
        entry.texture.bytes = bytes;
        entry.texture.last_use_frame = frame;
        entry.texture.fallback_bound = true;
        entry.texture.pinned = pinned;
    }

    // The real binding published: the entry is now a real resident texture.
    void MarkResident(std::uint64_t id, std::uint64_t bytes, std::uint64_t frame) {
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            return;
        }
        it->second.texture.bytes = bytes;
        it->second.texture.last_use_frame = frame;
        it->second.texture.fallback_bound = false;
    }

    void MarkEvicted(std::uint64_t id) { entries_.erase(id); }

    void Touch(std::uint64_t id, std::uint64_t frame) {
        auto it = entries_.find(id);
        if (it != entries_.end()) {
            it->second.texture.last_use_frame = frame;
        }
    }

    void SetPinned(std::uint64_t id) {
        auto it = entries_.find(id);
        if (it != entries_.end()) {
            it->second.texture.pinned = true;
        }
    }

    [[nodiscard]] bool Contains(std::uint64_t id) const { return entries_.contains(id); }
    [[nodiscard]] std::size_t Size() const { return entries_.size(); }

    [[nodiscard]] const ResidentTexture* Get(std::uint64_t id) const {
        const auto it = entries_.find(id);
        return it != entries_.end() ? &it->second.texture : nullptr;
    }

    [[nodiscard]] const std::unordered_map<std::uint64_t, Entry>& Entries() const {
        return entries_;
    }

    [[nodiscard]] std::uint64_t ResidentBytes() const {
        std::uint64_t total = 0;
        for (const auto& [id, entry] : entries_) {
            (void)id;
            if (!entry.texture.fallback_bound) {
                total += entry.texture.bytes;
            }
        }
        return total;
    }

    // Runs the pure plan over the current set. Does not mutate: the caller
    // applies the plan (release slots, MarkEvicted) so the device half owns the
    // side effects.
    [[nodiscard]] EvictionPlan Plan(std::uint64_t budget_bytes, std::uint64_t current_frame,
                                    std::uint64_t min_residency_frames = 0) const {
        std::vector<ResidentTexture> snapshot;
        snapshot.reserve(entries_.size());
        for (const auto& [id, entry] : entries_) {
            (void)id;
            snapshot.push_back(entry.texture);
        }
        return PlanEviction(snapshot, ResidentBytes(), budget_bytes, current_frame,
                            min_residency_frames);
    }

private:
    std::unordered_map<std::uint64_t, Entry> entries_;
};

} // namespace VulkanEngine::Residency
