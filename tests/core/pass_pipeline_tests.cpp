#include <gtest/gtest.h>

#include <glm/glm.hpp>

import std;

import vulkan_hpp;
import VulkanEngine.RenderPipeline;

namespace {

using namespace VulkanEngine::PipelinePass;

// A pass that declares an engine-owned compute pipeline, push constants, and an
// app descriptor binding, so the declaration plumbing can be inspected.
class DeclaringComputePass final : public IPipelinePass {
public:
    [[nodiscard]] std::string_view GetName() const override { return "declaring-compute"; }

    void Setup(PassSetupContext& ctx) override {
        ctx.RequestComputePipeline(/*compute_shader=*/42);

        VulkanEngine::Render::DescriptorDecl binding{};
        binding.set = 5;
        binding.binding = 0;
        binding.kind = VulkanEngine::Render::DescriptorKind::StorageImage;
        binding.descriptor_type = vk::DescriptorType::eStorageImage;
        binding.stage_flags = vk::ShaderStageFlagBits::eCompute;
        binding.count = 1;
        ctx.DeclareBindings({binding});

        ctx.DeclarePushConstants<glm::vec4>(vk::ShaderStageFlagBits::eCompute);

        const auto target = ctx.CreateTransientImage(TransientImageDesc{
            .name = "declaring-target",
            .format = vk::Format::eR8G8B8A8Unorm,
            .width = 320,
            .height = 240,
        });
        ctx.AddWrite(target);
    }

    void Execute(const FrameContext&, vk::CommandBuffer) override {}
};

class DeclaringGraphicsPass final : public IPipelinePass {
public:
    [[nodiscard]] std::string_view GetName() const override { return "declaring-graphics"; }

    void Setup(PassSetupContext& ctx) override {
        ctx.RequestGraphicsPipeline(/*vertex=*/7, /*fragment=*/9,
                                    {vk::Format::eR8G8B8A8Unorm}, vk::Format::eD32Sfloat);
        const auto target = ctx.CreateTransientImage(TransientImageDesc{
            .name = "graphics-target",
            .format = vk::Format::eR8G8B8A8Unorm,
            .width = 128,
            .height = 128,
        });
        ctx.AddWrite(target);
    }

    void Execute(const FrameContext&, vk::CommandBuffer) override {}
};

TEST(PassPipelineTest, ComputeRequestReachesPipelineDeclaration) {
    VulkanEngine::RenderPipeline::RenderPipeline pipeline;

    const auto handle = pipeline.RegisterPass(std::make_unique<DeclaringComputePass>());
    ASSERT_TRUE(handle.has_value());

    const auto* request = pipeline.GetPassPipelineRequest(*handle);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->kind, PassPipelineKind::Compute);
    EXPECT_EQ(request->compute_shader, 42u);
    EXPECT_EQ(request->push_constant_size, sizeof(glm::vec4));
    EXPECT_TRUE((request->push_constant_stages & vk::ShaderStageFlagBits::eCompute) !=
                vk::ShaderStageFlags{});

    // No device/shader manager was supplied, so nothing is built (and nothing
    // crashes trying).
    EXPECT_EQ(pipeline.GetPassPipelineLayout(*handle), nullptr);
    EXPECT_EQ(pipeline.GetPassPipelineLayoutByName("declaring-compute"), nullptr);
}

TEST(PassPipelineTest, GraphicsRequestReachesPipelineDeclaration) {
    VulkanEngine::RenderPipeline::RenderPipeline pipeline;

    const auto handle = pipeline.RegisterPass(std::make_unique<DeclaringGraphicsPass>());
    ASSERT_TRUE(handle.has_value());

    const auto* request = pipeline.GetPassPipelineRequest(*handle);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->kind, PassPipelineKind::Graphics);
    EXPECT_EQ(request->vertex_shader, 7u);
    EXPECT_EQ(request->fragment_shader, 9u);
    EXPECT_EQ(request->depth_format, vk::Format::eD32Sfloat);
    ASSERT_EQ(request->color_formats.size(), 1u);
    EXPECT_EQ(request->color_formats.front(), vk::Format::eR8G8B8A8Unorm);
}

TEST(PassPipelineTest, UndeclaredPassHasNoPipelineRequest) {
    class PlainPass final : public IPipelinePass {
    public:
        [[nodiscard]] std::string_view GetName() const override { return "plain"; }
        void Setup(PassSetupContext&) override {}
        void Execute(const FrameContext&, vk::CommandBuffer) override {}
    };

    VulkanEngine::RenderPipeline::RenderPipeline pipeline;
    const auto handle = pipeline.RegisterPass(std::make_unique<PlainPass>());
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ(pipeline.GetPassPipelineRequest(*handle), nullptr);
}

// A declared binding may be associated with a graph resource; binding an
// undeclared (set, binding) or a reserved engine set is a structured error.
TEST(PassPipelineTest, BindResourceValidatesAgainstDeclarations) {
    class BoundPass final : public IPipelinePass {
    public:
        BoundPass(std::string name, std::uint32_t set, std::uint32_t binding, bool declare)
            : name_(std::move(name)), set_(set), binding_(binding), declare_(declare) {}

        [[nodiscard]] std::string_view GetName() const override { return name_; }

        void Setup(PassSetupContext& ctx) override {
            const auto target = ctx.CreateTransientBuffer(TransientBufferDesc{
                .name = "bound-buffer", .size = 256});
            VulkanEngine::Render::DescriptorDecl binding{};
            binding.set = set_;
            binding.binding = binding_;
            binding.kind = VulkanEngine::Render::DescriptorKind::Shared;
            binding.descriptor_type = vk::DescriptorType::eStorageBuffer;
            binding.stage_flags = vk::ShaderStageFlagBits::eCompute;
            if (declare_) {
                ctx.DeclareBindings({binding});
            }
            ctx.BindResource(set_, binding_, target);
            ctx.RequestComputePipeline(11);
        }

        void Execute(const FrameContext&, vk::CommandBuffer) override {}

    private:
        std::string name_;
        std::uint32_t set_;
        std::uint32_t binding_;
        bool declare_;
    };

    VulkanEngine::RenderPipeline::RenderPipeline pipeline;
    EXPECT_TRUE(pipeline.RegisterPass(std::make_unique<BoundPass>("bound-ok", 5, 0, true)).has_value());

    const auto undeclared = pipeline.RegisterPass(std::make_unique<BoundPass>("bound-undeclared", 5, 3, false));
    ASSERT_FALSE(undeclared.has_value());
    EXPECT_EQ(undeclared.error().code, VulkanEngine::RenderPipeline::PassErrorCode::InvalidDeclaration);

    const auto reserved = pipeline.RegisterPass(std::make_unique<BoundPass>("bound-reserved", 0, 0, true));
    ASSERT_FALSE(reserved.has_value());
    EXPECT_EQ(reserved.error().code, VulkanEngine::RenderPipeline::PassErrorCode::InvalidDeclaration);
}

}  // namespace
