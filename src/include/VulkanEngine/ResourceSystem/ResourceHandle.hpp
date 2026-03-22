#ifndef VULKAN_ENGINE_RESOURCE_HANDLE_HPP
#define VULKAN_ENGINE_RESOURCE_HANDLE_HPP

#include <string>

template<typename T>
class ResourceHandle {
private:
    std::string resourceId;
    ResourceManager* resourceManager;

public:
    ResourceHandle() : resourceManager(nullptr) {}

    ResourceHandle(const std::string& id, ResourceManager* manager)
        : resourceId(id), resourceManager(manager) {}

    T* Get() const {
        if (!resourceManager) return nullptr;
        return resourceManager->GetResource<T>(resourceId);
    }

    bool IsValid() const {
        return resourceManager && resourceManager->HasResource<T>(resourceId);
    }

    const std::string& GetId() const {
        return resourceId;
    }

    // Convenience operators
    T* operator->() const {
        return Get();
    }

    T& operator*() const {
        return *Get();
    }

    operator bool() const {
        return IsValid();
    }
};

#endif // VULKAN_ENGINE_RESOURCE_HANDLE_HPP