#include <gtest/gtest.h>

import std;

import VulkanEngine.Text.FontWatcher;

namespace {

using VulkanEngine::Text::FontWatcher;

// A notified change is not immediately pollable while the debounce window is
// open, then becomes pollable once it elapses.
TEST(FontWatcherTest, DebouncesChanges) {
    FontWatcher watcher;
    watcher.SetDebounce(std::chrono::milliseconds(50));
    watcher.NotifyChanged("/tmp/fonts/Lato-Regular.ttf");
    EXPECT_TRUE(watcher.PollChanged().empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    const auto changed = watcher.PollChanged();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0], "/tmp/fonts/Lato-Regular.ttf");
}

// Repeated notifications of the same path collapse to one pending reload and
// one poll result, even with no debounce window.
TEST(FontWatcherTest, DeduplicatesRepeatedChanges) {
    FontWatcher watcher;
    watcher.SetDebounce(std::chrono::milliseconds(0));
    watcher.NotifyChanged("/tmp/Lato-Regular.ttf");
    watcher.NotifyChanged("/tmp/Lato-Regular.ttf");
    watcher.NotifyChanged("/tmp/Lato-Regular.ttf");
    const auto changed = watcher.PollChanged();
    EXPECT_EQ(changed.size(), 1u);
}

// Polling drains: a second poll returns nothing until a new change arrives.
TEST(FontWatcherTest, PollDrains) {
    FontWatcher watcher;
    watcher.SetDebounce(std::chrono::milliseconds(0));
    watcher.NotifyChanged("/tmp/Lato-Bold.ttf");
    EXPECT_EQ(watcher.PollChanged().size(), 1u);
    EXPECT_TRUE(watcher.PollChanged().empty());
}

// A stopped watcher ignores new notifications.
TEST(FontWatcherTest, StopSuppressesChanges) {
    FontWatcher watcher;
    watcher.SetDebounce(std::chrono::milliseconds(0));
    watcher.Stop();
    watcher.NotifyChanged("/tmp/Lato-Black.ttf");
    EXPECT_TRUE(watcher.PollChanged().empty());
}

// Stop then Start puts the watcher back in service: the stopped window's
// notifications are dropped and a later one is polled normally.
TEST(FontWatcherTest, StopThenStartResumesNotifications) {
    FontWatcher watcher;
    watcher.SetDebounce(std::chrono::milliseconds(0));
    watcher.Stop();
    watcher.NotifyChanged("/tmp/dropped.ttf");
    EXPECT_TRUE(watcher.PollChanged().empty());

    watcher.Start();
    watcher.NotifyChanged("/tmp/resumed.ttf");
    const auto changed = watcher.PollChanged();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0], "/tmp/resumed.ttf");
    watcher.Stop();
}

// Directories are remembered before Start and de-duplicated.
TEST(FontWatcherTest, RegistersDirectoriesBeforeStart) {
    FontWatcher watcher;
    EXPECT_TRUE(watcher.AddDirectory("/tmp/fonts"));
    EXPECT_TRUE(watcher.AddDirectory("/tmp/fonts"));
    EXPECT_EQ(watcher.GetRegisteredDirectoryCount(), 1u);
    watcher.Stop();
}

// An empty directory is rejected rather than remembered.
TEST(FontWatcherTest, RejectsAnEmptyDirectory) {
    FontWatcher watcher;
    EXPECT_FALSE(watcher.AddDirectory(""));
    EXPECT_EQ(watcher.GetRegisteredDirectoryCount(), 0u);
}

}  // namespace
