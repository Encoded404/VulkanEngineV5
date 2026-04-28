module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <array>
#include <numbers>
#include <cstring>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module App.DemoScene;

import App.FileLoaders.Mesh.BinMeshAssembler;
import App.FileLoaders.Mesh.GltfMeshAssembler;
import VulkanEngine.Mesh.MeshTypes;

namespace App::DemoScene {

struct DemoSceneManager::RawResources {
    std::unique_ptr<vk::raii::Buffer> vertex_buffer{};
    std::unique_ptr<vk::raii::DeviceMemory> vertex_memory{};
    std::unique_ptr<vk::raii::Buffer> index_buffer{};
    std::unique_ptr<vk::raii::DeviceMemory> index_memory{};
    std::unique_ptr<vk::raii::Image> texture_image{};
    std::unique_ptr<vk::raii::DeviceMemory> texture_memory{};
    std::unique_ptr<vk::raii::ImageView> texture_view{};
    std::unique_ptr<vk::raii::Sampler> texture_sampler{};
    std::unique_ptr<vk::raii::DescriptorSetLayout> descriptor_set_layout{};
    std::unique_ptr<vk::raii::DescriptorPool> descriptor_pool{};
    std::vector<vk::raii::DescriptorSet> descriptor_sets{};
    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout{};
    std::unique_ptr<vk::raii::Pipeline> pipeline{};
    uint32_t index_count = 0;
    bool depth_initialized = false;
    VkImage depth_image_handle = VK_NULL_HANDLE;
};

std::unique_ptr<DemoSceneManager::RawResources> DemoSceneManager::s_resources = nullptr;

namespace {
    [[nodiscard]] std::filesystem::path FindFirstFileWithExtension(const std::filesystem::path& dir,
                                                                   const std::vector<std::string>& extensions) {
        if (!std::filesystem::exists(dir)) {
            LOGIFACE_LOG(debug, "Directory does not exist: " + dir.string());
            return {};
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string ext = entry.path().extension().string();
            if (std::ranges::find(extensions, ext) != extensions.end()) {
                return entry.path();
            }
        }
        return {};
    }

    [[nodiscard]] std::vector<std::byte> ReadBinaryFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open model file: " + path.string());
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            throw std::runtime_error("Model file is empty: " + path.string());
        }

        std::vector<std::byte> bytes(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw std::runtime_error("Failed to read model file: " + path.string());
        }
        return bytes;
    }

    [[nodiscard]] uint32_t FindMemoryType(vk::PhysicalDevice physical_device, uint32_t type_filter, vk::MemoryPropertyFlags properties) {
        vk::PhysicalDeviceMemoryProperties const mem_properties = physical_device.getMemoryProperties();
        for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
            if ((type_filter & (1u << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("failed to find suitable memory type");
    }

    using Mat4 = std::array<float, 16>;

    struct PushConstants {
        Mat4 mvp{};
    };

    [[nodiscard]] Mat4 IdentityMatrix() {
        Mat4 m{};
        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
        return m;
    }

    [[nodiscard]] Mat4 Multiply(const Mat4& a, const Mat4& b) {
        Mat4 result{};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float value = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    value += a[k * 4 + row] * b[col * 4 + k];
                }
                result[col * 4 + row] = value;
            }
        }
        return result;
    }

    [[nodiscard]] Mat4 Translate(float x, float y, float z) {
        Mat4 m = IdentityMatrix();
        m[12] = x;
        m[13] = y;
        m[14] = z;
        return m;
    }

    [[nodiscard]] Mat4 RotateY(float radians) {
        Mat4 m = IdentityMatrix();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        m[0] = c;
        m[2] = s;
        m[8] = -s;
        m[10] = c;
        return m;
    }

    [[nodiscard]] Mat4 Perspective(float fov_y_radians, float aspect, float z_near, float z_far) {
        Mat4 m{};
        const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
        m[0] = f / aspect;
        m[5] = -f; // Vulkan clip space uses inverted Y for the conventional projection matrix.
        m[10] = z_far / (z_near - z_far);
        m[11] = -1.0f;
        m[14] = (z_far * z_near) / (z_near - z_far);
        return m;
    }

    [[nodiscard]] PushConstants BuildPushConstants(float angle_degrees, float aspect) {
        constexpr float pi = std::numbers::pi_v<float>;
        const float radians = angle_degrees * (pi / 180.0f);

        PushConstants constants{};
        const Mat4 model = RotateY(radians);
        const Mat4 view = Translate(0.0f, 0.0f, -3.0f);
        const Mat4 proj = Perspective(60.0f * (pi / 180.0f), aspect, 0.1f, 100.0f);
        constants.mvp = Multiply(proj, Multiply(view, model));
        return constants;
    }
}

MeshData DemoSceneManager::CreateFallbackQuad() {
    return MeshData{
        .positions = { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f },
        .normals = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
        .uvs = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f },
        .indices = { 0, 1, 2, 2, 3, 0 },
    };
}

MeshData DemoSceneManager::LoadMeshFromAssets(const std::filesystem::path& models_dir) {
    const auto bin_path = FindFirstFileWithExtension(models_dir, {".bin"});
    if (!bin_path.empty()) {
        try {
            const auto bytes = ReadBinaryFile(bin_path);
            auto assembler = std::make_shared<App::FileLoaders::Mesh::BinMeshAssembler>();
            auto mesh = assembler->AssembleFromFullBuffer(std::make_shared<std::vector<std::byte>>(bytes)).get();
            if (mesh) {
                MeshData data{};
                for (const auto& v : mesh->vertices) { data.positions.push_back(v.x); data.positions.push_back(v.y); data.positions.push_back(v.z); }
                for (const auto& n : mesh->normals) { data.normals.push_back(n.x); data.normals.push_back(n.y); data.normals.push_back(n.z); }
                for (const auto& uv : mesh->uvs) { data.uvs.push_back(uv.u); data.uvs.push_back(1.0f - uv.v); }
                data.indices = mesh->indices;
                return data;
            }
        } catch (const std::exception& e) {
            LOGIFACE_LOG(error, "got error in function LoadMeshFromAsset: " + std::string(e.what()));
        }
    }
    return CreateFallbackQuad();
}

VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
DemoSceneManager::LoadTexture(VulkanEngine::ResourceManager& resource_manager,
                              const std::filesystem::path& textures_dir,
                              const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback) {
    const auto texture_path = FindFirstFileWithExtension(textures_dir, {".ktx2"});
    if (texture_path.empty()) return fallback;
    auto handle = resource_manager.LoadFromFile<VulkanEngine::TextureResource>(texture_path);
    return (handle.IsValid() && handle->IsLoaded()) ? handle : fallback;
}

std::vector<DemoVertex> DemoSceneManager::ConvertToDemoVertices(const MeshData& mesh) {
    std::vector<DemoVertex> vertices{};
    const size_t vertex_count = mesh.positions.size() / 3U;
    if (vertex_count == 0U) return vertices;
    vertices.resize(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        const size_t p = i * 3U, t = i * 2U;
        vertices[i].px = mesh.positions[p+0]; vertices[i].py = mesh.positions[p+1]; vertices[i].pz = mesh.positions[p+2];
        if (mesh.normals.size() > p + 2) { vertices[i].nx = mesh.normals[p+0]; vertices[i].ny = mesh.normals[p+1]; vertices[i].nz = mesh.normals[p+2]; }
        vertices[i].u = (t + 1 < mesh.uvs.size()) ? mesh.uvs[t+0] : 0.0f;
        vertices[i].v = (t + 1 < mesh.uvs.size()) ? mesh.uvs[t+1] : 0.0f;
    }
    return vertices;
}

std::vector<uint32_t> DemoSceneManager::ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open SPIR-V file: " + path.string());
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("SPIR-V file is empty: " + path.string());
    }
    if ((size % static_cast<std::streamsize>(sizeof(uint32_t))) != 0) {
        throw std::runtime_error("SPIR-V file size is not word-aligned: " + path.string());
    }
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4U);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(words.data()), size);
    return words;
}

bool DemoSceneManager::UploadDemoScene(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                       const DemoVertex* vertices, uint32_t vertex_count,
                                       const uint32_t* indices, uint32_t index_count,
                                       VulkanEngine::TextureResource* texture,
                                       const uint32_t* vert_spv, size_t vert_spv_bytes,
                                       const uint32_t* frag_spv, size_t frag_spv_bytes) {
    LOGIFACE_LOG(trace, "entering UploadDemoScene");
    auto& backend = bootstrap.GetBackend();
    DestroyRawResources(backend);
    s_resources = std::make_unique<RawResources>();

    auto cleanup_and_fail = [&]() {
        DestroyRawResources(backend);
        return false;
    };

    if (!CreateBuffer(backend, vertex_count * sizeof(DemoVertex), vk::BufferUsageFlagBits::eVertexBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, s_resources->vertex_buffer, s_resources->vertex_memory)) return cleanup_and_fail();
    if (!CreateBuffer(backend, index_count * sizeof(uint32_t), vk::BufferUsageFlagBits::eIndexBuffer, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, s_resources->index_buffer, s_resources->index_memory)) return cleanup_and_fail();

    void* data = s_resources->vertex_memory->mapMemory(0, vertex_count * sizeof(DemoVertex));
    std::memcpy(data, vertices, vertex_count * sizeof(DemoVertex));
    s_resources->vertex_memory->unmapMemory();

    data = s_resources->index_memory->mapMemory(0, index_count * sizeof(uint32_t));
    std::memcpy(data, indices, index_count * sizeof(uint32_t));
    s_resources->index_memory->unmapMemory();

    if (texture && texture->HasPixels()) {
        if (!CreateTexture(backend, reinterpret_cast<const uint8_t*>(texture->GetPixels().data()), texture->GetWidth(), texture->GetHeight())) return cleanup_and_fail();
    }

    if (!CreatePipeline(backend, vert_spv, vert_spv_bytes, frag_spv, frag_spv_bytes, sizeof(DemoVertex))) return cleanup_and_fail();

    s_resources->index_count = index_count;
    LOGIFACE_LOG(trace, "leaving UploadDemoScene successfully");
    return true;
}

void DemoSceneManager::DestroyDemoSceneResources(VulkanEngine::Runtime::VulkanBootstrap& bootstrap) {
    DestroyRawResources(bootstrap.GetBackend());
}

bool DemoSceneManager::RenderDemoFrame(VulkanEngine::Runtime::VulkanBootstrap& bootstrap, uint32_t image_index, float angle_degrees) {
    if (!s_resources || !s_resources->pipeline) return false;
    auto& backend = bootstrap.GetBackend();
    uint32_t w = 0, h = 0;
    if (!backend.GetSwapchainExtent(w, h) || w == 0U || h == 0U) return false;
    VkImage current_depth_image = *backend.GetDepthImage();
    if (s_resources->depth_image_handle != current_depth_image) {
        s_resources->depth_image_handle = current_depth_image;
        s_resources->depth_initialized = false;
    }
    const uint32_t frame_idx = bootstrap.GetSnapshot().frame_index;
    auto& cmd = backend.GetCommandBuffer(frame_idx);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    auto image = backend.GetSwapchainImages()[image_index];
    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = backend.GetSwapchainImageInitializedFlags()[image_index] ? vk::ImageLayout::ePresentSrcKHR : vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    barrier.image = image;
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, barrier);

    if (!s_resources->depth_initialized) {
        vk::ImageMemoryBarrier depth_barrier{};
        depth_barrier.oldLayout = vk::ImageLayout::eUndefined;
        depth_barrier.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depth_barrier.image = current_depth_image;
        depth_barrier.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
        depth_barrier.dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eEarlyFragmentTests,
                            {}, nullptr, nullptr, depth_barrier);
        s_resources->depth_initialized = true;
    }

    vk::RenderingAttachmentInfo color_attach{};
    color_attach.imageView = *backend.GetSwapchainImageViews()[image_index];
    color_attach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attach.loadOp = vk::AttachmentLoadOp::eClear;
    color_attach.storeOp = vk::AttachmentStoreOp::eStore;
    color_attach.clearValue = vk::ClearColorValue(std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f});

    vk::RenderingAttachmentInfo depth_attach{};
    depth_attach.imageView = *backend.GetDepthImageView();
    depth_attach.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_attach.loadOp = vk::AttachmentLoadOp::eClear;
    depth_attach.storeOp = vk::AttachmentStoreOp::eDontCare;
    depth_attach.clearValue.depthStencil = vk::ClearDepthStencilValue(1.0f, 0);

    vk::RenderingInfo render_info{};
    render_info.renderArea = vk::Rect2D({0, 0}, {w, h});
    render_info.layerCount = 1;
    render_info.colorAttachmentCount = 1;
    render_info.pColorAttachments = &color_attach;
    render_info.pDepthAttachment = &depth_attach;
    cmd.beginRendering(render_info);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *s_resources->pipeline);
    cmd.setViewport(0, vk::Viewport(0, 0, (float)w, (float)h, 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));
    cmd.bindVertexBuffers(0, {*s_resources->vertex_buffer}, {0});
    cmd.bindIndexBuffer(*s_resources->index_buffer, 0, vk::IndexType::eUint32);
    if (!s_resources->descriptor_sets.empty()) cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *s_resources->pipeline_layout, 0, {*s_resources->descriptor_sets[0]}, {});

    const auto push_constants = BuildPushConstants(angle_degrees, static_cast<float>(w) / static_cast<float>(h));
    cmd.pushConstants(*s_resources->pipeline_layout,
                      vk::ShaderStageFlagBits::eVertex,
                      0,
                      vk::ArrayProxy<const PushConstants>(push_constants));

    cmd.drawIndexed(s_resources->index_count, 1, 0, 0, 0);
    cmd.endRendering();

    barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
    barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
    barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    barrier.dstAccessMask = {};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eBottomOfPipe, {}, nullptr, nullptr, barrier);
    cmd.end();
    return true;
}

void DemoSceneManager::DestroyRawResources(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    if (s_resources) {
        backend.GetDevice().waitIdle();
        s_resources.reset();
    }
}

bool DemoSceneManager::CreateBuffer(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, uint64_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties, std::unique_ptr<vk::raii::Buffer>& out_buffer, std::unique_ptr<vk::raii::DeviceMemory>& out_memory) {
    vk::BufferCreateInfo const info({}, size, usage);
    out_buffer = std::make_unique<vk::raii::Buffer>(backend.GetDevice(), info);

    // Use raw handles to get memory requirements
    VkBuffer raw_buffer = static_cast<VkBuffer>(**out_buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(static_cast<VkDevice>(*backend.GetDevice()), raw_buffer, &requirements);

    vk::MemoryAllocateInfo const alloc(requirements.size, FindMemoryType(backend.GetPhysicalDevice(), requirements.memoryTypeBits, properties));
    out_memory = std::make_unique<vk::raii::DeviceMemory>(backend.GetDevice(), alloc);
    out_buffer->bindMemory(*out_memory, 0);
    return true;
}

bool DemoSceneManager::CreateTexture(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, const uint8_t* pixels, uint32_t width, uint32_t height) {
    LOGIFACE_LOG(trace, "entering CreateTexture");
    if (!s_resources) return false;
    const uint64_t size = static_cast<uint64_t>(width) * height * 4;

    std::unique_ptr<vk::raii::Buffer> staging_buffer;
    std::unique_ptr<vk::raii::DeviceMemory> staging_memory;
    if (!CreateBuffer(backend, size, vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, staging_buffer, staging_memory)) return false;

    void* data = staging_memory->mapMemory(0, size);
    std::memcpy(data, pixels, size);
    staging_memory->unmapMemory();

    vk::ImageCreateInfo const image_info({}, vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm, vk::Extent3D(width, height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
    s_resources->texture_image = std::make_unique<vk::raii::Image>(backend.GetDevice(), image_info);

    vk::MemoryRequirements const requirements = s_resources->texture_image->getMemoryRequirements();
    vk::MemoryAllocateInfo const alloc(requirements.size, FindMemoryType(backend.GetPhysicalDevice(), requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal));
    s_resources->texture_memory = std::make_unique<vk::raii::DeviceMemory>(backend.GetDevice(), alloc);
    s_resources->texture_image->bindMemory(*s_resources->texture_memory, 0);

    auto& cmd = backend.GetCommandBuffer(0);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.image = **s_resources->texture_image;
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, barrier);

    vk::BufferImageCopy const region(0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {0, 0, 0}, {width, height, 1});
    cmd.copyBufferToImage(**staging_buffer, **s_resources->texture_image, vk::ImageLayout::eTransferDstOptimal, region);

    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);

    cmd.end();
    vk::SubmitInfo const submit(0, nullptr, nullptr, 1, &*cmd);
    backend.GetGraphicsQueue().submit(submit, nullptr);
    backend.GetGraphicsQueue().waitIdle();

    vk::ImageViewCreateInfo const view_info({}, **s_resources->texture_image, vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Unorm, {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    s_resources->texture_view = std::make_unique<vk::raii::ImageView>(backend.GetDevice(), view_info);

    vk::SamplerCreateInfo const sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear, vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat);
    s_resources->texture_sampler = std::make_unique<vk::raii::Sampler>(backend.GetDevice(), sampler_info);

    LOGIFACE_LOG(trace, "leaving CreateTexture successfully");
    return true;
}

bool DemoSceneManager::CreatePipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, const uint32_t* vert_spv, size_t vert_size, const uint32_t* frag_spv, size_t frag_size, uint32_t vertex_stride) {
    LOGIFACE_LOG(trace, "entering CreatePipeline");

    if (!s_resources) return false;
    const auto& device = backend.GetDevice();

    vk::ShaderModuleCreateInfo const vert_info({}, vert_size, vert_spv);
    vk::raii::ShaderModule const vert_module(device, vert_info);

    vk::ShaderModuleCreateInfo const frag_info({}, frag_size, frag_spv);
    vk::raii::ShaderModule const frag_module(device, frag_info);

    std::array<vk::PipelineShaderStageCreateInfo, 2> const stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *vert_module, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *frag_module, "main")
    };

    std::vector<vk::DescriptorSetLayoutBinding> const bindings = {
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment}
    };
    vk::DescriptorSetLayoutCreateInfo const dsl_info({}, bindings);
    s_resources->descriptor_set_layout = std::make_unique<vk::raii::DescriptorSetLayout>(device, dsl_info);

    const vk::PushConstantRange push_constant_range(vk::ShaderStageFlagBits::eVertex, 0, sizeof(PushConstants));
    const std::array<vk::DescriptorSetLayout, 1> set_layouts = { **s_resources->descriptor_set_layout };
    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges.size());
    layout_info.pPushConstantRanges = push_constant_ranges.data();
    s_resources->pipeline_layout = std::make_unique<vk::raii::PipelineLayout>(device, layout_info);

    std::array<vk::VertexInputBindingDescription, 1> const vertex_bindings = {
        vk::VertexInputBindingDescription(0, vertex_stride, vk::VertexInputRate::eVertex)
    };
    std::array<vk::VertexInputAttributeDescription, 3> const vertex_attributes = {
        vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32B32Sfloat, offsetof(DemoVertex, px)},
        vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32B32Sfloat, offsetof(DemoVertex, nx)},
        vk::VertexInputAttributeDescription{2, 0, vk::Format::eR32G32Sfloat, offsetof(DemoVertex, u)}
    };

    vk::PipelineVertexInputStateCreateInfo const vertex_input({}, vertex_bindings, vertex_attributes);
    vk::PipelineInputAssemblyStateCreateInfo const input_assembly({}, vk::PrimitiveTopology::eTriangleList);
    vk::PipelineViewportStateCreateInfo const viewport_state({}, 1, nullptr, 1, nullptr);
    vk::PipelineRasterizationStateCreateInfo const rasterization({}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise, false, 0, 0, 0, 1.0f);
    vk::PipelineMultisampleStateCreateInfo const multisample({}, vk::SampleCountFlagBits::e1);
    vk::PipelineDepthStencilStateCreateInfo const depth_stencil({}, true, true, vk::CompareOp::eLess);

    vk::PipelineColorBlendAttachmentState const color_blend_attachment(false, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd, vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    vk::PipelineColorBlendStateCreateInfo const color_blend({}, false, vk::LogicOp::eCopy, color_blend_attachment);

    std::array<vk::DynamicState, 2> const dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo const dynamic_state({}, dynamic_states);

    vk::Format const format = backend.GetSurfaceFormat().format;
    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &format;
    rendering_info.depthAttachmentFormat = backend.GetDepthFormat();
    rendering_info.stencilAttachmentFormat = vk::Format::eUndefined;

    vk::GraphicsPipelineCreateInfo pipeline_info({}, stages, &vertex_input, &input_assembly, nullptr, &viewport_state, &rasterization, &multisample, &depth_stencil, &color_blend, &dynamic_state, *s_resources->pipeline_layout, nullptr, 0, {}, 0); // Removed const
    pipeline_info.setPNext(&rendering_info);

    s_resources->pipeline = std::make_unique<vk::raii::Pipeline>(device, nullptr, pipeline_info);

    vk::DescriptorPoolSize const pool_size(vk::DescriptorType::eCombinedImageSampler, 1);
    vk::DescriptorPoolCreateInfo const pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, pool_size);
    s_resources->descriptor_pool = std::make_unique<vk::raii::DescriptorPool>(device, pool_info);

    vk::DescriptorSetAllocateInfo const alloc_info(**s_resources->descriptor_pool, **s_resources->descriptor_set_layout);
    s_resources->descriptor_sets = vk::raii::DescriptorSets(device, alloc_info);

    if (s_resources->texture_view && s_resources->texture_sampler) {
        vk::DescriptorImageInfo const image_info(**s_resources->texture_sampler, **s_resources->texture_view, vk::ImageLayout::eShaderReadOnlyOptimal);
        vk::WriteDescriptorSet const write(s_resources->descriptor_sets[0], 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &image_info);
        device.updateDescriptorSets(write, nullptr);
    }

    LOGIFACE_LOG(trace, "leaving CreatePipeline successfully");
    return true;
}

} // namespace App::DemoScene
