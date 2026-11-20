#include <gtest/gtest.h>

import std;

import VulkanEngine.MeshRegistry;
import VulkanEngine.SceneGpuLayout;

namespace {

using VulkanEngine::SceneGpuLayout::TableId;
using VulkanEngine::TablePublishRecord;

// One write per block per frame: the second mesh in the same block writes nothing.
TEST(TablePublishRecordTest, NotesAPairOnce) {
    TablePublishRecord record;
    EXPECT_TRUE(record.Note(TableId::vertex, 3));
    EXPECT_FALSE(record.Note(TableId::vertex, 3));
    EXPECT_TRUE(record.Contains(TableId::vertex, 3));
}

// Block indices are per heap, so the same number in two tables is two different elements.
TEST(TablePublishRecordTest, TablesAreIndependent) {
    TablePublishRecord record;
    EXPECT_TRUE(record.Note(TableId::vertex, 3));
    EXPECT_TRUE(record.Note(TableId::uv, 3));
    EXPECT_TRUE(record.Contains(TableId::vertex, 3));
    EXPECT_TRUE(record.Contains(TableId::uv, 3));
    EXPECT_FALSE(record.Contains(TableId::index, 3));
}

// The record is per frame, so clearing it makes the next publish of a block new again.
TEST(TablePublishRecordTest, ClearForgetsEveryPair) {
    TablePublishRecord record;
    EXPECT_TRUE(record.Note(TableId::vertex, 1));
    EXPECT_TRUE(record.Note(TableId::index, 2));
    record.Clear();
    EXPECT_FALSE(record.Contains(TableId::vertex, 1));
    EXPECT_FALSE(record.Contains(TableId::index, 2));
    EXPECT_TRUE(record.Note(TableId::vertex, 1));
}

// The count sentinel is not a table, and a table with nothing noted answers no for every index.
TEST(TablePublishRecordTest, UntouchedTableAndSentinelAnswerFalse) {
    TablePublishRecord record;
    EXPECT_FALSE(record.Contains(TableId::light, 0));
    EXPECT_FALSE(record.Contains(TableId::count, 0));
    EXPECT_FALSE(record.Note(TableId::count, 0));
    EXPECT_TRUE(record.Note(TableId::light, 0));
    EXPECT_FALSE(record.Contains(TableId::light, 1));
}

} // namespace
