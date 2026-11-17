#include <gtest/gtest.h>

import std;

import vulkan;

import VulkanEngine.SceneGpuLayout;
import VulkanEngine.SceneLimits;

namespace {

using namespace VulkanEngine::SceneGpuLayout;
using namespace VulkanEngine::SceneLimits;

[[nodiscard]] constexpr bool HasUsage(const vk::BufferUsageFlags usage,
                                      const vk::BufferUsageFlagBits bit) {
    return static_cast<bool>(usage & bit);
}

// Binding numbers each set's layout had when it was hand-written.
// Pinned as literals so a table edit that renumbers a binding (and silently breaks the matching shader) fails here instead of on the GPU.
constexpr std::array<std::pair<SetId, std::uint32_t>, kSetCount> kExpectedBindingCounts{{
    {SetId::submesh_vertex, 1},  {SetId::vertex_buffers, 3}, {SetId::indirection, 1},
    {SetId::index_buffers, 1},   {SetId::deform, 6},         {SetId::expand, 8},
    {SetId::occlusion, 12},      {SetId::occluder_select, 11}, {SetId::collect, 6},
    {SetId::collect_write, 2},   {SetId::empty, 0},          {SetId::scene_uniform, 2},
    {SetId::hiz, 3},
}};

} // namespace

TEST(SceneTablesTest, SetBindingCountsMatchTheirShaders) {
    for (const auto& [id, expected] : kExpectedBindingCounts) {
        EXPECT_EQ(BindingCount(id), expected) << SetOf(id).name;
    }
}

TEST(SceneTablesTest, BindingNumbersAreUniquePerSet) {
    for (const SetSpec& set : kSceneSets) {
        std::array<bool, 64> seen{};
        for (const BindingSpec& b : set.bindings) {
            ASSERT_LT(b.binding, seen.size()) << set.name << " binding " << b.binding;
            EXPECT_FALSE(seen[b.binding]) << set.name << " binding " << b.binding << " repeats";
            seen[b.binding] = true;
        }
    }
}

TEST(SceneTablesTest, EveryBindingNamesItsSourceAndWriter) {
    // A binding whose descriptors are never written is a GPU hang, so every row must say who fills it.
    // A row with no engine-side source is filled out of band (mesh upload, lighting upload, Hi-Z re-create): nothing in the frame loop may claim it.
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.source == SourceKind::none) {
                EXPECT_EQ(b.writer, Writer::external)
                    << set.name << " binding " << b.binding
                    << " has no source but claims a frame-loop writer";
            }
        }
    }
}

TEST(SceneTablesTest, BlockBindingsMatchTheirBlockArraySource) {
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.source == SourceKind::block_array) {
                // A block array is one descriptor per block.
                EXPECT_EQ(b.count, kBlocks)
                    << set.name << " binding " << b.binding
                    << " binds a block array with a fixed count";
            }
            if (b.count == kBlocks) {
                EXPECT_EQ(b.source, SourceKind::block_array)
                    << set.name << " binding " << b.binding
                    << " uses the block budget without binding a block array";
            }
            // The write cursor is indexed positionally, so an unknown row would silently share or lose a cursor.
            if (b.source == SourceKind::block_array) {
                EXPECT_NE(BlockBindingIndex(set.id, b.binding), kNoBinding);
            }
        }
    }
}

TEST(SceneTablesTest, EveryBlockSlotIsTabledOnce) {
    std::array<bool, kBlockSlotCount> tabled{};
    for (const BlockArraySpec& spec : kBlockArrays) {
        const auto slot = static_cast<std::size_t>(spec.slot);
        ASSERT_LT(slot, tabled.size());
        EXPECT_FALSE(tabled[slot]) << "block array slot " << slot << " appears twice";
        tabled[slot] = true;
        EXPECT_GT(spec.entry_size, 0u);
        EXPECT_EQ(spec.entries_per_block, kEntriesPerBlock);
    }
    for (std::size_t i = 0; i < tabled.size(); ++i) {
        EXPECT_TRUE(tabled[i]) << "block array slot " << i << " has no row";
    }
    // Every block slot must actually be bound somewhere, or the array it backs is allocated for nothing.
    std::array<bool, kBlockSlotCount> bound{};
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.source == SourceKind::block_array) {
                bound[static_cast<std::size_t>(b.block_slot)] = true;
            }
        }
    }
    for (std::size_t i = 0; i < bound.size(); ++i) {
        EXPECT_TRUE(bound[i]) << "block array slot " << i << " is never bound";
    }
    EXPECT_EQ(BlockBindingCount(), 15u);
}

TEST(SceneTablesTest, UpdateAfterBindIsDerivedFromTheBindings) {
    // A layout that carries an update-after-bind binding MUST be created with eUpdateAfterBindPool.
    // These four sets have one; every other set must not, or the frame loop would re-point descriptors it is not allowed to.
    std::array<bool, kSetCount> expected{};
    expected[static_cast<std::size_t>(SetId::vertex_buffers)] = true;
    expected[static_cast<std::size_t>(SetId::indirection)] = true;
    expected[static_cast<std::size_t>(SetId::index_buffers)] = true;
    expected[static_cast<std::size_t>(SetId::deform)] = true;
    for (const SetSpec& set : kSceneSets) {
        EXPECT_EQ(UpdatesAfterBind(set.id), expected[static_cast<std::size_t>(set.id)])
            << set.name;
    }
}

TEST(SceneTablesTest, VariableCountIsOnlyOnTheHighestBinding) {
    // Vulkan allows eVariableDescriptorCount on one binding, and requires it to be the highest in the set.
    EXPECT_EQ(VariableBinding(SetId::index_buffers), 0u);
    for (const SetSpec& set : kSceneSets) {
        const std::uint32_t variable = VariableBinding(set.id);
        if (variable == kNoBinding) continue;
        EXPECT_EQ(set.id, SetId::index_buffers) << set.name;
        EXPECT_EQ(variable + 1, BindingCount(set.id)) << set.name;
    }
}

TEST(SceneTablesTest, PoolCountsDeriveFromTheRows) {
    constexpr std::uint32_t blocks = 8;
    constexpr std::uint32_t frames = 3;

    // occlusion: 4 block arrays and 7 singles per set, one combined image sampler, three sets in flight.
    const PoolCounts occlusion = PoolCountsFor(SetId::occlusion, blocks, frames);
    EXPECT_EQ(occlusion.max_sets, frames);
    EXPECT_EQ(occlusion.storage_buffers, 39u * frames);
    EXPECT_EQ(occlusion.combined_image_samplers, frames);
    // The occlusion set has no sampled image binding of its own; the Hi-Z image is bound as a combined image sampler.
    // A pool that asked for sampled images here would be reserving descriptors nothing can allocate.
    EXPECT_EQ(occlusion.sampled_images, 0u);
    EXPECT_EQ(occlusion.storage_images, 0u);
    EXPECT_EQ(occlusion.samplers, 0u);

    // hiz: no storage buffers; the storage-image array is kMaxHiZMips wide.
    const PoolCounts hiz = PoolCountsFor(SetId::hiz, blocks, frames);
    EXPECT_EQ(hiz.storage_buffers, 0u);
    EXPECT_EQ(hiz.sampled_images, frames);
    EXPECT_EQ(hiz.samplers, frames);
    EXPECT_EQ(hiz.storage_images, kMaxHiZMips * frames);

    // collect-write: two storage buffers per set, no spare sets.
    const PoolCounts write = PoolCountsFor(SetId::collect_write, blocks, frames);
    EXPECT_EQ(write.max_sets, frames);
    EXPECT_EQ(write.storage_buffers, 2u * frames);

    // Lighting is a single set, not a ring.
    const PoolCounts lighting = PoolCountsFor(SetId::scene_uniform, blocks, frames);
    EXPECT_EQ(lighting.max_sets, 1u);
    EXPECT_EQ(lighting.storage_buffers, 1u + kMaxLightBlocks);

    // The empty set has no descriptor types at all; only its set count matters.
    const PoolCounts empty = PoolCountsFor(SetId::empty, blocks, frames);
    EXPECT_EQ(empty.max_sets, frames);
    EXPECT_EQ(empty.storage_buffers, 0u);
    EXPECT_EQ(empty.combined_image_samplers, 0u);
}

TEST(SceneTablesTest, FrameBuffersCoverBothModes) {
    std::array<bool, kBufferSlotCount> tabled{};
    for (const BufferSpec& spec : kFrameBuffers) {
        const auto slot = static_cast<std::size_t>(spec.slot);
        ASSERT_LT(slot, tabled.size());
        EXPECT_FALSE(tabled[slot]) << "buffer slot " << slot << " appears twice";
        tabled[slot] = true;

        // A slot has to exist in at least one mode, and each mode that has it must name its usage and memory.
        EXPECT_TRUE(spec.cid.present || spec.mid.present)
            << "buffer slot " << slot << " is absent in both modes";
        if (spec.cid.present) {
            EXPECT_TRUE(static_cast<bool>(spec.cid.flags.usage)) << "cid slot " << slot;
        }
        if (spec.mid.present) {
            EXPECT_TRUE(static_cast<bool>(spec.mid.flags.usage)) << "mid slot " << slot;
        }
        // A dummy is a fixed size, so it must also stay below the floor the real buffer would never go under.
        if (spec.cid.fixed_bytes != 0) {
            EXPECT_GE(spec.cid.fixed_bytes, 4u) << "cid slot " << slot;
        }
        if (spec.mid.fixed_bytes != 0) {
            EXPECT_GE(spec.mid.fixed_bytes, 4u) << "mid slot " << slot;
        }
    }
    for (std::size_t i = 0; i < tabled.size(); ++i) {
        const bool singleton = (i == static_cast<std::size_t>(BufferSlot::technique_flags) ||
                                i == static_cast<std::size_t>(BufferSlot::scene_header));
        EXPECT_EQ(tabled[i], !singleton) << "buffer slot " << i;
    }
}

TEST(SceneTablesTest, BindingSourcesResolveToABuffer) {
    // Every descriptor that names a buffer must also have a row for that slot,
    // in at least one mode. Catching this here beats a null vkBuffer on frame one.
    const auto present_in_some_mode = [](const BufferSlot slot) {
        for (const BufferSpec& spec : kFrameBuffers) {
            if (spec.slot == slot) return spec.cid.present || spec.mid.present;
        }
        for (const SingletonBufferSpec& spec : kSingletonBuffers) {
            if (spec.slot == slot) return true;
        }
        return false;
    };

    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.source == SourceKind::buffer) {
                EXPECT_TRUE(present_in_some_mode(b.buffer_slot))
                    << set.name << " binding " << b.binding << " names an untabled buffer";
                // A CID-only binding must bind a buffer CID actually allocates.
                if (b.mode == BindingMode::cid_only) {
                    bool cid = false;
                    for (const BufferSpec& spec : kFrameBuffers) {
                        if (spec.slot == b.buffer_slot) cid = spec.cid.present;
                    }
                    EXPECT_TRUE(cid) << set.name << " binding " << b.binding;
                }
            }
            if (b.source == SourceKind::mode_buffer) {
                EXPECT_NE(b.buffer_slot, b.buffer_slot_mid)
                    << set.name << " binding " << b.binding
                    << " picks the same buffer in both modes";
                EXPECT_TRUE(present_in_some_mode(b.buffer_slot));
                EXPECT_TRUE(present_in_some_mode(b.buffer_slot_mid));
            }
        }
    }
}

TEST(SceneTablesTest, IndexBuffersCarryTheIndexUsageInTheModeThatBindsThem) {
    // draw_indices is the MID index buffer (absolute vertex indices); CID binds the compacted main/depth/occluder lists and only writes draw_indices through a storage descriptor.
    // A missing index bit is VUID-vkCmdBindIndexBuffer-buffer-usage on every depth, occluder and main draw of that mode, which validation reports but a passing readback does not.
    const auto row = [](const BufferSlot slot) -> const BufferSpec* {
        for (const BufferSpec& spec : kFrameBuffers) {
            if (spec.slot == slot) return &spec;
        }
        return nullptr;
    };

    const BufferSpec* draw = row(BufferSlot::draw_indices);
    ASSERT_NE(draw, nullptr);
    EXPECT_TRUE(HasUsage(draw->mid.flags.usage, vk::BufferUsageFlagBits::eIndexBuffer))
        << "the MID draw_indices buffer is bound with vkCmdBindIndexBuffer";
    EXPECT_FALSE(HasUsage(draw->cid.flags.usage, vk::BufferUsageFlagBits::eIndexBuffer))
        << "the CID draw_indices buffer is only ever a storage descriptor";

    // The CID compact lists are index buffers. In MID they are 4 B dummies or absent, and a dummy is never bound.
    for (const BufferSlot slot : {BufferSlot::main_indices, BufferSlot::depth_indices,
                                  BufferSlot::occluder_indices}) {
        const BufferSpec* spec = row(slot);
        ASSERT_NE(spec, nullptr);
        ASSERT_TRUE(spec->cid.present);
        EXPECT_TRUE(HasUsage(spec->cid.flags.usage, vk::BufferUsageFlagBits::eIndexBuffer))
            << "cid slot " << int(slot);
        if (spec->mid.fixed_bytes == 0) {
            continue;   // real MID buffer: none of these are ever bound there
        }
        EXPECT_FALSE(HasUsage(spec->mid.flags.usage, vk::BufferUsageFlagBits::eIndexBuffer))
            << "a placeholder is never bound as an index buffer";
    }
}

TEST(SceneTablesTest, SingletonBuffersCompleteTheSlotSet) {
    EXPECT_EQ(kSingletonBuffers.size(), 2u);
    bool flags = false;
    bool header = false;
    for (const SingletonBufferSpec& spec : kSingletonBuffers) {
        if (spec.slot == BufferSlot::technique_flags) flags = true;
        if (spec.slot == BufferSlot::scene_header) header = true;
    }
    EXPECT_TRUE(flags);
    EXPECT_TRUE(header);
    EXPECT_EQ(kFrameBuffers.size() + kSingletonBuffers.size(), kBufferSlotCount);
}

TEST(SceneTablesTest, SizeFloorsAreOneByteOrARecord) {
    // The row is the only owner of a size floor: ComputeSize returns the raw formula, so a row that carries a floor has to be a record size the formula can under-shoot.
    // Every other row keeps the default floor of 1, which is all a zero-capacity scene needs to stay off a zero-byte allocation.
    for (const BufferSpec& spec : kFrameBuffers) {
        EXPECT_TRUE(spec.size.min_bytes == 1u ||
                    spec.size.min_bytes == kIndirectCommandBytes ||
                    spec.size.min_bytes == kTechniqueResultFloor)
            << "buffer slot " << int(spec.slot) << " has floor " << spec.size.min_bytes;
    }
    for (const SingletonBufferSpec& spec : kSingletonBuffers) {
        EXPECT_EQ(spec.size.min_bytes, 1u) << "singleton slot " << int(spec.slot);
    }
}

TEST(SceneTablesTest, EveryWriterValueIsUsed) {
    // A writer value no row names is a distinction nothing acts on: the frame loop filters on exactly these two, so both have to mean something.
    std::uint32_t in_frame_loop = 0;
    std::uint32_t out_of_band = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.writer == Writer::prepare_compute) ++in_frame_loop;
            if (b.writer == Writer::external) ++out_of_band;
        }
    }
    EXPECT_GT(in_frame_loop, 0u);
    EXPECT_GT(out_of_band, 0u);
}

TEST(SceneTablesTest, ExactlyOneSetIsNotRinged) {
    // `fixed_sets` sizes a pool that is allocated once instead of once per frame in flight.
    // A second such row would need the renderer to tell the two apart, and a field that is zero on every row is dead weight.
    std::uint32_t fixed_rows = 0;
    for (const SetSpec& set : kSceneSets) {
        if (set.fixed_sets == 0) continue;
        ++fixed_rows;
        EXPECT_EQ(set.id, SetId::scene_uniform) << set.name;
        EXPECT_EQ(set.fixed_sets, 1u) << set.name;
    }
    EXPECT_EQ(fixed_rows, 1u);
}

// ── Derivations the rows owe the renderer ──
//
// These moved here with the rows: the descriptor budget, the per-stage fit and the block clamp are all folded out of kSceneSets, so they are table tests, not capacity-constant tests.

TEST(SceneTablesTest, SetStorageBufferCountsMatchTheirLayouts) {
    constexpr std::uint32_t blocks = 8;

    // Literal expectations, not a copy of the row formula: a set that silently loses a block array changes the production count but not the expectation, so the mismatch is caught here.
    // submesh-vertex: 1 block array + 0 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::submesh_vertex, blocks), 8u);
    // expand: 5 block arrays + 3 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::expand, blocks), 43u);
    // occlusion: 4 block arrays + 7 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::occlusion, blocks), 39u);
    // occluder-select: 3 block arrays + 8 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::occluder_select, blocks), 32u);
    // collect: 1 block array + 5 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::collect, blocks), 13u);
    // deform: 1 block array + two skin tables + palette + output + parameters.
    EXPECT_EQ(StorageBuffersForSet(SetId::deform, blocks), 8u + 131u);
    // vertex-buffers: the vertex table, the UV table and the deformed output.
    EXPECT_EQ(StorageBuffersForSet(SetId::vertex_buffers, blocks), 64u + 64u + 1u);
    // index-buffers: the bindless index table.
    EXPECT_EQ(StorageBuffersForSet(SetId::index_buffers, blocks), 64u);
    // indirection: one buffer.
    EXPECT_EQ(StorageBuffersForSet(SetId::indirection, blocks), 1u);
    // collect-write: results + technique commands.
    EXPECT_EQ(StorageBuffersForSet(SetId::collect_write, blocks), 2u);
    // lighting: the scene header plus the light block table.
    EXPECT_EQ(StorageBuffersForSet(SetId::scene_uniform, blocks), 1u + 16u);
    // No storage buffers at all.
    EXPECT_EQ(StorageBuffersForSet(SetId::hiz, blocks), 0u);
    EXPECT_EQ(StorageBuffersForSet(SetId::empty, blocks), 0u);
}

TEST(SceneTablesTest, FixedTablesAreSizedByTheirRows) {
    // The deform set's fixed part: two fully-populated skin arrays (each array element is its own descriptor in the pool) plus the flat palette, the deformed-vertex output and the flat modifier parameters.
    const StorageFit deform = StorageFitOf(SetId::deform);
    EXPECT_EQ(deform.blocks, 1u);
    EXPECT_EQ(deform.fixed, 2u * 64u + 3u);
    // The expand set carries a fifth block array (the SkinEntry table) at binding 7.
    const StorageFit expand = StorageFitOf(SetId::expand);
    EXPECT_EQ(expand.blocks, 5u);
    EXPECT_EQ(expand.fixed, 3u);
}

TEST(SceneTablesTest, DepthGraphicsStageSumsTheThreeSetsItBinds) {
    // The depth/main pipeline binds empty, submesh-vertex, vertex-buffers and indirection on one stage.
    // The per-stage limit applies to their sum, and the sum must be derived from the rows (it used to be restated by hand and was one buffer short: it omitted the indirection set).
    const StorageFit stage = DepthGraphicsStageFit();
    EXPECT_EQ(stage.blocks, 1u);
    EXPECT_EQ(stage.fixed, 64u + 64u + 1u + 1u);
    EXPECT_EQ(stage.blocks * 8u + stage.fixed, 8u + 129u + 1u);
}

TEST(SceneTablesTest, WidestSetBoundsThePerSetTotal) {
    // The deform row is constant and large, so it is the worst case on a small block budget; expand's five block arrays overtake it once the budget passes (131 - 3) / 5 blocks.
    // Both rows matter and neither may be dropped.
    EXPECT_EQ(MaxStorageBuffersPerSet(1u), 132u);      // deform
    EXPECT_EQ(MaxStorageBuffersPerSet(8u), 139u);      // deform
    EXPECT_EQ(MaxStorageBuffersPerSet(256u), 1283u);   // expand: 5 * 256 + 3
    EXPECT_EQ(MaxStorageBuffersPerSet(4096u), 20483u); // expand: 5 * 4096 + 3
}

TEST(SceneTablesTest, HugeLimitsLeaveTheCeilingInPlace) {
    const DeviceLimits limits{1u << 20, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), kMaxBlocksCeiling);
}

TEST(SceneTablesTest, PerSetLimitClampsTheBudget) {
    // expand needs 5B + 3, so a per-set limit of 4*100 + 4 leaves (404 - 3) / 5 = 80 blocks.
    // This is tighter than occlusion's 4B + 4, which would allow 100: the set with the most block bindings binds first.
    const DeviceLimits limits{4u * 100u + 4u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 80u);
}

TEST(SceneTablesTest, PerStageLimitClampsTheBudget) {
    // Same derivation through maxPerStageDescriptorStorageBuffers.
    const DeviceLimits limits{1u << 20, 4u * 50u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(limits), (204u - 3u) / 5u);
}

TEST(SceneTablesTest, SkinTablesClampTheBlockBudget) {
    // The deform set's fixed part is 131 storage buffers.
    // With a limit of 131 + 8 the depth-graphics-stage sum (130 fixed) allows 9 blocks and deform allows 8, so 8 is the answer; neither may be ignored just because it is constant.
    const DeviceLimits limits{131u + 8u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 8u);
}

TEST(SceneTablesTest, NeverReturnsZeroBlocks) {
    // A device that cannot fit the fixed bindings still gets one block so the renderer can start and report the condition.
    // A tiny but nonzero limit takes the same floor, so no separate "small device" case is needed.
    const DeviceLimits none{0, 0};
    EXPECT_EQ(MaxBlocksForDevice(none), 1u);

    const DeviceLimits tiny{4u + 4u, 4u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(tiny), 1u);

    // Exactly the deform set's fixed part is still not enough to fit one block.
    const DeviceLimits deform_only{131u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(deform_only), 1u);
}

TEST(SceneTablesTest, EverySetHasExactlyOneRow) {
    // One row per SceneRenderer-owned descriptor set.
    // A set that gains bindings without a row would silently consume update-after-bind budget that BindlessManager::ComputeBindlessCapacity does not know about.
    EXPECT_EQ(kSceneSets.size(), kSetCount);
    for (std::size_t i = 0; i < kSceneSets.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kSceneSets[i].id), i)
            << "row " << i << " is out of SetId order: " << kSceneSets[i].name;
        for (std::size_t j = 0; j < i; ++j) {
            EXPECT_STRNE(kSceneSets[i].name.data(), kSceneSets[j].name.data())
                << "duplicate set name " << kSceneSets[i].name;
        }
    }
}

TEST(SceneTablesTest, UploadPathBindingsMatchTheirShaders) {
    // The upload paths look a binding up by kind and position, and the shaders hard-code the number, so the pairing is pinned here.
    // A renumbering that keeps both sides in step passes; one that only touches the table fails.
    EXPECT_EQ(BindingOfKind(SetId::vertex_buffers, vk::DescriptorType::eStorageBuffer, 0), 0u);
    EXPECT_EQ(BindingOfKind(SetId::vertex_buffers, vk::DescriptorType::eStorageBuffer, 1), 1u);
    EXPECT_EQ(BindingOfKind(SetId::index_buffers, vk::DescriptorType::eStorageBuffer, 0), 0u);
    EXPECT_EQ(BindingOfKind(SetId::deform, vk::DescriptorType::eStorageBuffer, 0), 0u);
    EXPECT_EQ(BindingOfKind(SetId::deform, vk::DescriptorType::eStorageBuffer, 1), 1u);
    EXPECT_EQ(BindingOfKind(SetId::scene_uniform, vk::DescriptorType::eStorageBuffer, 1), 1u);
}
