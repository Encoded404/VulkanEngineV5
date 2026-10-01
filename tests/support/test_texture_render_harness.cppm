module;

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

export module TestSupport.TextureRenderHarness;

import std;

import vulkan_hpp;

import ShaderReflection;
import VulkanBackend.Vulkan.VulkanCapabilities;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.GpuResources.GpuImageHeap;
import VulkanEngine.GpuTexture;
import TestSupport.HeadlessVulkanBackend;

// Offscreen graphics renderer for the compressed-format sampling tests.
//
// It draws a full-screen triangle with a test vertex shader and the test sample
// fragment shader, which returns the raw RGBA it samples from bindless slot 0.
// Textures are uploaded through the production GpuTexture::CreateFromTextureData
// path, so the real subresource copies and barriers are exercised; the test then
// compares two renders channel by channel with nothing else in the way.
//
// The bindless descriptor layout is declared here rather than through
// BaseTechnique: a test must not depend on the scene manager.
export namespace TestSupport {

struct TextureRenderHarnessConfig {
    std::uint32_t width = 64;
    std::uint32_t height = 64;
    // Shader ids for the test vertex shader and the raw-sampling fragment
    // shader; the caller owns the ShaderManager, so the harness never compiles
    // or reloads shaders.
    VulkanEngine::ShaderSystem::ShaderId vertex_shader{};
    VulkanEngine::ShaderSystem::ShaderId fragment_shader{};
};

// Registers the test-only shaders with a ShaderManager through RegisterManual
// (they have no generated Slang module). `shader_dir` holds the compiled
// test_fullscreen_vs.spv and test_sample_frag.spv.
struct HarnessShaderIds {
    VulkanEngine::ShaderSystem::ShaderId vertex{};
    VulkanEngine::ShaderSystem::ShaderId sample_fragment{};
};

class TextureRenderHarness {
public:
    TextureRenderHarness() = default;
    ~TextureRenderHarness();

    TextureRenderHarness(const TextureRenderHarness&) = delete;
    TextureRenderHarness& operator=(const TextureRenderHarness&) = delete;

    bool Initialize(TestSupport::HeadlessVulkanBackend& backend,
                    VulkanEngine::ShaderSystem::ShaderManager& shaders,
                    const TextureRenderHarnessConfig& config);
    void Shutdown();

    // Draws one frame sampling `albedo` at bindless slot 0, then reads the color
    // image back as RGBA8 (width * height * 4). Empty if `albedo` is invalid.
    std::vector<std::uint8_t> RenderAndReadBack(const VulkanEngine::GpuResources::GpuTexture& albedo);

private:
    bool CreateDescriptorLayouts();
    bool CreatePipeline();
    void WriteDescriptors(const VulkanEngine::GpuResources::GpuTexture& albedo);

    TestSupport::HeadlessVulkanBackend* backend_ = nullptr;
    VulkanEngine::ShaderSystem::ShaderManager* shaders_ = nullptr;
    TextureRenderHarnessConfig config_{};

    std::unique_ptr<vk::raii::DescriptorSetLayout> bindless_set0_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> pool_{};
    std::unique_ptr<vk::raii::DescriptorSet> bindless_set0_{};

    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_{};
    // PipelineProduct (not a bare Pipeline) keeps GPL library pipelines alive.
    std::unique_ptr<VulkanEngine::ShaderSystem::PipelineProduct> pipeline_{};
};

[[nodiscard]] HarnessShaderIds RegisterHarnessShaders(
    VulkanEngine::ShaderSystem::ShaderManager& shaders, std::string_view shader_dir);

}  // namespace TestSupport
