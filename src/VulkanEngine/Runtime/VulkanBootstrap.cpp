module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "logging/logging.hpp"

module VulkanEngine.Runtime.VulkanBootstrap;

namespace VulkanEngine::Runtime {

VulkanBootstrap::VulkanBootstrap(std::shared_ptr<IVulkanBootstrapBackend> backend)
    : backend_(std::move(backend)) {}

bool VulkanBootstrap::Initialize(const VulkanBootstrapConfig& config) {
    if (!backend_) {
        snapshot_.status = BootstrapStatus::FatalError;
        initialized_ = false;
        return false;
    }

    config_ = config;
    config_.frames_in_flight = std::max(config_.frames_in_flight, 1u);
    config_.preferred_swapchain_image_count = std::max(config_.preferred_swapchain_image_count, 2u);

    snapshot_ = VulkanBootstrapSnapshot{};

    if (!backend_->CreateInstance(config_)) {
        snapshot_.status = BootstrapStatus::InstanceCreationFailed;
        backend_->Shutdown();
        return false;
    }
    snapshot_.instance_ready = true;

    if (!backend_->SelectPhysicalDevice()) {
        snapshot_.status = BootstrapStatus::DeviceSelectionFailed;
        backend_->Shutdown();
        return false;
    }

    if (!backend_->CreateLogicalDevice(config_.frames_in_flight)) {
        snapshot_.status = BootstrapStatus::DeviceCreationFailed;
        backend_->Shutdown();
        return false;
    }
    snapshot_.device_ready = true;

    uint32_t swapchain_image_count = 0;
    if (!backend_->CreateSwapchain(config_.preferred_swapchain_image_count, swapchain_image_count)) {
        snapshot_.status = BootstrapStatus::SwapchainCreationFailed;
        backend_->Shutdown();
        return false;
    }

    snapshot_.swapchain_ready = true;
    snapshot_.swapchain_image_count = swapchain_image_count;
    if (!backend_->GetSwapchainExtent(snapshot_.swapchain_width, snapshot_.swapchain_height)) {
        snapshot_.status = BootstrapStatus::SwapchainCreationFailed;
        backend_->Shutdown();
        return false;
    }
    snapshot_.status = BootstrapStatus::Ok;

    pending_status_ = BootstrapStatus::Ok;
    initialized_ = true;
    return true;
}

void VulkanBootstrap::Shutdown() {
    if (backend_ && initialized_) {
        backend_->Shutdown();
    }

    snapshot_ = VulkanBootstrapSnapshot{};
    snapshot_.status = BootstrapStatus::NotInitialized;
    pending_status_ = BootstrapStatus::NotInitialized;
    initialized_ = false;
}

VulkanBootstrapSnapshot VulkanBootstrap::BeginFrame() {
    LOGIFACE_LOG(trace, "entering VulkanBootstrap::BeginFrame");
    if (!initialized_) {
        VulkanBootstrapSnapshot snapshot{};
        snapshot.status = BootstrapStatus::NotInitialized;
        return snapshot;
    }

    if (pending_status_ == BootstrapStatus::DeviceLost) {
        snapshot_.status = BootstrapStatus::DeviceLost;
        return snapshot_;
    }

    if (pending_status_ == BootstrapStatus::SwapchainOutOfDate) {
        snapshot_.status = BootstrapStatus::SwapchainOutOfDate;
        return snapshot_;
    }

    snapshot_.status = BootstrapStatus::Ok;
    LOGIFACE_LOG(trace, "leaving VulkanBootstrap::BeginFrame successfully");
    return snapshot_;
}

void VulkanBootstrap::EndFrame() {
    LOGIFACE_LOG(trace, "entering VulkanBootstrap::EndFrame");
    if (!initialized_) {
        return;
    }

    ++snapshot_.frame_index;
    LOGIFACE_LOG(trace, "leaving VulkanBootstrap::EndFrame. frame_index is " + std::to_string(snapshot_.frame_index));
}

void VulkanBootstrap::NotifySwapchainOutOfDate() {
    pending_status_ = BootstrapStatus::SwapchainOutOfDate;
}

bool VulkanBootstrap::RecreateSwapchain() {
    if (!initialized_ || !backend_) {
        return false;
    }

    uint32_t swapchain_image_count = 0;
    if (!backend_->CreateSwapchain(config_.preferred_swapchain_image_count, swapchain_image_count)) {
        snapshot_.status = BootstrapStatus::SwapchainCreationFailed;
        snapshot_.swapchain_ready = false;
        return false;
    }

    snapshot_.swapchain_ready = true;
    snapshot_.swapchain_image_count = swapchain_image_count;
    if (!backend_->GetSwapchainExtent(snapshot_.swapchain_width, snapshot_.swapchain_height)) {
        snapshot_.status = BootstrapStatus::SwapchainCreationFailed;
        snapshot_.swapchain_ready = false;
        return false;
    }
    snapshot_.status = BootstrapStatus::Ok;
    pending_status_ = BootstrapStatus::Ok;
    return true;
}

bool VulkanBootstrap::AcquireNextImage(uint32_t& out_image_index) {
    if (!initialized_ || !backend_) {
        return false;
    }
    return backend_->AcquireNextImage(snapshot_.frame_index, out_image_index);
}

bool VulkanBootstrap::Present(uint32_t image_index, bool rendering_succeeded) {
    if (!initialized_ || !backend_) {
        return false;
    }
    return backend_->Present(snapshot_.frame_index, image_index, rendering_succeeded);
}

void VulkanBootstrap::NotifyDeviceLost() {
    pending_status_ = BootstrapStatus::DeviceLost;
}

bool VulkanBootstrap::IsInitialized() const {
    return initialized_;
}

const VulkanBootstrapSnapshot& VulkanBootstrap::GetSnapshot() const {
    return snapshot_;
}

}  // namespace VulkanEngine::Runtime
