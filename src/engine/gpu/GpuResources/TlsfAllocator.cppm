module;

export module VulkanEngine.GpuResources.TlsfAllocator;

import std;

constexpr std::uint32_t UINT32_MAX =
    std::numeric_limits<std::uint32_t>::max();

export namespace VulkanEngine::GpuResources {

class TlsfAllocator {
public:
    TlsfAllocator() = default;
    ~TlsfAllocator();

    TlsfAllocator(const TlsfAllocator&) = delete;
    TlsfAllocator& operator=(const TlsfAllocator&) = delete;
    TlsfAllocator(TlsfAllocator&&) = default;
    TlsfAllocator& operator=(TlsfAllocator&&) = default;

    bool Initialize(std::uint64_t total_size);

    // Returns UINT64_MAX on failure
    std::uint64_t Allocate(std::uint64_t size, std::uint64_t alignment);

    // Frees a previously allocated region. Returns false if corrupt.
    bool Free(std::uint64_t offset, std::uint64_t size);

    void Reset();

    [[nodiscard]] std::uint64_t GetTotalSize() const { return total_size_; }
    [[nodiscard]] std::uint64_t GetFreeSize() const { return free_size_; }
    [[nodiscard]] std::uint64_t GetUsedSize() const { return total_size_ - free_size_; }

private:
    static constexpr std::uint64_t FL_INDEX_SHIFT = 4ULL;
    static constexpr std::uint64_t SL_INDEX_COUNT = 4ULL;
    static constexpr std::uint64_t MAX_FL = 31ULL;
    static constexpr std::uint64_t FL_INDEX_COUNT = MAX_FL - FL_INDEX_SHIFT + 1ULL;
    static constexpr std::uint64_t NUM_LISTS = FL_INDEX_COUNT * SL_INDEX_COUNT;

    struct TlsfFreeNode {
        std::uint64_t offset;
        std::uint64_t size;
        std::uint32_t prev_phys;
        std::uint32_t next_phys;
        std::uint32_t prev_free;
        std::uint32_t next_free;
    };

    void Mapping(std::uint64_t size, std::uint32_t& fl, std::uint32_t& sl) const;
    [[nodiscard]] std::uint32_t ListIndex(std::uint32_t fl, std::uint32_t sl) const;

    std::uint32_t FindSuitableBlock(std::uint32_t fl, std::uint32_t sl);
    void RemoveFromFreeLists(std::uint32_t node_index);
    void InsertIntoFreeLists(std::uint32_t node_index);

    std::uint32_t AllocNode();
    void FreeNode(std::uint32_t index);

    std::uint64_t total_size_ = 0;
    std::uint64_t free_size_ = 0;

    std::vector<TlsfFreeNode> nodes_;
    std::int32_t node_pool_head_ = -1;

    std::array<std::uint32_t, NUM_LISTS> heads_{};

    std::uint32_t fl_bitmap_ = 0;
    std::array<std::uint32_t, FL_INDEX_COUNT> sl_bitmaps_{};

    std::uint32_t phys_head_ = UINT32_MAX;
};

} // namespace VulkanEngine::GpuResources
