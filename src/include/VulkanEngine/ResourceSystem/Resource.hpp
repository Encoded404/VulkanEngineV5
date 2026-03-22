#ifndef VULKAN_ENGINE_RESOURCE_HPP
#define VULKAN_ENGINE_RESOURCE_HPP

#include <string>
#include <FileLoader/IncrementalBuffer.hpp>

class Resource {
private:
    std::string resourceId;     // Unique identifier for this resource within the system
    bool loaded = false;        // Loading state flag for resource lifecycle management

public:
    explicit Resource(const std::string& id) : resourceId(id) {}
    virtual ~Resource() = default;

    // Core resource identity and state access methods
    const std::string& GetId() const { return resourceId; }
    bool IsLoaded() const { return loaded; }

    // Virtual interface for resource-specific loading and unloading behavior
    bool Load() {
        loaded = doLoad();
        return loaded;
    }

    // Buffer-based loading: default implementation returns false so existing
    // resource types that don't implement buffer loading will fail gracefully.
    bool Load(const FileLoader::ByteBuffer& buf) {
        loaded = doLoadFromBuffer(buf);
        return loaded;
    }

    void Unload() {
        doUnload();
        loaded = false;
    }

    protected:
        virtual bool doLoad() = 0;
        virtual bool doUnload() = 0;
        // Optional buffer-based load hook; default returns false.
        virtual bool doLoadFromBuffer(const FileLoader::ByteBuffer& /*buf*/) { return false; }
};

#endif // VULKAN_ENGINE_RESOURCE_HPP