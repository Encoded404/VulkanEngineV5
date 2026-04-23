module;

#include <string>
#include <shared_mutex>
#include <mutex>
#include <ranges>
#include <utility>
#include <FileLoader/Types.hpp>

module VulkanEngine.ResourceSystem;
namespace VulkanEngine {
    Resource::Resource(std::string id) : resourceId_(std::move(id)) {}

    const std::string& Resource::GetId() const {
        return resourceId_;
    }

    bool Resource::IsLoaded() const {
        return loaded_;
    }

    bool Resource::Load() {
        loaded_ = DoLoad();
        return loaded_;
    }

    bool Resource::Load(const FileLoader::ByteBuffer& buf) {
        loaded_ = DoLoadFromBuffer(buf);
        return loaded_;
    }

    void Resource::Unload() {
        DoUnload();
        loaded_ = false;
    }

    bool Resource::DoLoadFromBuffer(const FileLoader::ByteBuffer& /*buf*/) {
        return false;
    }

    void ResourceManager::Release(const std::string& resource_id) {
        std::unique_lock<std::shared_mutex> const wlock(mutex_);
        for (auto &[type_index, unordered_map] : refCounts_) {
            auto& map = unordered_map;
            auto it = map.find(resource_id);
            if (it != map.end()) {
                it->second.refCount--;
                if (it->second.refCount <= 0) {
                    auto res_type_it = resources_.find(type_index);
                    if (res_type_it != resources_.end()) {
                        auto r_it = res_type_it->second.find(resource_id);
                        if (r_it != res_type_it->second.end()) {
                            r_it->second->Unload();
                            res_type_it->second.erase(r_it);
                        }
                    }
                    map.erase(it);
                }
                return;
            }
        }
    }

    void ResourceManager::UnloadAll() {
        std::unique_lock<std::shared_mutex> const wlock(mutex_);
        for (auto & [type_index, type_resources] : resources_) {
            for (auto & [resource_id, resource] : type_resources) {
                resource->Unload();
            }
            type_resources.clear();
        }
        refCounts_.clear();
    }
}