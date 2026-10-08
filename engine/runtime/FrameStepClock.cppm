module;

export module Runtime.FrameStepClock;

import std;

export namespace Runtime {

// One iteration's worth of frame-clock decisions.
//
// delta_time is the clamped wall-clock delta for the iteration; it is what
// `frame.delta_time` is set from and what presentation-rate work should scale
// with. step_delta is the constant timestep handed to each fixed step, and
// step_count is how many of them this iteration owes.
struct FrameStep {
    float delta_time = 0.0f;         // clamped wall-clock delta
    float step_delta = 0.0f;         // timestep for each fixed step
    std::uint32_t step_count = 0;    // 1 when disabled; 0..max_steps when enabled
    float alpha = 1.0f;              // accumulator / fixed_timestep, in [0, 1)
    bool budget_exhausted = false;   // hit the step cap; the backlog was dropped
};

// Pure frame-clock: turns a measured wall-clock delta into a number of fixed
// simulation steps. Holds no clock of its own, so the caller decides when an
// iteration happens and this stays trivially testable without a device.
//
// Disabled (fixed_timestep <= 0) is not "zero steps": every Advance returns
// exactly one step carrying the clamped wall-clock delta. That keeps a consumer
// that routes its simulation through the fixed hook working unchanged when the
// constant rate is off, so the fixed rate can be enabled later without touching
// gameplay code.
class FrameStepClock {
public:
    // fixed_timestep: seconds per fixed step; <= 0 disables fixed stepping.
    // max_steps_per_iteration: upper bound on steps per Advance; clamped to >= 1.
    // max_frame_delta: ceiling on the wall-clock delta (bounds the cost of a
    //   minimize, resize, breakpoint or shader compile); <= 0 disables the clamp.
    void Configure(float fixed_timestep, std::uint32_t max_steps_per_iteration, float max_frame_delta) {
        fixed_timestep_ = fixed_timestep;
        max_steps_ = max_steps_per_iteration < 1 ? 1u : max_steps_per_iteration;
        max_frame_delta_ = max_frame_delta;
        accumulator_ = 0.0;
    }

    [[nodiscard]] FrameStep Advance(float raw_delta) {
        FrameStep out{};
        // A non-monotonic clock can hand back a negative delta; floor at zero so
        // it can never run the accumulator backwards.
        out.delta_time = max_frame_delta_ > 0.0f
                             ? std::clamp(raw_delta, 0.0f, max_frame_delta_)
                             : std::max(raw_delta, 0.0f);

        if (fixed_timestep_ <= 0.0f) {
            out.step_delta = out.delta_time;
            out.step_count = 1;
            out.alpha = 1.0f;
            return out;
        }

        accumulator_ += static_cast<double>(out.delta_time);
        const double step = static_cast<double>(fixed_timestep_);

        std::uint32_t count = 0;
        while (accumulator_ >= step && count < max_steps_) {
            accumulator_ -= step;
            ++count;
        }

        if (accumulator_ >= step) {
            // The step cap is the spiral-of-death guard: drop the backlog instead
            // of carrying it, so a hitch cannot compound into ever more steps.
            accumulator_ = std::fmod(accumulator_, step);
            out.budget_exhausted = true;
        }

        out.step_delta = fixed_timestep_;
        out.step_count = count;
        out.alpha = static_cast<float>(accumulator_ / step);
        return out;
    }

    void Reset() { accumulator_ = 0.0; }

    [[nodiscard]] float FixedTimestep() const { return fixed_timestep_; }
    [[nodiscard]] std::uint32_t MaxStepsPerIteration() const { return max_steps_; }
    [[nodiscard]] float MaxFrameDelta() const { return max_frame_delta_; }
    [[nodiscard]] double Accumulator() const { return accumulator_; }

private:
    float fixed_timestep_ = 0.0f;
    float max_frame_delta_ = 0.25f;
    std::uint32_t max_steps_ = 8;
    double accumulator_ = 0.0;
};

} // namespace Runtime
