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

}  // namespace
