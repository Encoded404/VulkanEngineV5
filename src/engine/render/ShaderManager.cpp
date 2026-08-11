module;

#include <logging/logging_macros.hpp>

module VulkanEngine.ShaderManager;

import std;
import std.compat;

import logiface;
import vulkan_hpp;

import VulkanShared.FileIO;
import VulkanShared.ThreadPool;
import VulkanEngine.CompilerEngine;
import VulkanEngine.ShaderLoader;

namespace VulkanEngine::ShaderSystem {

namespace {
    std::string uuidToHex(const std::array<uint8_t, 16>& uuid) {
        std::string result;
        result.reserve(32);
        for (auto byte : uuid) {
            result += "0123456789abcdef"[byte >> 4];
            result += "0123456789abcdef"[byte & 0xf];
        }
        return result;
    }
}

ShaderManager::ShaderManager(const vk::raii::Device& device, const VulkanBackend::Vulkan::VulkanCapabilities& caps,
                               std::string_view cache_dir)
    : device_(device)
{
    auto uuid_hex = uuidToHex(caps.GetProperties().pipelineCacheUUID);
    cache_path_ = std::format("{}/pipeline_cache_{}.bin", cache_dir, uuid_hex);

    auto initial_data = VulkanShared::FileIO::ReadBinary(cache_path_.native());
    vk::PipelineCacheCreateInfo ci{};
    if (!initial_data.empty()) {
        ci.initialDataSize = initial_data.size();
        ci.pInitialData = initial_data.data();
    }

    try {
        cache_ = device_.createPipelineCache(ci);
    } catch (const vk::SystemError&) {
        if (!initial_data.empty()) {
            LOGIFACE_LOG(warn, "Pipeline cache load failed, starting fresh");
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            cache_ = device_.createPipelineCache(ci);
        }
    }
    LOGIFACE_LOG(info, std::format("ShaderManager: pipeline cache at {}", cache_path_.native()));
}

ShaderManager::~ShaderManager() {
    SerializeCache();
    cache_ = vk::raii::PipelineCache(nullptr);
}

ShaderId ShaderManager::Register(std::string spv_path, std::string slang_path,
                                   ShaderStage stage, std::span<const Binding> bindings,
                                   std::uint64_t binding_hash) {
    std::unique_lock lock(slots_mutex_);
    auto slot = std::make_unique<ShaderModuleSlot>();
    slot->spv_path = std::move(spv_path);
    slot->slang_path = std::move(slang_path);
    slot->stage = stage;
    slot->bindings.assign(bindings.begin(), bindings.end());
    slot->binding_hash = binding_hash;
    slot->manual = false;
    ShaderId id = static_cast<ShaderId>(slots_.size());
    slots_.push_back(std::move(slot));
    return id;
}

ShaderId ShaderManager::RegisterManual(std::string spv_path, std::string slang_path,
                                         ShaderStage stage) {
    std::unique_lock lock(slots_mutex_);
    auto slot = std::make_unique<ShaderModuleSlot>();
    slot->spv_path = std::move(spv_path);
    slot->slang_path = std::move(slang_path);
    slot->stage = stage;
    slot->binding_hash = 0;
    slot->manual = true;
    ShaderId id = static_cast<ShaderId>(slots_.size());
    slots_.push_back(std::move(slot));
    return id;
}

std::expected<vk::ShaderModule, std::string> ShaderManager::GetModule(ShaderId id) {
    std::shared_lock lock(slots_mutex_);
    auto& slot = *slots_[id];
    if (!slot.loaded) {
        lock.unlock();
        if (!LoadSpirvFromDisk(slot)) {
            return std::unexpected("Failed to load shader: " + slot.spv_path);
        }
        lock.lock();
    }
    return *slot.module;
}

const ShaderModuleSlot& ShaderManager::GetSlot(ShaderId id) const {
    std::shared_lock lock(slots_mutex_);
    return *slots_[id];
}

ShaderId ShaderManager::FindBySlangPath(std::string_view path) const {
    std::shared_lock lock(slots_mutex_);
    for (ShaderId i = 0; i < static_cast<ShaderId>(slots_.size()); ++i) {
        if (slots_[i]->slang_path == path) return i;
    }
    return static_cast<ShaderId>(-1);
}

std::uint64_t ShaderManager::GetVersion(ShaderId id) const {
    std::shared_lock lock(slots_mutex_);
    return slots_[id]->version.load(std::memory_order_acquire);
}

std::future<bool> ShaderManager::RequestReload(ShaderId id) {
    return VulkanShared::ThreadPool::Global().Enqueue([this, id]() -> bool {
        ShaderModuleSlot* slot = nullptr;
        {
            std::shared_lock lock(slots_mutex_);
            slot = slots_[id].get();
        }

#if VKENGINE_HOT_RELOAD
        VulkanEngine::ShaderSystem::CompileRequest req{};
        req.source_path = slot->slang_path;
        req.entry_point = "main";
        req.stage = slot->stage;
        req.optimization_level = 0;

        auto result = CompilerEngine::Compile(req);
        if (!result.has_value()) {
            LOGIFACE_LOG(error, std::format("ShaderManager: compilation failed: {}", result.error()));
            return false;
        }

        if (!slot->manual && result->binding_hash != slot->binding_hash) {
            LOGIFACE_LOG(error, std::format("ShaderManager: {} — bindings changed, C++ rebuild required", slot->slang_path));
            return false;
        }

        auto& spirv_vec = result->spirv;
        VulkanShared::FileIO::WriteBinary(slot->spv_path,
            std::span{reinterpret_cast<const std::byte*>(spirv_vec.data()),
                      spirv_vec.size() * sizeof(std::uint32_t)});

        vk::ShaderModuleCreateInfo info({},
            spirv_vec.size() * sizeof(std::uint32_t), spirv_vec.data());
        slot->module = vk::raii::ShaderModule(device_, info);
#else
        auto spirv_result = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(slot->spv_path);
        if (!spirv_result) {
            LOGIFACE_LOG(error, std::format("ShaderManager: reload failed for {}: {}", slot->spv_path, spirv_result.error()));
            return false;
        }
        auto& spirv = *spirv_result;
        vk::ShaderModuleCreateInfo info({},
            spirv.size() * sizeof(std::uint32_t), spirv.data());
        slot->module = vk::raii::ShaderModule(device_, info);
#endif

        slot->loaded = true;
        slot->version.fetch_add(1, std::memory_order_release);
        return true;
    });
}

std::future<bool> ShaderManager::RequestReloadByPath(std::string_view slang_path) {
    auto id = FindBySlangPath(slang_path);
    if (id == static_cast<ShaderId>(-1)) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    return RequestReload(id);
}

vk::PipelineCache ShaderManager::GetPipelineCache() const {
    return *cache_;
}

const vk::raii::PipelineCache& ShaderManager::GetPipelineCacheRAII() const {
    return cache_;
}

void ShaderManager::SerializeCache() {
    try {
        auto data = cache_.getData();
        if (!data.empty()) {
            VulkanShared::FileIO::AtomicWrite(cache_path_.native(),
                std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()});
        }
    } catch (const std::exception& e) {
        LOGIFACE_LOG(error, std::format("ShaderManager: failed to serialize pipeline cache: {}", e.what()));
    }
}

void ShaderManager::FlushCache() {
    SerializeCache();
}

bool ShaderManager::LoadSpirvFromDisk(ShaderModuleSlot& slot) {
    LOGIFACE_LOG(debug, std::format("ShaderManager: loading SPIR-V module from {}", slot.spv_path));
    auto result = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(slot.spv_path);
    if (!result) {
        LOGIFACE_LOG(error, std::format("ShaderManager: {} — {}", slot.spv_path, result.error()));
        return false;
    }
    auto& spirv = *result;
    if (spirv.empty()) {
        LOGIFACE_LOG(error, std::format("ShaderManager: empty SPIR-V for {}", slot.spv_path));
        return false;
    }
    vk::ShaderModuleCreateInfo info({},
        spirv.size() * sizeof(std::uint32_t), spirv.data());
    slot.module = vk::raii::ShaderModule(device_, info);
    slot.loaded = true;
    return true;
}

} // namespace VulkanEngine::ShaderSystem
