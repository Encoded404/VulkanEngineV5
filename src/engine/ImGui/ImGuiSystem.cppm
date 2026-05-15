module;

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vulkan/vulkan.hpp>

export module VulkanEngine.ImGuiSystem;

export import VulkanBackend.ImGui;

export namespace VulkanEngine::ImGuiSystem {

struct ImGuiConfig {
    bool enabled = true;
    bool show_demo_window = false;
    bool show_metrics = false;
    bool show_style_editor = false;
    float font_size = 16.0f;
    std::string ini_file_path = "";
};

using ImGuiDrawCallback = std::function<void()>;

struct ImGuiSystemInitInfo {
    void* sdl_window = nullptr;
    ImGuiConfig config{};
    VulkanEngine::Backend::ImGui::ImGuiBackendConfig backend_config{};
    vk::Instance instance{};
    vk::PhysicalDevice physical_device{};
    vk::Device device{};
    uint32_t queue_family = UINT32_MAX;
    vk::Queue queue{};
    uint32_t api_version = VK_API_VERSION_1_0;
};

class ImGuiSystem {
public:
    explicit ImGuiSystem(std::shared_ptr<VulkanEngine::Backend::ImGui::IImGuiBackend> backend);

    [[nodiscard]] bool Initialize(const ImGuiSystemInitInfo& init_info);
    void Shutdown();

    void NewFrame();
    void ProcessSDLEvent(void* sdl_event);
    void SetDrawCallback(ImGuiDrawCallback callback);

    void RenderDrawData(vk::CommandBuffer command_buffer, vk::ImageView color_attachment, uint32_t width, uint32_t height);

    [[nodiscard]] const ImGuiConfig& GetConfig() const { return config_; }
    void SetConfig(const ImGuiConfig& config) { config_ = config; }

    [[nodiscard]] bool IsInitialized() const { return initialized_; }

private:
    std::shared_ptr<VulkanEngine::Backend::ImGui::IImGuiBackend> backend_;
    ImGuiConfig config_{};
    VulkanEngine::Backend::ImGui::ImGuiBackendConfig backend_config_{};
    ImGuiDrawCallback draw_callback_;
    bool initialized_ = false;
};

}
