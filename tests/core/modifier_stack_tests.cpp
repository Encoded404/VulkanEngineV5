#include <gtest/gtest.h>

import std;

import VulkanEngine.Components.GeometryModifier;
import VulkanEngine.ModifierManager;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::Components;
using namespace VulkanEngine::Modifiers;

ModifierId Id(const std::uint32_t value) {
    return ModifierId{.value = value, .generation = 0};
}

ModifierInstance Instance(const std::uint32_t id) {
    ModifierInstance instance{};
    instance.id = Id(id);
    return instance;
}

// The effective stage ids for one object-local submesh, in execution order.
std::vector<std::uint32_t> StageIds(const GeometryModifier& modifier,
                                   const std::uint32_t local_submesh) {
    std::vector<std::uint32_t> ids;
    ForEachModifierInstance(modifier, local_submesh,
                            [&ids](const ModifierInstance& instance) {
                                ids.push_back(instance.id.value);
                            });
    return ids;
}

TEST(ModifierStackTest, InheritsTheObjectStackWhenTheSubmeshSaysNothing) {
    GeometryModifier modifier{};
    modifier.modifiers = {Instance(1), Instance(2)};

    EXPECT_EQ(StageIds(modifier, 0), (std::vector<std::uint32_t>{1, 2}));
    // A submesh past the sparse vector's end inherits too.
    EXPECT_EQ(StageIds(modifier, 7), (std::vector<std::uint32_t>{1, 2}));
}

TEST(ModifierStackTest, NulloptAndAppendBothInherit) {
    GeometryModifier modifier{};
    modifier.modifiers = {Instance(1)};

    // An explicitly empty optional is the same as no entry at all.
    modifier.submeshes.resize(2);
    EXPECT_EQ(StageIds(modifier, 0), (std::vector<std::uint32_t>{1}));

    SubMeshModifierStack append{};
    append.mode = ModifierStackMode::Append;
    append.modifiers = {Instance(2)};
    modifier.submeshes[1] = append;
    EXPECT_EQ(StageIds(modifier, 1), (std::vector<std::uint32_t>{1, 2}));
}

TEST(ModifierStackTest, AppendRunsAfterTheObjectStack) {
    GeometryModifier modifier{};
    modifier.modifiers = {Instance(1)};
    SubMeshModifierStack stack{};
    stack.mode = ModifierStackMode::Append;
    stack.modifiers = {Instance(2), Instance(3)};
    modifier.submeshes.resize(1);
    modifier.submeshes[0] = stack;

    EXPECT_EQ(StageIds(modifier, 0), (std::vector<std::uint32_t>{1, 2, 3}));
}

TEST(ModifierStackTest, ReplaceDropsTheObjectStack) {
    GeometryModifier modifier{};
    modifier.modifiers = {Instance(1)};
    SubMeshModifierStack stack{};
    stack.mode = ModifierStackMode::Replace;
    stack.modifiers = {Instance(4)};
    modifier.submeshes.resize(1);
    modifier.submeshes[0] = stack;

    EXPECT_EQ(StageIds(modifier, 0), (std::vector<std::uint32_t>{4}));
}

TEST(ModifierStackTest, EmptyReplaceDisablesModifiersOnThatSubmesh) {
    GeometryModifier modifier{};
    modifier.modifiers = {Instance(1)};
    SubMeshModifierStack stack{};
    stack.mode = ModifierStackMode::Replace;
    modifier.submeshes.resize(2);
    modifier.submeshes[0] = stack;

    EXPECT_TRUE(StageIds(modifier, 0).empty());
    // The neighbouring submesh is untouched: Replace is per object-local submesh.
    EXPECT_EQ(StageIds(modifier, 1), (std::vector<std::uint32_t>{1}));
}

TEST(ModifierStackTest, AnEmptyObjectStackStillRunsSubMeshEntries) {
    GeometryModifier modifier{};
    SubMeshModifierStack stack{};
    stack.mode = ModifierStackMode::Append;
    stack.modifiers = {Instance(6)};
    modifier.submeshes.resize(1);
    modifier.submeshes[0] = stack;

    EXPECT_EQ(StageIds(modifier, 0), (std::vector<std::uint32_t>{6}));
}

TEST(ModifierStackTest, SubMeshStacksAreSparseAndAddressable) {
    GeometryModifier modifier{};
    EXPECT_EQ(modifier.GetSubMeshStack(0), nullptr);

    SubMeshModifierStack& stack = modifier.SetSubMeshStack(2);
    stack.mode = ModifierStackMode::Replace;
    ASSERT_EQ(modifier.submeshes.size(), 3u);
    // The gap is explicitly inherit, not "unset but sized".
    EXPECT_FALSE(modifier.submeshes[0].has_value());
    EXPECT_FALSE(modifier.submeshes[1].has_value());
    ASSERT_NE(modifier.GetSubMeshStack(2), nullptr);
    EXPECT_EQ(modifier.GetSubMeshStack(2)->mode, ModifierStackMode::Replace);

    // Setting the same index twice returns the same slot instead of resetting it.
    stack.modifiers = {Instance(9)};
    SubMeshModifierStack& again = modifier.SetSubMeshStack(2);
    EXPECT_EQ(again.modifiers.size(), 1u);
}

TEST(ModifierStackTest, VisitsParamsInStageOrder) {
    GeometryModifier modifier{};
    ModifierInstance first = Instance(1);
    first.params = {std::byte{0xAA}, std::byte{0xBB}};
    ModifierInstance second = Instance(2);
    second.params = {std::byte{0xCC}};
    modifier.modifiers = {first, second};

    std::vector<std::uint8_t> markers;
    ForEachModifierInstance(modifier, 0, [&markers](const ModifierInstance& instance) {
        markers.push_back(static_cast<std::uint8_t>(instance.params.front()));
    });
    EXPECT_EQ(markers, (std::vector<std::uint8_t>{0xAA, 0xCC}));
}

// Mesh fingerprint adoption and the bounds policy live in
// geometry_modifier_tests.cpp: this file only owns stack resolution.

} // namespace
