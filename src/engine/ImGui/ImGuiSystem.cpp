module;

#include <cstdint>
#include <memory>
#include <SDL3/SDL_video.h>
#include <imgui.h>
#include <vulkan/vulkan.hpp>

module VulkanEngine.ImGuiSystem;

namespace VulkanEngine::ImGuiSystem {

ImGuiSystem::ImGuiSystem(std::shared_ptr<VulkanEngine::Backend::ImGui::IImGuiBackend> backend)
    : backend_(std::move(backend)) {}

[[nodiscard]] bool ImGuiSystem::Initialize(const ImGuiSystemInitInfo& init_info) {
    config_ = init_info.config;
    backend_config_ = init_info.backend_config;

    if (!backend_->Initialize(static_cast<SDL_Window*>(init_info.sdl_window),
                              backend_config_,
                              init_info.instance,
                              init_info.physical_device,
                              init_info.device,
                              init_info.queue_family,
                              init_info.queue,
                              init_info.api_version)) {
        return false;
    }

    initialized_ = true;
    return true;
}

void ImGuiSystem::Shutdown() {
    if (initialized_) {
        backend_->Shutdown();
        initialized_ = false;
    }
}

void ImGuiSystem::NewFrame() {
    if (!initialized_) {
        return;
    }
    backend_->NewFrame();
}

void ImGuiSystem::ProcessSDLEvent(void* sdl_event) {
    if (!initialized_) {
        return;
    }
    backend_->ProcessSDLEvent(sdl_event);
}

void ImGuiSystem::SetDrawCallback(ImGuiDrawCallback callback) {
    draw_callback_ = std::move(callback);
}

void ImGuiSystem::RenderDrawData(vk::CommandBuffer command_buffer, vk::ImageView color_attachment, uint32_t width, uint32_t height) {
    if (!initialized_) {
        return;
    }

    if (draw_callback_) {
        draw_callback_();
    }

    if (config_.show_demo_window) {
        ::ImGui::ShowDemoWindow(&config_.show_demo_window);
    }
    if (config_.show_metrics) {
        ::ImGui::ShowMetricsWindow(&config_.show_metrics);
    }
    if (config_.show_style_editor) {
        ::ImGui::ShowStyleEditor();
    }

    backend_->RenderDrawData(command_buffer, color_attachment, backend_config_.swapchain_format, width, height);
}

}
