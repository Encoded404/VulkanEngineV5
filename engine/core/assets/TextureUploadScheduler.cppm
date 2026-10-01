module;

export module VulkanEngine.TextureUploadScheduler;

import std;

export namespace VulkanEngine::Textures {

// One queued upload as the scheduler sees it: pure values, no device handle.
// `sequence` is the monotonic enqueue order used to keep FIFO ordering within a
// priority class; `id` is opaque to the scheduler and is echoed back so the
// caller can identify the item it should record.
struct PendingUpload {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint64_t id = 0;
    std::uint64_t bytes = 0;
    std::uint32_t priority = 0;
    std::uint64_t sequence = 0;
    bool cancelled = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct UploadLimits {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t max_items = 256;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// One item chosen for this frame, in record order.
struct UploadPlanItem {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint64_t id = 0;
    std::uint64_t bytes = 0;
    std::uint32_t priority = 0;
    // True when `bytes > budget_bytes`: the item is recorded alone because the
    // whole-item pacing target cannot be met without splitting it.
    bool exceeds_budget = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct UploadPlan {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<UploadPlanItem> order{};
    std::uint64_t bytes_planned = 0;
    std::uint32_t cancelled_dropped = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Pure, device-free upload pacing policy. It decides which pending items to
// record this frame and in what order; it never touches a device and never
// decides retire/reuse safety (that is the frame ring's job). The byte budget
// is a soft whole-item pacing target, not a capacity cap: an item larger than
// the budget is always selected, on its own, so no upload is ever split or
// starved by its own size.
class UploadScheduler {
public:
    [[nodiscard]] UploadPlan Plan(std::span<const PendingUpload> pending,
                                  std::uint64_t budget_bytes,
                                  std::uint32_t /*frame_index*/,
                                  const UploadLimits& limits = {}) const {
        UploadPlan plan{};
        const std::uint32_t max_items = limits.max_items == 0 ? 1U : limits.max_items;

        // Gather live items in priority order (descending), FIFO within a class.
        std::vector<std::uint32_t> selected{};
        selected.reserve(pending.size());
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pending.size()); ++i) {
            if (pending[i].cancelled) {
                ++plan.cancelled_dropped;
                continue;
            }
            selected.push_back(i);
        }
        std::ranges::stable_sort(selected, [&](std::uint32_t a, std::uint32_t b) {
            if (pending[a].priority != pending[b].priority) {
                return pending[a].priority > pending[b].priority;
            }
            return pending[a].sequence < pending[b].sequence;
        });

        for (const std::uint32_t index : selected) {
            if (plan.order.size() >= max_items) {
                break;
            }
            const auto& item = pending[index];
            const bool exceeds_budget = item.bytes > budget_bytes;
            const bool first_item = plan.order.empty();
            // A soft budget: stop before the item that would overrun, unless the
            // item is too large to ever fit (then take it alone) or nothing has
            // been planned yet (progress must be possible even when the first
            // eligible item is close to the budget).
            if (!first_item && !exceeds_budget &&
                plan.bytes_planned + item.bytes > budget_bytes) {
                break;
            }
            plan.order.push_back(UploadPlanItem{
                .id = item.id,
                .bytes = item.bytes,
                .priority = item.priority,
                .exceeds_budget = exceeds_budget,
            });
            plan.bytes_planned += item.bytes;
            if (exceeds_budget) {
                break;
            }
        }

        return plan;
    }
};

} // namespace VulkanEngine::Textures
