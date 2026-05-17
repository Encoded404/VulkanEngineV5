#include <gtest/gtest.h>

#include <memory>

import VulkanBackend.Event;

namespace {

using namespace VulkanEngine::Backend::Event;

TEST(BackendEventTest, ClonesConcreteEvents) {
    WindowResizedEvent original{1280u, 720u};

    const auto clone = original.Clone();
    ASSERT_NE(clone, nullptr);
    EXPECT_EQ(clone->GetType(), typeid(WindowResizedEvent));
    EXPECT_EQ(clone->GetCategory(), EventCategory::Platform);

    const auto* resized = dynamic_cast<const WindowResizedEvent*>(clone.get());
    ASSERT_NE(resized, nullptr);
    EXPECT_EQ(resized->width, 1280u);
    EXPECT_EQ(resized->height, 720u);
}

TEST(BackendEventTest, EventQueueDrainsOwnedEvents) {
    EventQueue queue;
    queue.Push(std::make_unique<QuitEvent>());
    queue.Push(std::make_unique<KeyDownEvent>(42, false));

    EXPECT_FALSE(queue.Empty());
    EXPECT_EQ(queue.Size(), 2u);

    auto drained = queue.Drain();
    EXPECT_TRUE(queue.Empty());
    EXPECT_EQ(queue.Size(), 0u);
    ASSERT_EQ(drained.size(), 2u);
    EXPECT_NE(dynamic_cast<QuitEvent*>(drained[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<KeyDownEvent*>(drained[1].get()), nullptr);
}

}  // namespace

