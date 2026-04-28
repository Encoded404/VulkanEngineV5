#include <gtest/gtest.h>

#include <SDL3/SDL_keycode.h>

import VulkanBackend.Event;
import VulkanEngine.Input;

namespace {

using namespace VulkanEngine::Backend::Event;
using namespace VulkanEngine::Input;

TEST(InputSystemTest, TracksKeyboardActionsAcrossFrames) {
    InputSystem input_system;
    input_system.BindAction("jump", InputBinding::Key(SDLK_SPACE));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyDownEvent{static_cast<int32_t>(SDLK_SPACE), false});
    input_system.Update();

    EXPECT_TRUE(input_system.IsActionActive("jump"));
    EXPECT_TRUE(input_system.WasActionStarted("jump"));
    EXPECT_FALSE(input_system.WasActionCanceled("jump"));

    input_system.BeginFrame();
    input_system.ProcessEvent(KeyUpEvent{static_cast<int32_t>(SDLK_SPACE)});
    input_system.Update();

    EXPECT_FALSE(input_system.IsActionActive("jump"));
    EXPECT_FALSE(input_system.WasActionStarted("jump"));
    EXPECT_TRUE(input_system.WasActionCanceled("jump"));
}

TEST(InputSystemTest, ProcessesBatchedMouseInputEvents) {
    InputSystem input_system;
    input_system.BindAction("fire", InputBinding::MouseButton(1));

    EventList events{};
    events.push_back(std::make_unique<MouseButtonDownEvent>(1, 100, 200));
    events.push_back(std::make_unique<MouseMotionEvent>(100.0f, 200.0f, 4.5f, -2.0f));
    events.push_back(std::make_unique<MouseWheelEvent>(0.0f, 1.0f));

    input_system.ProcessEvents(events);

    EXPECT_TRUE(input_system.IsActionActive("fire"));
    EXPECT_TRUE(input_system.WasActionStarted("fire"));
    EXPECT_EQ(input_system.GetRawState().mouse_x, 100.0f);
    EXPECT_EQ(input_system.GetRawState().mouse_y, 200.0f);
    EXPECT_FLOAT_EQ(input_system.GetRawState().mouse_delta_x, 4.5f);
    EXPECT_FLOAT_EQ(input_system.GetRawState().mouse_delta_y, -2.0f);
    EXPECT_FLOAT_EQ(input_system.GetRawState().wheel_y, 1.0f);
}

}  // namespace

