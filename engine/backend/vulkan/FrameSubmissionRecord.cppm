module;

#include <cassert>

export module VulkanBackend.Vulkan.FrameSubmissionRecord;

import std;

export namespace VulkanBackend::Vulkan {

// Per-FIF submission record: entry k names the newest frame whose real
// queue runs were submitted into slot k % frames_in_flight. A frame whose runs
// were dropped (rendering_succeeded == false, an empty multi-run path) or that
// never reached a submit never advances the record, even though its slot's
// fence is still signaled by the empty consume-submit.
//
// `IsRecorded(frame)` answers the submission half of the corrected
// IsFrameComplete: frame N is recorded iff slot N % FIF has seen frame N or
// any later frame that landed in the same slot (N <= record for later frames
// with record % FIF == N % FIF). Device-free: the fence half stays with the
// backend, which polls its own fence alongside this record.
class FrameSubmissionRecord {
public:
    FrameSubmissionRecord() = default;

    void Initialize(std::uint32_t frames_in_flight) {
        frames_in_flight_ = std::clamp(frames_in_flight, 1U, kMaxFramesInFlight);
        record_.fill(kNeverSubmitted);
    }

    // Advances the slot's record. Only called when real runs were submitted.
    void MarkSubmitted(std::uint32_t frame_idx) {
        assert(frames_in_flight_ != 0U);
        record_[frame_idx % frames_in_flight_] = frame_idx;
    }

    // True when frame N has been submitted into its slot. The record entry for
    // slot N % FIF only names same-slot frames, so "record >= N" there means
    // frame N's submit happened and any later same-slot frames acquired only
    // after frame N's fence was signaled (or frame N was dropped and had no
    // GPU work at all) — both are free-safe for lifetime gating.
    [[nodiscard]] bool IsRecorded(std::uint32_t frame_idx) const {
        if (frames_in_flight_ == 0U) {
            return false;
        }
        const std::uint32_t recorded = record_[frame_idx % frames_in_flight_];
        return recorded != kNeverSubmitted && recorded >= frame_idx;
    }

    [[nodiscard]] std::uint32_t GetFramesInFlight() const noexcept { return frames_in_flight_; }

    // FIF the engine allows (RunApplication clamps 1..4); the record is sized
    // for the upper bound.
    static constexpr std::uint32_t kMaxFramesInFlight = 4;

private:
    // Sentinel: this slot has never held a real submit. max() can never be a
    // real frame that was recorded (MarkSubmitted asserts below max() only
    // implicitly; a frame index reaching the sentinel is beyond engine life).
    static constexpr std::uint32_t kNeverSubmitted = std::numeric_limits<std::uint32_t>::max();

    std::array<std::uint32_t, kMaxFramesInFlight> record_{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t frames_in_flight_ = 0;
};

} // namespace VulkanBackend::Vulkan