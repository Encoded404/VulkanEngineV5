module;

#include <algorithm>

module VulkanEngine.Runtime.RuntimeShell;

namespace VulkanEngine::Runtime {

bool RuntimeShell::Initialize(const RuntimeConfig& config) {
    if (initialized_) {
        return true;
    }

    config_ = config;
    config_.frames_in_flight = std::max(config_.frames_in_flight, 1u);
    frame_counter_ = 0;
    minimized_ = false;
    pending_status_ = RuntimeStatus::Ok;
    initialized_ = true;
    return true;
}

void RuntimeShell::Shutdown() {
    initialized_ = false;
    pending_status_ = RuntimeStatus::ShutdownRequested;
}

RuntimeFrameInfo RuntimeShell::BeginFrame() {
    if (!initialized_) {
        return RuntimeFrameInfo{.status = RuntimeStatus::FatalError};
    }

    RuntimeFrameInfo frame_info{};
    frame_info.frame_index = frame_counter_;
    frame_info.swapchain_image_index = frame_counter_ % config_.frames_in_flight;

    if (minimized_) {
        frame_info.status = RuntimeStatus::Minimized;
        return frame_info;
    }

    frame_info.status = pending_status_;
    if (pending_status_ == RuntimeStatus::ResizePending ||
        pending_status_ == RuntimeStatus::SwapchainOutOfDate ||
        pending_status_ == RuntimeStatus::SwapchainSuboptimal) {
        pending_status_ = RuntimeStatus::Ok;
    }

    return frame_info;
}

void RuntimeShell::EndFrame() {
    if (!initialized_) {
        return;
    }

    ++frame_counter_;
}

void RuntimeShell::NotifyWindowResized() {
    pending_status_ = RuntimeStatus::ResizePending;
}

void RuntimeShell::NotifySwapchainOutOfDate() {
    pending_status_ = RuntimeStatus::SwapchainOutOfDate;
}

void RuntimeShell::NotifySwapchainSuboptimal() {
    pending_status_ = RuntimeStatus::SwapchainSuboptimal;
}

void RuntimeShell::NotifyWindowMinimized(bool minimized) {
    minimized_ = minimized;
}

void RuntimeShell::RequestShutdown() {
    pending_status_ = RuntimeStatus::ShutdownRequested;
}

bool RuntimeShell::IsInitialized() const {
    return initialized_;
}

bool RuntimeShell::ShouldShutdown() const {
    return pending_status_ == RuntimeStatus::ShutdownRequested;
}

}  // namespace VulkanEngine::Runtime
