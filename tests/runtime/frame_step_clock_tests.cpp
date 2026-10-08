#include <gtest/gtest.h>

import std;

import Runtime.FrameStepClock;

namespace {

using Runtime::FrameStep;
using Runtime::FrameStepClock;

// 60 Hz, the usual fixed rate, and a generous clamp so only the case under test
// decides the outcome.
constexpr float kFixed60 = 1.0f / 60.0f;

FrameStepClock MakeEnabled(std::uint32_t max_steps = 8, float max_frame_delta = 0.25f) {
    FrameStepClock clock{};
    clock.Configure(kFixed60, max_steps, max_frame_delta);
    return clock;
}

} // namespace

// ── Disabled: one variable step per Advance ──────────────────────────────────

TEST(FrameStepClockTest, DisabledRunsExactlyOneVariableStep) {
    FrameStepClock clock{};
    clock.Configure(0.0f, 8, 0.25f);

    const FrameStep step = clock.Advance(0.016f);

    EXPECT_EQ(step.step_count, 1u);
    EXPECT_FLOAT_EQ(step.step_delta, step.delta_time);
    EXPECT_FLOAT_EQ(step.alpha, 1.0f);
    EXPECT_FALSE(step.budget_exhausted);
}

TEST(FrameStepClockTest, DisabledIsUnchangedAcrossRepeatedAdvances) {
    FrameStepClock clock{};
    clock.Configure(0.0f, 8, 0.25f);

    for (int i = 0; i < 5; ++i) {
        const FrameStep step = clock.Advance(0.01f);
        EXPECT_EQ(step.step_count, 1u);
        EXPECT_FLOAT_EQ(step.delta_time, 0.01f);
    }
}

// ── Clamping ────────────────────────────────────────────────────────────────

TEST(FrameStepClockTest, ClampsRawDeltaToMaxFrameDelta) {
    FrameStepClock clock{};
    clock.Configure(0.0f, 8, 0.25f);

    const FrameStep step = clock.Advance(5.0f);

    EXPECT_FLOAT_EQ(step.delta_time, 0.25f);
}

TEST(FrameStepClockTest, NegativeDeltaFloorsAtZero) {
    FrameStepClock clock{};
    clock.Configure(0.0f, 8, 0.25f);

    EXPECT_FLOAT_EQ(clock.Advance(-1.0f).delta_time, 0.0f);
}

TEST(FrameStepClockTest, NonPositiveMaxFrameDeltaDisablesTheClamp) {
    FrameStepClock clock{};
    clock.Configure(0.0f, 8, 0.0f);

    EXPECT_FLOAT_EQ(clock.Advance(5.0f).delta_time, 5.0f);
}

// ── Accumulation and catch-up ───────────────────────────────────────────────

TEST(FrameStepClockTest, SubStepDeltasAccumulateUntilAFullStepIsOwed) {
    FrameStepClock clock{};
    clock.Configure(0.5f, 8, 1.0f);

    EXPECT_EQ(clock.Advance(0.1f).step_count, 0u);
    EXPECT_EQ(clock.Advance(0.1f).step_count, 0u);
    EXPECT_EQ(clock.Advance(0.1f).step_count, 0u);
    EXPECT_EQ(clock.Advance(0.1f).step_count, 0u);
    EXPECT_EQ(clock.Advance(0.1f).step_count, 1u);
}

TEST(FrameStepClockTest, CatchUpRunsOneStepPerElapsedTimestep) {
    FrameStepClock clock = MakeEnabled();

    const FrameStep step = clock.Advance(3.0f * kFixed60);

    EXPECT_EQ(step.step_count, 3u);
    EXPECT_FLOAT_EQ(step.step_delta, kFixed60);
    EXPECT_FALSE(step.budget_exhausted);
}

TEST(FrameStepClockTest, AlphaStaysInUnitIntervalWhileEnabled) {
    FrameStepClock clock = MakeEnabled();

    for (int i = 0; i < 20; ++i) {
        const FrameStep step = clock.Advance(0.007f);
        EXPECT_GE(step.alpha, 0.0f);
        EXPECT_LT(step.alpha, 1.0f);
    }
}

// ── Spiral-of-death guard ───────────────────────────────────────────────────

TEST(FrameStepClockTest, StepBudgetCapsASingleHitch) {
    FrameStepClock clock = MakeEnabled(8, 1.0f);

    const FrameStep step = clock.Advance(1.0f);

    EXPECT_EQ(step.step_count, 8u);
    EXPECT_TRUE(step.budget_exhausted);
}

TEST(FrameStepClockTest, DroppedBacklogIsNotCarriedIntoTheNextAdvance) {
    FrameStepClock clock = MakeEnabled(8, 1.0f);

    (void)clock.Advance(1.0f);
    // If the backlog had been carried rather than dropped, this would owe dozens
    // of steps and the hitch would compound.
    const FrameStep step = clock.Advance(kFixed60);

    EXPECT_EQ(step.step_count, 1u);
    EXPECT_FALSE(step.budget_exhausted);
}

TEST(FrameStepClockTest, ZeroMaxStepsIsClampedToOne) {
    FrameStepClock clock = MakeEnabled(0, 1.0f);

    EXPECT_EQ(clock.MaxStepsPerIteration(), 1u);
    const FrameStep step = clock.Advance(1.0f);
    EXPECT_EQ(step.step_count, 1u);
    EXPECT_TRUE(step.budget_exhausted);
}

// ── Reset ───────────────────────────────────────────────────────────────────

TEST(FrameStepClockTest, ResetDropsTheAccumulatedFraction) {
    FrameStepClock clock{};
    clock.Configure(0.5f, 8, 1.0f);

    (void)clock.Advance(0.4f);
    ASSERT_GT(clock.Accumulator(), 0.0);

    clock.Reset();

    EXPECT_DOUBLE_EQ(clock.Accumulator(), 0.0);
    EXPECT_EQ(clock.Advance(0.1f).step_count, 0u);
}

// ── Configuration round-trip ────────────────────────────────────────────────

TEST(FrameStepClockTest, ConfigureRecordsItsInputs) {
    FrameStepClock clock{};
    clock.Configure(0.02f, 4, 0.3f);

    EXPECT_FLOAT_EQ(clock.FixedTimestep(), 0.02f);
    EXPECT_EQ(clock.MaxStepsPerIteration(), 4u);
    EXPECT_FLOAT_EQ(clock.MaxFrameDelta(), 0.3f);
}
