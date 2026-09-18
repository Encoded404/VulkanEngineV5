module;

module VulkanEngine.GpuResources.TlsfAllocator;

import std;
import std.compat;

namespace VulkanEngine::GpuResources {

TlsfAllocator::~TlsfAllocator() = default;

bool TlsfAllocator::Initialize(uint64_t total_size) {
    total_size_ = total_size;
    free_size_ = total_size;

    nodes_.clear();
    node_pool_head_ = -1;

    heads_.fill(UINT32_MAX);

    fl_bitmap_ = 0;
    sl_bitmaps_.fill(0);

    phys_head_ = UINT32_MAX;

    live_extents_.clear();
    debug_overlap_ = false;
    debug_double_free_ = false;

    const std::uint32_t node_idx = AllocNode();
    if (node_idx == UINT32_MAX) return false;

    TlsfFreeNode& root = nodes_[node_idx];
    root.offset = 0;
    root.size = total_size;
    root.prev_phys = UINT32_MAX;
    root.next_phys = UINT32_MAX;
    root.prev_free = UINT32_MAX;
    root.next_free = UINT32_MAX;

    InsertIntoFreeLists(node_idx);

    return true;
}

void TlsfAllocator::Mapping(uint64_t size, std::uint32_t& fl, std::uint32_t& sl) const {
    if (size < (1ULL << FL_INDEX_SHIFT)) {
        size = (1ULL << FL_INDEX_SHIFT);
    }

    fl = static_cast<std::uint32_t>(63ULL - static_cast<uint64_t>(__builtin_clzll(size)));

    if (fl > MAX_FL) {
        fl = MAX_FL;
        sl = SL_INDEX_COUNT - 1;
        return;
    }

    const std::uint64_t fl_min = 1ULL << fl;
    sl = static_cast<std::uint32_t>(((size - fl_min) * SL_INDEX_COUNT) >> fl);
}

uint32_t TlsfAllocator::ListIndex(std::uint32_t fl, std::uint32_t sl) const {
    return (fl - FL_INDEX_SHIFT) * SL_INDEX_COUNT + sl;
}

uint32_t TlsfAllocator::FindSuitableBlock(std::uint32_t fl, std::uint32_t sl) {
    // Search within the current first-level index
    const std::uint32_t sl_map = sl_bitmaps_[fl - FL_INDEX_SHIFT] >> sl;
    if (sl_map != 0) {
        const std::uint32_t matched_sl = sl + static_cast<std::uint32_t>(__builtin_ctz(sl_map));
        const std::uint32_t list = ListIndex(fl, matched_sl);
        if (heads_[list] != UINT32_MAX) {
            return heads_[list];
        }
    }

    // Search higher first-level indices (clamp to avoid UB shift by >= 32)
    if (fl < 31U) {
        const std::uint32_t fl_map = fl_bitmap_ >> (fl + 1);
        if (fl_map != 0) {
            fl = fl + 1 + static_cast<std::uint32_t>(__builtin_ctz(fl_map));
            const std::uint32_t sl_bitmap = sl_bitmaps_[fl - FL_INDEX_SHIFT];
            if (sl_bitmap != 0) {
                sl = static_cast<std::uint32_t>(__builtin_ctz(sl_bitmap));
                const std::uint32_t list = ListIndex(fl, sl);
                if (heads_[list] != UINT32_MAX) {
                    return heads_[list];
                }
            }
        }
    }

    return UINT32_MAX;
}

void TlsfAllocator::RemoveFromFreeLists(std::uint32_t node_index) {
    TlsfFreeNode& node = nodes_[node_index];

    std::uint32_t fl = 0;
    std::uint32_t sl = 0;
    Mapping(node.size, fl, sl);
    const std::uint32_t list_idx = ListIndex(fl, sl);

    if (node.prev_free != UINT32_MAX) {
        nodes_[node.prev_free].next_free = node.next_free;
    } else {
        heads_[list_idx] = node.next_free;
    }
    if (node.next_free != UINT32_MAX) {
        nodes_[node.next_free].prev_free = node.prev_free;
    }

    if (heads_[list_idx] == UINT32_MAX) {
        sl_bitmaps_[fl - FL_INDEX_SHIFT] &= ~(1U << sl);
        if (sl_bitmaps_[fl - FL_INDEX_SHIFT] == 0) {
            fl_bitmap_ &= ~(1U << fl);
        }
    }

    if (node.prev_phys != UINT32_MAX) {
        nodes_[node.prev_phys].next_phys = node.next_phys;
    } else {
        phys_head_ = node.next_phys;
    }
    if (node.next_phys != UINT32_MAX) {
        nodes_[node.next_phys].prev_phys = node.prev_phys;
    }

    node.prev_free = UINT32_MAX;
    node.next_free = UINT32_MAX;
    node.prev_phys = UINT32_MAX;
    node.next_phys = UINT32_MAX;
}

void TlsfAllocator::InsertIntoFreeLists(std::uint32_t node_index) {
    TlsfFreeNode& node = nodes_[node_index];

    std::uint32_t fl = 0;
    std::uint32_t sl = 0;
    Mapping(node.size, fl, sl);
    const std::uint32_t list_idx = ListIndex(fl, sl);

    node.prev_free = UINT32_MAX;
    node.next_free = heads_[list_idx];
    if (heads_[list_idx] != UINT32_MAX) {
        nodes_[heads_[list_idx]].prev_free = node_index;
    }
    heads_[list_idx] = node_index;

    sl_bitmaps_[fl - FL_INDEX_SHIFT] |= (1U << sl);
    fl_bitmap_ |= (1U << fl);

    node.prev_phys = UINT32_MAX;
    node.next_phys = UINT32_MAX;

    if (phys_head_ == UINT32_MAX) {
        phys_head_ = node_index;
        return;
    }

    std::uint32_t current = phys_head_;
    std::uint32_t prev = UINT32_MAX;
    while (current != UINT32_MAX && nodes_[current].offset < node.offset) {
        prev = current;
        current = nodes_[current].next_phys;
    }

    node.prev_phys = prev;
    node.next_phys = current;

    if (prev != UINT32_MAX) {
        nodes_[prev].next_phys = node_index;
    } else {
        phys_head_ = node_index;
    }
    if (current != UINT32_MAX) {
        nodes_[current].prev_phys = node_index;
    }
}

uint32_t TlsfAllocator::AllocNode() {
    if (node_pool_head_ >= 0) {
        const std::uint32_t index = static_cast<std::uint32_t>(node_pool_head_);
        node_pool_head_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(nodes_[index].next_free));
        return index;
    }

    const std::uint32_t index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.emplace_back();
    return index;
}

void TlsfAllocator::FreeNode(std::uint32_t index) {
    TlsfFreeNode& node = nodes_[index];
    node.next_free = static_cast<std::uint32_t>(node_pool_head_);
    node.prev_free = UINT32_MAX;
    node_pool_head_ = static_cast<std::int32_t>(index);
}

uint64_t TlsfAllocator::Allocate(uint64_t size, std::uint64_t alignment) {
    if (size == 0 || total_size_ == 0 || alignment == 0) return UINT64_MAX;

    // Does the free node at `index` have room for `size` once aligned, and if so
    // at what absolute offset?
    const auto fits = [&](std::uint32_t index, std::uint64_t& aligned_out) {
        const TlsfFreeNode& candidate = nodes_[index];
        std::uint64_t candidate_aligned = candidate.offset;
        const std::uint64_t mod = candidate_aligned % alignment;
        if (mod != 0) candidate_aligned += alignment - mod;
        if (candidate_aligned + size > candidate.offset + candidate.size) {
            return false;
        }
        aligned_out = candidate_aligned;
        return true;
    };

    std::uint32_t fl = 0;
    std::uint32_t sl = 0;
    Mapping(size + (alignment > 1 ? alignment - 1 : 0), fl, sl);

    std::uint32_t node_idx = FindSuitableBlock(fl, sl);
    std::uint64_t aligned = 0;

    // The bitmap search uses the worst-case alignment slack, so it can land on a
    // block that turns out not to fit once alignment is applied. Scan every free
    // block (best fit) instead of failing the allocation.
    if (node_idx == UINT32_MAX || !fits(node_idx, aligned)) {
        node_idx = UINT32_MAX;
        std::uint64_t best_size = UINT64_MAX;
        for (std::uint32_t index = phys_head_; index != UINT32_MAX; index = nodes_[index].next_phys) {
            std::uint64_t candidate_aligned = 0;
            if (!fits(index, candidate_aligned)) {
                continue;
            }
            if (nodes_[index].size < best_size) {
                best_size = nodes_[index].size;
                aligned = candidate_aligned;
                node_idx = index;
            }
        }
    }

    if (node_idx == UINT32_MAX) return UINT64_MAX;

    // Defensive: never hand out a range that overlaps a live allocation.
    for (const auto& [live_offset, live_size] : live_extents_) {
        if (aligned < live_offset + live_size && live_offset < aligned + size) {
            debug_overlap_ = true;
            return UINT64_MAX;
        }
    }

    const TlsfFreeNode node = nodes_[node_idx];
    const std::uint64_t block_start = node.offset;
    const std::uint64_t block_end = node.offset + node.size;

    const std::uint64_t padding_before = aligned - block_start;
    const std::uint64_t remainder = block_end - aligned - size;

    RemoveFromFreeLists(node_idx);
    free_size_ -= node.size;
    FreeNode(node_idx);

    if (padding_before >= (1ULL << FL_INDEX_SHIFT)) {
        const std::uint32_t pad_idx = AllocNode();
        if (pad_idx != UINT32_MAX) {
            nodes_[pad_idx] = {block_start, padding_before, 0, 0, 0, 0};
            InsertIntoFreeLists(pad_idx);
            free_size_ += padding_before;
        }
    }

    if (remainder >= (1ULL << FL_INDEX_SHIFT)) {
        const std::uint32_t rem_idx = AllocNode();
        if (rem_idx != UINT32_MAX) {
            nodes_[rem_idx] = {aligned + size, remainder, 0, 0, 0, 0};
            InsertIntoFreeLists(rem_idx);
            free_size_ += remainder;
        }
    }

    live_extents_[aligned] = size;
    return aligned;
}

void TlsfAllocator::Reset() {
    nodes_.clear();
    node_pool_head_ = -1;
    heads_.fill(UINT32_MAX);
    fl_bitmap_ = 0;
    sl_bitmaps_.fill(0);
    phys_head_ = UINT32_MAX;
    free_size_ = total_size_;
    live_extents_.clear();
    debug_overlap_ = false;
    debug_double_free_ = false;

    const std::uint32_t node_idx = AllocNode();
    if (node_idx != UINT32_MAX) {
        TlsfFreeNode& root = nodes_[node_idx];
        root.offset = 0;
        root.size = total_size_;
        root.prev_phys = UINT32_MAX;
        root.next_phys = UINT32_MAX;
        root.prev_free = UINT32_MAX;
        root.next_free = UINT32_MAX;
        InsertIntoFreeLists(node_idx);
    }
}

bool TlsfAllocator::Free(uint64_t offset, std::uint64_t size) {
    if (offset + size > total_size_ || size == 0) return false;

    const auto live_it = live_extents_.find(offset);
    if (live_it == live_extents_.end() || live_it->second != size) {
        // Not a live extent: double free, or a free of something never allocated.
        debug_double_free_ = true;
        return false;
    }
    live_extents_.erase(live_it);

    // Locate both physical neighbours before unlinking anything; RemoveFromFreeLists
    // clears next_phys, so reading the walk pointer afterwards would lose the rest.
    std::uint32_t prev_node = UINT32_MAX;
    std::uint32_t next_node = UINT32_MAX;
    for (std::uint32_t current = phys_head_; current != UINT32_MAX; current = nodes_[current].next_phys) {
        if (nodes_[current].offset + nodes_[current].size == offset) {
            prev_node = current;
        }
        if (offset + size == nodes_[current].offset) {
            next_node = current;
        }
    }

    std::uint64_t coalesced_offset = offset;
    std::uint64_t coalesced_size = size;
    if (prev_node != UINT32_MAX) {
        coalesced_offset = nodes_[prev_node].offset;
        coalesced_size += nodes_[prev_node].size;
    }
    if (next_node != UINT32_MAX) {
        coalesced_size += nodes_[next_node].size;
    }

    if (prev_node != UINT32_MAX) {
        free_size_ -= nodes_[prev_node].size;
        RemoveFromFreeLists(prev_node);
        FreeNode(prev_node);
    }
    if (next_node != UINT32_MAX) {
        free_size_ -= nodes_[next_node].size;
        RemoveFromFreeLists(next_node);
        FreeNode(next_node);
    }

    const std::uint32_t node_idx = AllocNode();
    if (node_idx == UINT32_MAX) return false;

    nodes_[node_idx] = {coalesced_offset, coalesced_size, 0, 0, 0, 0};
    free_size_ += coalesced_size;
    InsertIntoFreeLists(node_idx);

    return true;
}

} // namespace VulkanEngine::GpuResources
