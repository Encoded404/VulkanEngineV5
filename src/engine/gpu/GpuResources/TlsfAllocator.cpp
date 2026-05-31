module;

#include <cstdint>

module VulkanEngine.GpuResources.TlsfAllocator;

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

    const uint32_t node_idx = AllocNode();
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

void TlsfAllocator::Mapping(uint64_t size, uint32_t& fl, uint32_t& sl) const {
    if (size < (1ULL << FL_INDEX_SHIFT)) {
        size = (1ULL << FL_INDEX_SHIFT);
    }

    fl = static_cast<uint32_t>(63ULL - static_cast<uint64_t>(__builtin_clzll(size)));

    if (fl > MAX_FL) {
        fl = MAX_FL;
        sl = SL_INDEX_COUNT - 1;
        return;
    }

    const uint64_t fl_min = 1ULL << fl;
    sl = static_cast<uint32_t>(((size - fl_min) * SL_INDEX_COUNT) >> fl);
}

uint32_t TlsfAllocator::ListIndex(uint32_t fl, uint32_t sl) const {
    return (fl - FL_INDEX_SHIFT) * SL_INDEX_COUNT + sl;
}

uint32_t TlsfAllocator::FindSuitableBlock(uint32_t fl, uint32_t sl) {
    // Search within the current first-level index
    const uint32_t sl_map = sl_bitmaps_[fl - FL_INDEX_SHIFT] >> sl;
    if (sl_map != 0) {
        const uint32_t matched_sl = sl + static_cast<uint32_t>(__builtin_ctz(sl_map));
        const uint32_t list = ListIndex(fl, matched_sl);
        if (heads_[list] != UINT32_MAX) {
            return heads_[list];
        }
    }

    // Search higher first-level indices (clamp to avoid UB shift by >= 32)
    if (fl < 31U) {
        const uint32_t fl_map = fl_bitmap_ >> (fl + 1);
        if (fl_map != 0) {
            fl = fl + 1 + static_cast<uint32_t>(__builtin_ctz(fl_map));
            const uint32_t sl_bitmap = sl_bitmaps_[fl - FL_INDEX_SHIFT];
            if (sl_bitmap != 0) {
                sl = static_cast<uint32_t>(__builtin_ctz(sl_bitmap));
                const uint32_t list = ListIndex(fl, sl);
                if (heads_[list] != UINT32_MAX) {
                    return heads_[list];
                }
            }
        }
    }

    return UINT32_MAX;
}

void TlsfAllocator::RemoveFromFreeLists(uint32_t node_index) {
    TlsfFreeNode& node = nodes_[node_index];

    uint32_t fl = 0;
    uint32_t sl = 0;
    Mapping(node.size, fl, sl);
    const uint32_t list_idx = ListIndex(fl, sl);

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

void TlsfAllocator::InsertIntoFreeLists(uint32_t node_index) {
    TlsfFreeNode& node = nodes_[node_index];

    uint32_t fl = 0;
    uint32_t sl = 0;
    Mapping(node.size, fl, sl);
    const uint32_t list_idx = ListIndex(fl, sl);

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

    uint32_t current = phys_head_;
    uint32_t prev = UINT32_MAX;
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
        const uint32_t index = static_cast<uint32_t>(node_pool_head_);
        node_pool_head_ = static_cast<int32_t>(static_cast<uint32_t>(nodes_[index].next_free));
        return index;
    }

    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back();
    return index;
}

void TlsfAllocator::FreeNode(uint32_t index) {
    TlsfFreeNode& node = nodes_[index];
    node.next_free = static_cast<uint32_t>(node_pool_head_);
    node.prev_free = UINT32_MAX;
    node_pool_head_ = static_cast<int32_t>(index);
}

uint64_t TlsfAllocator::Allocate(uint64_t size, uint64_t alignment) {
    if (size == 0 || total_size_ == 0 || alignment == 0) return UINT64_MAX;

    const uint64_t search_size = size + (alignment > 1 ? alignment - 1 : 0);

    uint32_t fl = 0;
    uint32_t sl = 0;
    Mapping(search_size, fl, sl);

    const uint32_t node_idx = FindSuitableBlock(fl, sl);
    if (node_idx == UINT32_MAX) return UINT64_MAX;

    const TlsfFreeNode& node = nodes_[node_idx];
    const uint64_t block_start = node.offset;
    const uint64_t block_end = node.offset + node.size;

    uint64_t aligned = block_start;
    const uint64_t mod = block_start % alignment;
    if (mod != 0) aligned += alignment - mod;

    if (aligned + size > block_end) {
        return UINT64_MAX;
    }

    const uint64_t padding_before = aligned - block_start;
    const uint64_t remainder = block_end - aligned - size;

    RemoveFromFreeLists(node_idx);
    free_size_ -= node.size;
    FreeNode(node_idx);

    if (padding_before >= (1ULL << FL_INDEX_SHIFT)) {
        const uint32_t pad_idx = AllocNode();
        if (pad_idx != UINT32_MAX) {
            nodes_[pad_idx] = {block_start, padding_before, 0, 0, 0, 0};
            InsertIntoFreeLists(pad_idx);
            free_size_ += padding_before;
        }
    }

    if (remainder >= (1ULL << FL_INDEX_SHIFT)) {
        const uint32_t rem_idx = AllocNode();
        if (rem_idx != UINT32_MAX) {
            nodes_[rem_idx] = {aligned + size, remainder, 0, 0, 0, 0};
            InsertIntoFreeLists(rem_idx);
            free_size_ += remainder;
        }
    }

    return aligned;
}

bool TlsfAllocator::Free(uint64_t offset, uint64_t size) {
    if (offset + size > total_size_ || size == 0) return false;

    uint64_t coalesced_offset = offset;
    uint64_t coalesced_size = size;

    uint32_t current = phys_head_;
    while (current != UINT32_MAX) {
        const TlsfFreeNode& n = nodes_[current];

        if (n.offset + n.size == coalesced_offset) {
            coalesced_offset = n.offset;
            coalesced_size += n.size;
            free_size_ -= n.size;
            RemoveFromFreeLists(current);
            const uint32_t to_free = current;
            current = nodes_[current].next_phys;
            FreeNode(to_free);
            continue;
        }

        if (coalesced_offset + coalesced_size == n.offset) {
            coalesced_size += n.size;
            free_size_ -= n.size;
            RemoveFromFreeLists(current);
            const uint32_t to_free = current;
            current = nodes_[current].next_phys;
            FreeNode(to_free);
            continue;
        }

        current = n.next_phys;
    }

    const uint32_t node_idx = AllocNode();
    if (node_idx == UINT32_MAX) return false;

    nodes_[node_idx] = {coalesced_offset, coalesced_size, 0, 0, 0, 0};
    free_size_ += coalesced_size;
    InsertIntoFreeLists(node_idx);

    return true;
}

} // namespace VulkanEngine::GpuResources
