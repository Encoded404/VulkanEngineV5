module;

export module VulkanEngine.GpuResources.FrameRing;

import std;

export namespace VulkanEngine::GpuResources {

// Device-free deferred-destruction ring: one queue per frame in flight,
// drained at the start of the frame that reuses the ring slot. Ops carry the
// enqueueing frame. `gated` ops (Publish-class) additionally require the
// enqueueing frame to have been submitted: the gate callback is the backend's
// corrected IsFrameComplete. A gated op whose gate fails is DROPPED (not
// retried) and reported through on_drop — e.g. an upload whose recording frame
// was never submitted returns to Ready. Destruct-only ops (gated=false)
// always apply at the drain.
template <typename Op>
class FrameRing {
public:
    FrameRing() = default;

    void Initialize(std::uint32_t frames_in_flight) {
        frames_in_flight_ = std::max<std::uint32_t>(frames_in_flight, 1U);
        // resize (not assign-with-value): Op may be move-only, so the entries
        // must be default-constructed in place, never copied.
        rings_.clear();
        rings_.resize(frames_in_flight_);
    }

    [[nodiscard]] std::uint32_t GetFramesInFlight() const noexcept { return frames_in_flight_; }

    // Enqueues `op` into the ring of `recording_frame`. Applied at the start
    // of frame `recording_frame + frames_in_flight`.
    void Enqueue(Op op, std::uint32_t recording_frame, bool gated) {
        if (rings_.empty()) {
            rings_.resize(1);
            frames_in_flight_ = std::max<std::uint32_t>(frames_in_flight_, 1U);
        }
        rings_[recording_frame % frames_in_flight_].push_back(Entry{std::move(op), recording_frame, gated});
    }

    // Drains the ring that `frame_index` reuses. `apply` is called for every
    // surviving op; `gate(frame)` decides gated ops; dropped gated ops go to
    // `on_drop`. Called once per frame by the ring's owner.
    template <typename Apply, typename Gate, typename OnDrop>
    void BeginFrame(std::uint32_t frame_index, Apply&& apply, Gate&& gate, OnDrop&& on_drop) {
        if (rings_.empty()) {
            return;
        }
        // An applied op may enqueue a follow-up op into this same ring (the
        // bindless Decommit enqueues the destroy of the binding it retired).
        // Swap the ring out before iterating it so that re-entrant Enqueue
        // cannot reallocate the container under the loop; the follow-up op lands
        // in the now-empty ring and applies at the next drain of this slot,
        // which is the "one ring cycle later" the callers document.
        std::vector<Entry> draining;
        draining.swap(rings_[frame_index % frames_in_flight_]);
        for (auto& entry : draining) {
            if (entry.gated && !gate(entry.frame)) {
                on_drop(entry.op, entry.frame);
            } else {
                apply(entry.op, entry.frame);
            }
        }
    }

    // Drain without any gating (shutdown path: the device is idle).
    template <typename Apply>
    void Flush(Apply&& apply) {
        for (auto& ring : rings_) {
            std::vector<Entry> draining;
            draining.swap(ring);
            for (auto& entry : draining) {
                apply(entry.op, entry.frame);
            }
        }
        rings_.clear();
    }

    [[nodiscard]] std::size_t PendingCount() const noexcept {
        std::size_t total = 0;
        for (const auto& ring : rings_) {
            total += ring.size();
        }
        return total;
    }

private:
    struct Entry {
        Op op{};
        std::uint32_t frame{0};
        bool gated{false};
    };

    std::uint32_t frames_in_flight_{1};
    std::vector<std::vector<Entry>> rings_{};
};

} // namespace VulkanEngine::GpuResources
