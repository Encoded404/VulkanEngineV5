module;

#include <cstdint>
#include <array>
#include <vector>

export module VulkanEngine.GpuResources.TlsfAllocator;

export namespace VulkanEngine::GpuResources {

class TlsfAllocator {
public:
    TlsfAllocator() = default;
    ~TlsfAllocator();

    TlsfAllocator(const TlsfAllocator&) = delete;
    TlsfAllocator& operator=(const TlsfAllocator&) = delete;
    TlsfAllocator(TlsfAllocator&&) = default;
    TlsfAllocator& operator=(TlsfAllocator&&) = default;

    bool Initialize(uint64_t total_size);

    // Returns UINT64_MAX on failure
    uint64_t Allocate(uint64_t size, uint64_t alignment);

    // Frees a previously allocated region. Returns false if corrupt.
    bool Free(uint64_t offset, uint64_t size);

    [[nodiscard]] uint64_t GetTotalSize() const { return total_size_; }
    [[nodiscard]] uint64_t GetFreeSize() const { return free_size_; }
    [[nodiscard]] uint64_t GetUsedSize() const { return total_size_ - free_size_; }

private:
    static constexpr uint64_t FL_INDEX_SHIFT = 4ULL;
    static constexpr uint64_t SL_INDEX_COUNT = 4ULL;
    static constexpr uint64_t MAX_FL = 31ULL;
    static constexpr uint64_t FL_INDEX_COUNT = MAX_FL - FL_INDEX_SHIFT + 1ULL;
    static constexpr uint64_t NUM_LISTS = FL_INDEX_COUNT * SL_INDEX_COUNT;

    struct TlsfFreeNode {
        uint64_t offset;
        uint64_t size;
        uint32_t prev_phys;
        uint32_t next_phys;
        uint32_t prev_free;
        uint32_t next_free;
    };

    void Mapping(uint64_t size, uint32_t& fl, uint32_t& sl) const;
    [[nodiscard]] uint32_t ListIndex(uint32_t fl, uint32_t sl) const;

    uint32_t FindSuitableBlock(uint32_t fl, uint32_t sl);
    void RemoveFromFreeLists(uint32_t node_index);
    void InsertIntoFreeLists(uint32_t node_index);

    uint32_t AllocNode();
    void FreeNode(uint32_t index);

    uint64_t total_size_ = 0;
    uint64_t free_size_ = 0;

    std::vector<TlsfFreeNode> nodes_;
    int32_t node_pool_head_ = -1;

    std::array<uint32_t, NUM_LISTS> heads_{};

    uint32_t fl_bitmap_ = 0;
    std::array<uint32_t, FL_INDEX_COUNT> sl_bitmaps_{};

    uint32_t phys_head_ = UINT32_MAX;
};

} // namespace VulkanEngine::GpuResources
