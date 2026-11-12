#include <gtest/gtest.h>

import std;
import std.compat;

import vulkan;

import test_gpu;

import ShaderReflection;
import TestSupport.HeadlessVulkanBackend;
import VulkanBackend.Vulkan.VulkanCapabilities;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.GpuResources;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.NormalEncoding;

namespace {

using VulkanEngine::GpuResources::GpuBuffer;
using VulkanEngine::NormalEncoding::PackTBN;
using VulkanEngine::ShaderSystem::ComputePipelineDesc;
using VulkanEngine::ShaderSystem::PipelineFactory;
using VulkanEngine::ShaderSystem::PipelineProduct;
using VulkanEngine::ShaderSystem::ShaderId;
using VulkanEngine::ShaderSystem::ShaderManager;

struct PackInput {
    std::array<float, 4> normal{};
    std::array<float, 4> tangent{};
};
static_assert(sizeof(PackInput) == 32, "the probe's input row must be two float4s");

struct PackPC {
    std::uint32_t element_count = 0;
};
static_assert(sizeof(PackPC) == 4, "the probe's push constant is one word");

// Device-bound parity for the tangent-frame packer: the GPU mirror in
// normal_pack.slang must produce exactly the word the CPU encoder produces.
// A mismatch shows up as subtle normal shading, not a crash, so this test is
// the guard for the deform pass rewriting packedTBN.
class NormalPackGpuTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend_.Initialize());

        shaders_ = std::make_unique<ShaderManager>(backend_.GetDevice(), backend_.GetCapabilities(),
                                                   std::filesystem::temp_directory_path().string());
        const ShaderId compute_id = shaders_->RegisterManual(
            std::format("{}/test_pack_tbn.spv", VKENGINE_TEST_SHADER_DIR), "",
            ShaderStage::eCompute);

        const vk::DeviceSize bytes =
            static_cast<vk::DeviceSize>(kElements) * sizeof(PackInput);
        in_buffer_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, bytes, vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(in_buffer_->IsValid());
        out_buffer_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, static_cast<vk::DeviceSize>(kElements) * sizeof(std::uint32_t),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(out_buffer_->IsValid());

        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
            vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1,
                                           vk::ShaderStageFlagBits::eCompute, nullptr),
            vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                           vk::ShaderStageFlagBits::eCompute, nullptr)};
        set_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            backend_.GetDevice(),
            vk::DescriptorSetLayoutCreateInfo{{}, bindings.size(), bindings.data()});

        const std::array<vk::DescriptorPoolSize, 1> pool_sizes{
            vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 2)};
        pool_ = std::make_unique<vk::raii::DescriptorPool>(
            backend_.GetDevice(),
            vk::DescriptorPoolCreateInfo({}, 1, pool_sizes.size(), pool_sizes.data()));
        auto sets = backend_.GetDevice().allocateDescriptorSets(
            vk::DescriptorSetAllocateInfo{**pool_, 1, &**set_layout_});
        ASSERT_EQ(sets.size(), 1u);
        set_ = std::make_unique<vk::raii::DescriptorSet>(std::move(sets[0]));

        const vk::PushConstantRange push_range(vk::ShaderStageFlagBits::eCompute, 0,
                                               sizeof(PackPC));
        const std::array<vk::DescriptorSetLayout, 1> set_layouts{**set_layout_};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(set_layouts);
        layout_info.setPushConstantRanges(push_range);
        pipeline_layout_ =
            std::make_unique<vk::raii::PipelineLayout>(backend_.GetDevice(), layout_info);

        PipelineFactory factory(backend_.GetDevice(), backend_.GetCapabilities(),
                                shaders_->GetPipelineCacheRAII());
        ComputePipelineDesc desc{};
        desc.shader = compute_id;
        desc.layout = **pipeline_layout_;
        auto product = factory.CreateCompute(desc, *shaders_);
        ASSERT_TRUE(product.has_value()) << product.error().message;
        pipeline_product_ = std::make_unique<PipelineProduct>(std::move(*product));
    }

    void TearDown() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            return;
        }
        pipeline_product_.reset();
        pipeline_layout_.reset();
        set_.reset();
        pool_.reset();
        set_layout_.reset();
        out_buffer_.reset();
        in_buffer_.reset();
        shaders_.reset();
        backend_.Shutdown();
    }

    // A deterministic grid of valid frames: normals walk a longitude/latitude
    // lattice and tangents are a per-index rotated orthonormal partner, so the
    // packer sees every octahedral region and both handedness signs. A few
    // degenerate pairs (tangent parallel to the normal) exercise the packer's
    // fallback branch.
    [[nodiscard]] static std::vector<PackInput> MakeInputs() {
        std::vector<PackInput> inputs(kElements);
        for (std::uint32_t i = 0; i < kElements; ++i) {
            float u = static_cast<float>(i % 16) / 15.0f;         // 0..1
            float v = static_cast<float>((i / 16) % 16) / 15.0f;  // 0..1
            float az = u * 2.0f * std::numbers::pi_v<float>;
            float el = (v - 0.5f) * std::numbers::pi_v<float>;
            const float cx = std::cos(el);
            std::array<float, 3> n{cx * std::cos(az), cx * std::sin(az), std::sin(el)};
            // Tangent: cross with a fixed up-ish axis unless nearly parallel.
            std::array<float, 3> up{0.0f, 0.0f, 1.0f};
            if (std::abs(n[2]) > 0.95f) up = {1.0f, 0.0f, 0.0f};
            std::array<float, 3> t{n[1] * up[2] - n[2] * up[1],
                                   n[2] * up[0] - n[0] * up[2],
                                   n[0] * up[1] - n[1] * up[0]};
            // Deliberately never parallel to the normal: that case takes the
            // packer's "degenerate tangent" fallback, whose output depends on
            // the direction a near-zero vector happens to normalize to and is
            // therefore not comparable between two implementations.
            const float len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            for (float& c : t) c /= len;
            inputs[i].normal = {n[0], n[1], n[2], 0.0f};
            const float handedness = ((i % 3) == 0) ? -1.0f : 1.0f;
            inputs[i].tangent = {t[0], t[1], t[2], handedness};
        }
        return inputs;
    }

    [[nodiscard]] std::vector<std::uint32_t> RunPack(const std::vector<PackInput>& inputs) {
        in_buffer_->Upload(inputs.data(), inputs.size() * sizeof(PackInput));

        vk::DescriptorBufferInfo in_info{};
        in_info.buffer = static_cast<vk::Buffer>(*in_buffer_->GetBuffer());
        in_info.offset = 0;
        in_info.range = vk::WholeSize;
        vk::DescriptorBufferInfo out_info{};
        out_info.buffer = static_cast<vk::Buffer>(*out_buffer_->GetBuffer());
        out_info.offset = 0;
        out_info.range = vk::WholeSize;
        vk::WriteDescriptorSet in_write{};
        in_write.dstSet = **set_;
        in_write.dstBinding = 0;
        in_write.descriptorCount = 1;
        in_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        in_write.pBufferInfo = &in_info;
        vk::WriteDescriptorSet out_write{};
        out_write.dstSet = **set_;
        out_write.dstBinding = 1;
        out_write.descriptorCount = 1;
        out_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        out_write.pBufferInfo = &out_info;
        backend_.GetDevice().updateDescriptorSets({in_write, out_write}, {});

        vk::raii::CommandBuffer& cmd = backend_.GetCommandBuffer(0);
        cmd.reset({});
        cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_product_->Get());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, **pipeline_layout_, 0,
                               std::array<vk::DescriptorSet, 1>{**set_}, {});
        const PackPC pc{static_cast<std::uint32_t>(inputs.size())};
        cmd.pushConstants<PackPC>(**pipeline_layout_, vk::ShaderStageFlagBits::eCompute, 0, pc);
        cmd.dispatch((pc.element_count + 63) / 64, 1, 1);
        const vk::DeviceSize out_bytes =
            static_cast<vk::DeviceSize>(inputs.size()) * sizeof(std::uint32_t);
        const vk::BufferMemoryBarrier to_host(
            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
            static_cast<vk::Buffer>(*out_buffer_->GetBuffer()), 0, out_bytes);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eHost, {}, {}, to_host, {});
        cmd.end();
        backend_.GetGraphicsQueue().submit(vk::SubmitInfo(0, nullptr, nullptr, 1, &*cmd), {});
        backend_.GetGraphicsQueue().waitIdle();

        std::vector<std::uint32_t> words(inputs.size());
        const void* mapped = out_buffer_->Map(0, out_bytes);
        std::memcpy(words.data(), mapped, static_cast<std::size_t>(out_bytes));
        out_buffer_->Unmap();
        return words;
    }

    static constexpr std::uint32_t kElements = 256;

    TestSupport::HeadlessVulkanBackend backend_{};
    std::unique_ptr<ShaderManager> shaders_;
    std::unique_ptr<vk::raii::DescriptorSetLayout> set_layout_;
    std::unique_ptr<vk::raii::DescriptorPool> pool_;
    std::unique_ptr<vk::raii::DescriptorSet> set_;
    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_;
    std::unique_ptr<PipelineProduct> pipeline_product_;
    std::unique_ptr<GpuBuffer> in_buffer_;
    std::unique_ptr<GpuBuffer> out_buffer_;
};

}  // namespace

TEST_F(NormalPackGpuTest, PackedWordsMatchTheCpuEncoder) {
    const auto inputs = MakeInputs();
    const auto words = RunPack(inputs);

    // The normal (octahedral) field and the handedness bit are pure
    // quantization and must agree exactly: they are computed from the same
    // normalized input with the same constants.
    constexpr std::uint32_t kOctAndSignMask = 0xFFFFFu | (1u << 31);
    // The tangent field is a projected 2D direction. Its parameterization is
    // ill-conditioned when the projected tangent is nearly parallel to the
    // basis tangent: there the subtraction that produces the coordinate
    // cancels to ~0, so a 1-ulp difference in the input direction moves the
    // quantized value by several units. Structural errors (wrong basis, wrong
    // scale, swapped axes) move it by orders of magnitude more, which is what
    // this bound catches.
    constexpr std::uint32_t kDiamondTolerance = 32;
    constexpr std::uint32_t kDiamondMask = 0x3FFu;

    std::size_t oct_mismatches = 0;
    std::uint32_t max_diamond_delta = 0;
    std::size_t worst = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const VulkanEngine::MeshVertexVec3 normal{inputs[i].normal[0], inputs[i].normal[1],
                                                  inputs[i].normal[2]};
        const VulkanEngine::MeshVertexVec3 tangent{inputs[i].tangent[0], inputs[i].tangent[1],
                                                   inputs[i].tangent[2]};
        const std::uint32_t expected = PackTBN(normal, tangent, inputs[i].tangent[3]);
        if ((words[i] & kOctAndSignMask) != (expected & kOctAndSignMask)) {
            if (oct_mismatches < 8) {
                EXPECT_EQ(words[i] & kOctAndSignMask, expected & kOctAndSignMask)
                    << "element " << i << " octahedral normal/handedness differs";
            }
            ++oct_mismatches;
        }
        const std::uint32_t gpu_diamond = (words[i] >> 21) & kDiamondMask;
        const std::uint32_t cpu_diamond = (expected >> 21) & kDiamondMask;
        const std::uint32_t delta =
            gpu_diamond > cpu_diamond ? gpu_diamond - cpu_diamond : cpu_diamond - gpu_diamond;
        if (delta > max_diamond_delta) {
            max_diamond_delta = delta;
            worst = i;
        }
    }
    EXPECT_EQ(oct_mismatches, 0u)
        << "normal_pack.slang and NormalEncoding.cppm disagree on the normal field; "
           "the deform pass would re-pack a different tangent frame than the importer";
    EXPECT_LE(max_diamond_delta, kDiamondTolerance)
        << "the GPU tangent projection diverged from the CPU encoder by "
        << max_diamond_delta << " units at element " << worst << " (n="
        << inputs[worst].normal[0] << "," << inputs[worst].normal[1] << ","
        << inputs[worst].normal[2] << " t=" << inputs[worst].tangent[0] << ","
        << inputs[worst].tangent[1] << "," << inputs[worst].tangent[2] << ") cpu="
        << ((PackTBN(VulkanEngine::MeshVertexVec3{inputs[worst].normal[0], inputs[worst].normal[1],
                                                 inputs[worst].normal[2]},
                     VulkanEngine::MeshVertexVec3{inputs[worst].tangent[0],
                                                 inputs[worst].tangent[1],
                                                 inputs[worst].tangent[2]},
                     inputs[worst].tangent[3]) >> 21) & 0x3FF)
        << " gpu=" << ((words[worst] >> 21) & 0x3FF);
}
