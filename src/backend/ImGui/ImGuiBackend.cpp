module;

#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include <array>
#include <cstdint>
#include <memory>
#include <SDL3/SDL_events.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <vulkan/vulkan.hpp>

#include <logging/logging.hpp>

module VulkanBackend.ImGui;

namespace VulkanEngine::Backend::ImGui {

class VulkanImGuiBackend : public IImGuiBackend {
public:
    ~VulkanImGuiBackend() override = default;

    [[nodiscard]] bool Initialize(SDL_Window* window, const ImGuiBackendConfig& config,
                                  vk::Instance instance, vk::PhysicalDevice physical_device,
                                  vk::Device device, uint32_t queue_family, vk::Queue queue,
                                  uint32_t api_version) override {
        config_ = config;
        device_ = device;

        ::ImGui::DebugCheckVersionAndDataLayout(IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4), sizeof(ImDrawVert), sizeof(ImDrawIdx));
        ::ImGui::CreateContext();
        ImGuiIO& io = ::ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;

        ::ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForVulkan(window)) {
            return false;
        }

        std::array<vk::DescriptorPoolSize, 1> pool_sizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 16},
        };

        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = 16;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();

        descriptor_pool_ = std::make_unique<vk::DescriptorPool>(device.createDescriptorPool(pool_info));

        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = instance;
        init_info.PhysicalDevice = physical_device;
        init_info.Device = device;
        init_info.QueueFamily = queue_family;
        init_info.Queue = queue;
        init_info.DescriptorPool = *descriptor_pool_;
        init_info.MinImageCount = config.image_count;
        init_info.ImageCount = config.image_count;
        init_info.UseDynamicRendering = true;
        init_info.ApiVersion = api_version;
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        const vk::Format format = config.swapchain_format;
        init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = reinterpret_cast<const VkFormat*>(&format);
        init_info.CheckVkResultFn = [](VkResult err) {
            if (err < 0) {
                LOGIFACE_LOG(warn, "An error occurred when initializing ImGui. Error code: " + std::to_string(static_cast<uint32_t>(err)));
            }
        };

        if (!ImGui_ImplVulkan_Init(&init_info)) {
            return false;
        }

        initialized_ = true;
        return true;
    }

    void Shutdown() override {
        if (initialized_) {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ::ImGui::DestroyContext();
            if (descriptor_pool_) {
                device_.destroyDescriptorPool(*descriptor_pool_);
                descriptor_pool_.reset();
            }
            initialized_ = false;
        }
    }

    void NewFrame() override {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ::ImGui::NewFrame();
    }

    void ProcessSDLEvent(void* sdl_event) override {
        ImGui_ImplSDL3_ProcessEvent(static_cast<SDL_Event*>(sdl_event));
    }

    void RenderDrawData(vk::CommandBuffer command_buffer, vk::ImageView color_attachment,
                        vk::Format /*render_target_format*/, uint32_t width, uint32_t height) override {
        ::ImGui::Render();

        ImDrawData* draw_data = ::ImGui::GetDrawData();
        if (draw_data == nullptr || draw_data->TotalVtxCount == 0) {
            return;
        }

        vk::RenderingAttachmentInfo color_attach{};
        color_attach.imageView = color_attachment;
        color_attach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color_attach.loadOp = vk::AttachmentLoadOp::eLoad;
        color_attach.storeOp = vk::AttachmentStoreOp::eStore;

        vk::RenderingInfo render_info{};
        render_info.renderArea = vk::Rect2D({0, 0}, {width, height});
        render_info.layerCount = 1;
        render_info.colorAttachmentCount = 1;
        render_info.pColorAttachments = &color_attach;

        command_buffer.beginRendering(render_info);
        ImGui_ImplVulkan_RenderDrawData(draw_data, command_buffer);
        command_buffer.endRendering();
    }

    void OnResize() override {
        ImGui_ImplVulkan_SetMinImageCount(config_.image_count);
    }

    [[nodiscard]] vk::DescriptorPool GetDescriptorPool() const override {
        return *descriptor_pool_;
    }

private:
    ImGuiBackendConfig config_{};
    vk::Device device_{};
    std::unique_ptr<vk::DescriptorPool> descriptor_pool_{};
    bool initialized_ = false;
};

[[nodiscard]] std::shared_ptr<IImGuiBackend> CreateImGuiBackend() {
    return std::make_shared<VulkanImGuiBackend>();
}

}
