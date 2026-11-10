#include <gtest/gtest.h>

import std;

import VulkanEngine.ModifierManager;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::Modifiers;

ModifierDesc Desc(const std::string& name = "test",
                  const ShaderSystem::ShaderId shader = ShaderSystem::ShaderId{7}) {
    ModifierDesc desc{};
    desc.name = name;
    desc.compute_shader = shader;
    return desc;
}

TEST(ModifierRegistryTest, RegisterReturnsALiveHandleAndBumpsTheVersion) {
    ModifierManager manager{};
    const std::uint64_t before = manager.RegistryVersion();

    const ModifierId id = manager.Register(Desc("displace"));
    ASSERT_TRUE(id.IsValid());
    EXPECT_GT(manager.RegistryVersion(), before);
    EXPECT_TRUE(manager.IsAlive(id));
    ASSERT_NE(manager.Get(id), nullptr);
    EXPECT_EQ(manager.Get(id)->name, "displace");
    EXPECT_FALSE(manager.Get(id)->builtin);
    // Only RegisterBuiltin may mint a builtin, whatever the descriptor claimed.
    EXPECT_FALSE(manager.SkinningId().IsValid());
}

TEST(ModifierRegistryTest, RegisterRejectsInvalidDescriptors) {
    ModifierManager manager{};
    const std::uint64_t baseline = manager.RegistryVersion();

    EXPECT_FALSE(manager.Register(Desc("no-shader", ShaderSystem::kInvalidShaderId)).IsValid());

    ModifierDesc too_many_params = Desc("too-many-params");
    too_many_params.param_size = ModifierManager::kMaxParamBytes + 1;
    EXPECT_FALSE(manager.Register(too_many_params).IsValid());

    for (const std::uint32_t alignment : {0u, 1u, 3u, 512u}) {
        ModifierDesc bad_alignment = Desc("bad-alignment");
        bad_alignment.param_alignment = alignment;
        EXPECT_FALSE(manager.Register(bad_alignment).IsValid()) << "alignment=" << alignment;
    }

    ModifierDesc zero_group = Desc("zero-workgroup");
    zero_group.workgroup_size = 0;
    EXPECT_FALSE(manager.Register(zero_group).IsValid());

    ModifierDesc huge_group = Desc("huge-workgroup");
    huge_group.workgroup_size = ModifierManager::kMaxWorkgroupSize + 1;
    EXPECT_FALSE(manager.Register(huge_group).IsValid());

    // A rejected registration may not advance the version: a renderer reconciles
    // pipelines on a version change, and a rejected entry has nothing to build.
    EXPECT_EQ(manager.RegistryVersion(), baseline);

    // The boundary values are accepted.
    for (const std::uint32_t alignment : {4u, 16u, 256u}) {
        ModifierDesc good_alignment = Desc("good-alignment");
        good_alignment.param_alignment = alignment;
        EXPECT_TRUE(manager.Register(good_alignment).IsValid()) << "alignment=" << alignment;
    }
}

TEST(ModifierRegistryTest, RegisterBuiltinRecordsSkinningExactlyOnce) {
    ModifierManager manager{};
    EXPECT_FALSE(manager.SkinningId().IsValid());

    ModifierDesc skinning = Desc(std::string(ModifierManager::kSkinningName));
    skinning.param_size = 0;
    const ModifierId id = manager.RegisterBuiltin(skinning);
    ASSERT_TRUE(id.IsValid());
    EXPECT_EQ(manager.SkinningId(), id);
    ASSERT_NE(manager.Get(id), nullptr);
    EXPECT_TRUE(manager.Get(id)->builtin);

    // A second builtin with the reserved name would make SkinningId() ambiguous.
    EXPECT_FALSE(manager.RegisterBuiltin(Desc(std::string(ModifierManager::kSkinningName))).IsValid());
    EXPECT_EQ(manager.SkinningId(), id);
}

TEST(ModifierRegistryTest, RegisterCannotCreateABuiltin) {
    ModifierManager manager{};
    ModifierDesc desc = Desc("pretender");
    desc.builtin = true;
    const ModifierId id = manager.Register(desc);
    ASSERT_TRUE(id.IsValid());
    ASSERT_NE(manager.Get(id), nullptr);
    EXPECT_FALSE(manager.Get(id)->builtin);
    // And therefore it is removable.
    manager.Unregister(id);
    EXPECT_FALSE(manager.IsAlive(id));
}

TEST(ModifierRegistryTest, UnregisterInvalidatesTheHandleAndBumpsTheVersion) {
    ModifierManager manager{};
    const ModifierId id = manager.Register(Desc());
    ASSERT_TRUE(id.IsValid());
    const std::uint64_t before = manager.RegistryVersion();

    manager.Unregister(id);
    EXPECT_FALSE(manager.IsAlive(id));
    EXPECT_EQ(manager.Get(id), nullptr);
    EXPECT_GT(manager.RegistryVersion(), before);

    // Unregistering twice is a no-op, not a version bump.
    const std::uint64_t after = manager.RegistryVersion();
    manager.Unregister(id);
    EXPECT_EQ(manager.RegistryVersion(), after);
}

TEST(ModifierRegistryTest, ARecycledSlotGetsANewGeneration) {
    ModifierManager manager{};
    const ModifierId first = manager.Register(Desc("a"));
    ASSERT_TRUE(first.IsValid());
    manager.Unregister(first);

    const ModifierId second = manager.Register(Desc("b"));
    ASSERT_TRUE(second.IsValid());
    // The slot is reused (that is what makes the registry worth having), so the
    // generation is the only thing that tells the two handles apart.
    EXPECT_EQ(second.value, first.value);
    EXPECT_NE(second.generation, first.generation);
    EXPECT_FALSE(manager.IsAlive(first));
    EXPECT_TRUE(manager.IsAlive(second));
    EXPECT_EQ(manager.Get(second)->name, "b");
}

TEST(ModifierRegistryTest, BuiltinsCannotBeUnregistered) {
    ModifierManager manager{};
    const ModifierId skinning = manager.RegisterBuiltin(
        Desc(std::string(ModifierManager::kSkinningName)));
    ASSERT_TRUE(skinning.IsValid());

    manager.Unregister(skinning);
    EXPECT_TRUE(manager.IsAlive(skinning));
    EXPECT_EQ(manager.SkinningId(), skinning);
}

TEST(ModifierRegistryTest, ResetClearsEverythingIncludingSkinning) {
    ModifierManager manager{};
    const ModifierId skinning = manager.RegisterBuiltin(
        Desc(std::string(ModifierManager::kSkinningName)));
    const ModifierId app = manager.Register(Desc("app"));
    ASSERT_TRUE(skinning.IsValid());
    ASSERT_TRUE(app.IsValid());

    manager.Reset();
    EXPECT_FALSE(manager.IsAlive(skinning));
    EXPECT_FALSE(manager.IsAlive(app));
    EXPECT_FALSE(manager.SkinningId().IsValid());
    EXPECT_TRUE(manager.Entries().empty());

    // A later initialization can register skinning again exactly once.
    const ModifierId revived = manager.RegisterBuiltin(
        Desc(std::string(ModifierManager::kSkinningName)));
    EXPECT_TRUE(revived.IsValid());
    EXPECT_EQ(manager.SkinningId(), revived);
}

TEST(ModifierRegistryTest, EntriesExposeLiveAndRetiredSlots) {
    ModifierManager manager{};
    const ModifierId first = manager.Register(Desc("first"));
    const ModifierId second = manager.Register(Desc("second"));
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());
    ASSERT_EQ(manager.Entries().size(), 2u);

    manager.Unregister(first);
    // The retired slot stays in the view (the slot array is append-only), but it
    // is marked dead and its descriptor is cleared.
    ASSERT_EQ(manager.Entries().size(), 2u);
    bool saw_dead = false;
    bool saw_live = false;
    for (const ModifierManager::Entry& entry : manager.Entries()) {
        if (entry.alive) {
            saw_live = true;
            EXPECT_EQ(entry.id, second);
        } else {
            saw_dead = true;
            EXPECT_TRUE(entry.desc.name.empty());
        }
    }
    EXPECT_TRUE(saw_dead);
    EXPECT_TRUE(saw_live);
}

} // namespace
