#include <gtest/gtest.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>

import std;
import std.compat;

import VulkanBackend.Event;
import VulkanEngine.Input;

namespace {

using namespace VulkanBackend::Event;
using namespace VulkanEngine::Input;

TEST(InputSystemTest, TracksKeyboardActionsAcrossFrames) {
    InputSystem input_system;
    const ActionHandle jump_handle = input_system.BindAction("jump", InputBinding::Key(SDLK_SPACE));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_SPACE), false});
    input_system.Update();

    EXPECT_TRUE(input_system.IsActionActive(jump_handle));
    EXPECT_TRUE(input_system.WasActionStarted(jump_handle));
    EXPECT_FALSE(input_system.WasActionCanceled(jump_handle));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyUpEvent{static_cast<int32_t>(SDLK_SPACE)});
    input_system.Update();

    EXPECT_FALSE(input_system.IsActionActive(jump_handle));
    EXPECT_FALSE(input_system.WasActionStarted(jump_handle));
    EXPECT_TRUE(input_system.WasActionCanceled(jump_handle));
}

TEST(InputSystemTest, ProcessesBatchedMouseInputEvents) {
    InputSystem input_system;
    const ActionHandle fire_handle = input_system.BindAction("fire", InputBinding::MouseButton(1));

    EventList events{};
    events.push_back(std::make_unique<MouseButtonDownEvent>(1, 100, 200));
    events.push_back(std::make_unique<MouseMotionEvent>(100.0f, 200.0f, 4.5f, -2.0f));
    events.push_back(std::make_unique<MouseWheelEvent>(0.0f, 1.0f));

    input_system.ProcessEvents(events);

    EXPECT_TRUE(input_system.IsActionActive(fire_handle));
    EXPECT_TRUE(input_system.WasActionStarted(fire_handle));
    EXPECT_EQ(input_system.GetRawState().mouse_x, 100.0f);
    EXPECT_EQ(input_system.GetRawState().mouse_y, 200.0f);
    EXPECT_FLOAT_EQ(input_system.GetRawState().mouse_delta_x, 4.5f);
    EXPECT_FLOAT_EQ(input_system.GetRawState().mouse_delta_y, -2.0f);
    EXPECT_FLOAT_EQ(input_system.GetRawState().wheel_y, 1.0f);
}

TEST(InputSystemTest, DigitalActionReportsUnitValueWhenActive) {
    InputSystem input_system;
    const ActionHandle jump_handle = input_system.BindAction("jump", InputBinding::Key(SDLK_SPACE));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_SPACE), false});
    input_system.Update();

    EXPECT_FLOAT_EQ(input_system.GetActionValue(jump_handle), 1.0f);
}

TEST(InputSystemTest, KeyboardAndGamepadAxisShareOneAction) {
    InputSystem input_system;
    auto move = input_system.BindAction<1>("move", ActionConfig{.processing = ActionProcessing::ClampLength});
    move.Bind(InputBinding::Key(SDLK_A).Component(0, -1.0f));
    move.Bind(InputBinding::Key(SDLK_D).Component(0, +1.0f));
    move.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTX).Component(0, +1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_D), false});
    input_system.Update();
    EXPECT_FLOAT_EQ(move.Scalar(), 1.0f);

    // Release the key and drive the same action from the stick alone.
    input_system.BeginFrame();
    input_system.ProcessEvent(KeyUpEvent{static_cast<std::int32_t>(SDLK_D)});
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTX, 0.5f});
    input_system.Update();
    EXPECT_FLOAT_EQ(move.Scalar(), 0.5f);

    // A now cancels half of the stick: -1.0 + 0.5.
    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_A), false});
    input_system.Update();
    EXPECT_FLOAT_EQ(move.Scalar(), -0.5f);
}

TEST(InputSystemTest, ClampLengthLimitsDiagonalMagnitude) {
    InputSystem input_system;
    auto stick = input_system.BindAction<2>("stick", ActionConfig{.processing = ActionProcessing::ClampLength});
    stick.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTX).Component(0, 1.0f));
    stick.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTY).Component(1, -1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTX, 1.0f});
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTY, -1.0f});
    input_system.Update();

    const auto value = stick.Value();
    EXPECT_NEAR(value[0], 0.70710678f, 1e-5f);
    EXPECT_NEAR(value[1], 0.70710678f, 1e-5f);
    EXPECT_NEAR(std::sqrt(value[0] * value[0] + value[1] * value[1]), 1.0f, 1e-5f);
}

TEST(InputSystemTest, NormalizeProcessingReportsUnitDirection) {
    InputSystem input_system;
    auto direction = input_system.BindAction<2>("direction", ActionConfig{.processing = ActionProcessing::Normalize});
    direction.Bind(InputBinding::Key(SDLK_D).Component(0, 1.0f));
    direction.Bind(InputBinding::Key(SDLK_W).Component(1, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_D), false});
    input_system.Update();
    auto value = direction.Value();
    EXPECT_FLOAT_EQ(value[0], 1.0f);
    EXPECT_FLOAT_EQ(value[1], 0.0f);

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_W), false});
    input_system.Update();
    value = direction.Value();
    EXPECT_NEAR(value[0], 0.70710678f, 1e-5f);
    EXPECT_NEAR(value[1], 0.70710678f, 1e-5f);
}

TEST(InputSystemTest, DirectProcessingKeepsRawSum) {
    InputSystem input_system;
    auto move = input_system.BindAction<1>("move", ActionConfig{.processing = ActionProcessing::Direct});
    move.Bind(InputBinding::Key(SDLK_A).Component(0, -1.0f));
    move.Bind(InputBinding::Key(SDLK_D).Component(0, +1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_A), false});
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_D), false});
    input_system.Update();

    EXPECT_FLOAT_EQ(move.Scalar(), 0.0f);
}

TEST(InputSystemTest, BindingDeadzoneSuppressesSmallAxisValues) {
    InputSystem input_system;
    auto move = input_system.BindAction<1>("move", ActionConfig{.processing = ActionProcessing::Direct});
    move.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTX).Component(0, 1.0f).Deadzone(0.2f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTX, 0.1f});
    input_system.Update();
    EXPECT_FLOAT_EQ(move.Scalar(), 0.0f);
    EXPECT_FALSE(move.Active());

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTX, 0.6f});
    input_system.Update();
    EXPECT_FLOAT_EQ(move.Scalar(), 0.6f);
    EXPECT_TRUE(move.Active());
}

TEST(InputSystemTest, ActionDeadzoneZeroesWholeVector) {
    InputSystem input_system;
    auto stick = input_system.BindAction<2>("stick", ActionConfig{
        .processing = ActionProcessing::ClampLength,
        .deadzone = 0.3f,
    });
    stick.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTX).Component(0, 1.0f));
    stick.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTY).Component(1, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTX, 0.1f});
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTY, 0.1f});
    input_system.Update();

    const auto value = stick.Value();
    EXPECT_FLOAT_EQ(value[0], 0.0f);
    EXPECT_FLOAT_EQ(value[1], 0.0f);
}

TEST(InputSystemTest, AnalogHysteresisDrivesStartedAndCanceled) {
    InputSystem input_system;
    auto trigger = input_system.BindAction<1>("trigger", ActionConfig{
        .processing = ActionProcessing::Direct,
        .press_threshold = 0.5f,
        .release_threshold = 0.3f,
    });
    trigger.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER).Component(0, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.6f});
    input_system.Update();
    EXPECT_TRUE(trigger.Active());
    EXPECT_TRUE(trigger.Started());
    EXPECT_FALSE(trigger.Canceled());

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.4f});
    input_system.Update();
    EXPECT_TRUE(trigger.Active()); // above release threshold, stays engaged
    EXPECT_FALSE(trigger.Started());
    EXPECT_FALSE(trigger.Canceled());

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.2f});
    input_system.Update();
    EXPECT_FALSE(trigger.Active());
    EXPECT_TRUE(trigger.Canceled());
}

TEST(InputSystemTest, TriggerBiasMapsZeroToOneToSignedRange) {
    InputSystem input_system;
    auto trigger = input_system.BindAction<1>("trigger", ActionConfig{.processing = ActionProcessing::Direct});
    trigger.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER).Component(0, 2.0f).Bias(-0.5f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f});
    input_system.Update();
    EXPECT_FLOAT_EQ(trigger.Scalar(), -1.0f);

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.5f});
    input_system.Update();
    EXPECT_FLOAT_EQ(trigger.Scalar(), 0.0f);

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1.0f});
    input_system.Update();
    EXPECT_FLOAT_EQ(trigger.Scalar(), 1.0f);
}

TEST(InputSystemTest, GamepadButtonBindingActivatesAction) {
    InputSystem input_system;
    const ActionHandle jump_handle = input_system.BindAction("jump", InputBinding::GamepadButton(SDL_GAMEPAD_BUTTON_SOUTH));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadButtonDownEvent{0, SDL_GAMEPAD_BUTTON_SOUTH});
    input_system.Update();
    EXPECT_TRUE(input_system.IsActionActive(jump_handle));
    EXPECT_TRUE(input_system.WasActionStarted(jump_handle));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadButtonUpEvent{0, SDL_GAMEPAD_BUTTON_SOUTH});
    input_system.Update();
    EXPECT_FALSE(input_system.IsActionActive(jump_handle));
    EXPECT_TRUE(input_system.WasActionCanceled(jump_handle));
}

TEST(InputSystemTest, AnyGamepadBindingMatchesAnyDevice) {
    InputSystem input_system;
    auto move = input_system.BindAction<1>("move", ActionConfig{.processing = ActionProcessing::Direct});
    move.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTX).Component(0, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{3, SDL_GAMEPAD_AXIS_LEFTX, -0.4f});
    input_system.Update();

    EXPECT_FLOAT_EQ(move.Scalar(), -0.4f);
    EXPECT_TRUE(input_system.IsGamepadConnected(3));
}

TEST(InputSystemTest, SpecificGamepadIndexSelectsThatDevice) {
    InputSystem input_system;
    auto move = input_system.BindAction<1>("move", ActionConfig{.processing = ActionProcessing::Direct});
    move.Bind(InputBinding::GamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, /*gamepad=*/1).Component(0, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadAxisMotionEvent{0, SDL_GAMEPAD_AXIS_LEFTX, 0.2f});
    input_system.ProcessEvent(GamepadAxisMotionEvent{1, SDL_GAMEPAD_AXIS_LEFTX, 0.9f});
    input_system.Update();

    EXPECT_FLOAT_EQ(move.Scalar(), 0.9f);
}

TEST(InputSystemTest, DisconnectClearsGamepadState) {
    InputSystem input_system;
    const ActionHandle jump_handle = input_system.BindAction("jump", InputBinding::GamepadButton(SDL_GAMEPAD_BUTTON_SOUTH));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadConnectedEvent{0});
    input_system.ProcessEvent(GamepadButtonDownEvent{0, SDL_GAMEPAD_BUTTON_SOUTH});
    input_system.Update();
    EXPECT_TRUE(input_system.IsActionActive(jump_handle));
    EXPECT_TRUE(input_system.IsGamepadConnected(0));

    input_system.BeginFrame();
    input_system.ProcessEvent(GamepadDisconnectedEvent{0});
    input_system.Update();
    EXPECT_FALSE(input_system.IsActionActive(jump_handle));
    EXPECT_FALSE(input_system.IsGamepadConnected(0));
}

TEST(InputSystemTest, MouseWheelAndAxisFeedAnalogActions) {
    InputSystem input_system;

    auto scroll = input_system.BindAction<1>("scroll", ActionConfig{.processing = ActionProcessing::Direct});
    scroll.Bind(InputBinding::MouseWheel(1).Component(0, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(MouseWheelEvent{0.0f, 2.5f});
    input_system.Update();
    EXPECT_FLOAT_EQ(scroll.Scalar(), 2.5f);

    auto look = input_system.BindAction<2>("look", ActionConfig{.processing = ActionProcessing::Direct});
    look.Bind(InputBinding::MouseAxis(0).Component(0, 1.0f));
    look.Bind(InputBinding::MouseAxis(1).Component(1, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(MouseMotionEvent{0.0f, 0.0f, 3.0f, -4.0f});
    input_system.Update();

    const auto value = look.Value();
    EXPECT_FLOAT_EQ(value[0], 3.0f);
    EXPECT_FLOAT_EQ(value[1], -4.0f);
}

TEST(InputSystemTest, ActionVectorMirrorsConfiguredDimension) {
    InputSystem input_system;
    auto move = input_system.BindAction<2>("move", ActionConfig{});
    move.Bind(InputBinding::Key(SDLK_D).Component(0, 1.0f));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<std::int32_t>(SDLK_D), false});
    input_system.Update();

    EXPECT_EQ(input_system.GetActionVector(move.Handle()).size(), 2u);
    EXPECT_EQ(input_system.GetActionValue2D(move.Handle()),
              (std::pair<float, float>{1.0f, 0.0f}));
}

}  // namespace
