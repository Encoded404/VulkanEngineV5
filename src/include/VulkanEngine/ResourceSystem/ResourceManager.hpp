#ifndef VULKAN_ENGINE_RESOURCE_MANAGER_HPP
#define VULKAN_ENGINE_RESOURCE_MANAGER_HPP

#include "Resource.hpp"
#include "ResourceHandle.hpp"
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <shared_mutex>
#include <future>
#include <thread>
#include <mutex>
#include <string>
#include <type_traits>
#include <any>
#include <filesystem>
#include <FileLoader/FileLoader.hpp>
#include <functional>

class ResourceManager {
private:
    // Two-level storage system: organize by type first, then by unique identifier
    // This approach enables type-safe resource access while maintaining efficient lookup
    std::unordered_map<std::type_index,
                       std::unordered_map<std::string, std::shared_ptr<Resource>>> resources;

    // Two-level reference counting system for automatic resource lifecycle management
    // First level maps resource type, second level maps resource IDs to their data
    struct ResourceData {
        std::shared_ptr<Resource> resource;  // The actual resource
        int refCount;                        // Reference count for this resource
        ResourceData() : resource(nullptr), refCount(0) {}
    };
    std::unordered_map<std::type_index,
                       std::unordered_map<std::string, ResourceData>> refCounts;

    // Concurrent access control
    mutable std::shared_mutex mutex_; // protects resources, refCounts and inProgress

    // Track in-progress loads to coalesce duplicate requests.
    // Each entry stores an outward future (type-erased) and a type-erased setter
    // that allows adjusting the underlying FileManager handle's read rate mid-load.
    struct InProgressEntry {
        std::any outwardFuture; // actually std::shared_future<ResourceHandle<T>> for some T
        std::function<void(std::uint64_t)> setReadRate; // type-erased setter for handle.SetReadRateLimit
        std::any handleBox; // hold the heap-allocated handle so stored lambdas don't dangle
    };

    std::unordered_map<std::type_index,
                       std::unordered_map<std::string, InProgressEntry>> inProgress;

public:
    enum class LoadSpeed {
        Instant,
        Fast,
        Normal,
        Slow
    };

    // Unified synchronous file-based loader that uses the external FileLoader
    // library to load the file at the requested `LoadSpeed` and calls
    // `Resource::Load(const ByteBuffer&)` on the resource with the resulting bytes.
    template<typename T>
    ResourceHandle<T> LoadFromFile(const std::filesystem::path& path, LoadSpeed speed = LoadSpeed::Normal, std::string& resourceId = "") {
        static_assert(std::is_base_of<Resource, T>::value, "T must derive from Resource");
        auto typeIdx = std::type_index(typeid(T));

        // if no explicit resourceId provided, use path as ID
        resourceId = resourceId.empty() ? path.string() : resourceId;

        // fast-path: already loaded
        {
            std::shared_lock<std::shared_mutex> rlock(mutex_);
            auto typeIt = resources.find(typeIdx);
            if (typeIt != resources.end()) {
                auto it = typeIt->second.find(resourceId);
                if (it != typeIt->second.end()) {
                    refCounts[typeIdx][resourceId].refCount++;
                    return ResourceHandle<T>(resourceId, this);
                }
            }
        }

        // If another load is already in-progress, coalesce: make it instant and wait for it
        {
            InProgressEntry entryCopy;
            bool found = false;
            {
                std::shared_lock<std::shared_mutex> rlock(mutex_);
                auto inProgTypeIt = inProgress.find(typeIdx);
                if (inProgTypeIt != inProgress.end()) {
                    auto futIt = inProgTypeIt->second.find(resourceId);
                    if (futIt != inProgTypeIt->second.end()) {
                        // copy entry to call setter outside the lock
                        entryCopy = futIt->second;
                        found = true;
                    }
                }
            }

            if (found) {
                try { if (entryCopy.setReadRate) entryCopy.setReadRate(0); } catch(...) {}

                // bump refcount for this requester
                {
                    std::unique_lock<std::shared_mutex> wlock(mutex_);
                    refCounts[typeIdx][resourceId];
                    refCounts[typeIdx][resourceId].refCount++;
                }

                try {
                    auto outFut = std::any_cast<std::shared_future<ResourceHandle<T>>>(entryCopy.outwardFuture);
                    return outFut.get();
                } catch (...) {
                    // fall through to start a fresh load if any_cast fails
                }
            }
        }

        // map speed to read-rate (0 = unthrottled)
        std::uint64_t rate = 0;
        switch (speed) {
            case LoadSpeed::Instant: rate = 0; break;
            case LoadSpeed::Fast:    rate = 50ULL * 1024 * 1024; break; // 50 MB/s
            case LoadSpeed::Normal:  rate = 10ULL * 1024 * 1024; break; // 10 MB/s
            case LoadSpeed::Slow:    rate = 1ULL  * 1024 * 1024; break; // 1 MB/s
        }

        // Use FileManager with a small assembler that constructs the resource
        using ByteBuffer = FileLoader::ByteBuffer;
        struct Asm : FileLoader::IAssembler<T, FileLoader::AssemblyMode::FullBuffer> {
            std::string id;
            explicit Asm(std::string i) : id(std::move(i)) {}
            std::future<std::shared_ptr<T>> AssembleFromFullBuffer(std::shared_ptr<ByteBuffer> buffer) override {
                auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
                try {
                    auto res = std::make_shared<T>(id);
                    if (!res->Load(*buffer)) {
                        prom->set_value(nullptr);
                    } else {
                        prom->set_value(res);
                    }
                } catch (...) { prom->set_exception(std::current_exception()); }
                return prom->get_future();
            }
        };

        static FileLoader::FileManager fm;
        FileLoader::FileLoadInfo info;
        info.path = path;
        info.initial_read_rate_bytes_per_sec = rate;

        auto handle = fm.LoadFile<T>(info, std::make_shared<Asm>(resourceId));

        // create outward promise so other callers can coalesce on this load
        std::shared_ptr<std::promise<ResourceHandle<T>>> outPromise = std::make_shared<std::promise<ResourceHandle<T>>>();
        auto outFut = outPromise->get_future().share();

        // move the handle into a heap box so we can store a setter lambda (type-erased)
        auto handleBox = std::make_shared<std::decay_t<decltype(handle)>>(std::move(handle));

        InProgressEntry entry;
        entry.outwardFuture = outFut;
        entry.setReadRate = [handleBox](std::uint64_t r) {
            try { handleBox->SetReadRateLimit(r); } catch(...) {}
        };
        entry.handleBox = handleBox;

        {
            std::unique_lock<std::shared_mutex> wlock(mutex_);
            inProgress[typeIdx][resourceId] = entry;
            refCounts[typeIdx][resourceId];
            refCounts[typeIdx][resourceId].refCount = 1;
        }

        // block until assembly completes
        std::shared_future<std::shared_ptr<T>> internalFut = handleBox->GetFuture();
        std::shared_ptr<T> resPtr;
        try {
            resPtr = internalFut.get();
        } catch (...) {
            std::unique_lock<std::shared_mutex> wlock(mutex_);
            auto it = inProgress.find(typeIdx);
            if (it != inProgress.end()) {
                it->second.erase(resourceId);
                if (it->second.empty()) inProgress.erase(it);
            }
            refCounts[typeIdx].erase(resourceId);
            outPromise->set_value(ResourceHandle<T>());
            return ResourceHandle<T>();
        }

        if (!resPtr) {
            std::unique_lock<std::shared_mutex> wlock(mutex_);
            auto it = inProgress.find(typeIdx);
            if (it != inProgress.end()) {
                it->second.erase(resourceId);
                if (it->second.empty()) inProgress.erase(it);
            }
            refCounts[typeIdx].erase(resourceId);
            outPromise->set_value(ResourceHandle<T>());
            return ResourceHandle<T>();
        }

        // store resource and finalize
        {
            std::unique_lock<std::shared_mutex> wlock(mutex_);
            resources[typeIdx][resourceId] = resPtr;
            refCounts[typeIdx][resourceId].resource = resPtr;
            auto it = inProgress.find(typeIdx);
            if (it != inProgress.end()) {
                it->second.erase(resourceId);
                if (it->second.empty()) inProgress.erase(it);
            }
        }
        outPromise->set_value(ResourceHandle<T>(resourceId, this));
        return ResourceHandle<T>(resourceId, this);
    }

    // Asynchronous variant that returns a shared_future<ResourceHandle<T>>
    template<typename T>
    std::shared_future<ResourceHandle<T>> LoadFromFileAsync(const std::filesystem::path& path, LoadSpeed speed = LoadSpeed::Normal, std::string& resourceId = "") {
        static_assert(std::is_base_of<Resource, T>::value, "T must derive from Resource");
        auto typeIdx = std::type_index(typeid(T));

        // if no explicit resourceId provided, use path as ID
        resourceId = resourceId.empty() ? path.string() : resourceId;

        // Fast path: already loaded
        InProgressEntry entryCopy;
        bool entryFound = false;
        {
            std::shared_lock<std::shared_mutex> rlock(mutex_);
            auto typeIt = resources.find(typeIdx);
            if (typeIt != resources.end()) {
                auto it = typeIt->second.find(resourceId);
                if (it != typeIt->second.end()) {
                    std::promise<ResourceHandle<T>> p;
                    p.set_value(ResourceHandle<T>(resourceId, this));
                    return p.get_future().share();
                }
            }

            // attempt to copy any in-progress entry while holding the shared lock
            auto inProgTypeIt = inProgress.find(typeIdx);
            if (inProgTypeIt != inProgress.end()) {
                auto futIt = inProgTypeIt->second.find(resourceId);
                if (futIt != inProgTypeIt->second.end()) {
                    entryCopy = futIt->second;
                    entryFound = true;
                }
            }
        }

        if (entryFound) {
            // bump refcount for this requester
            {
                std::unique_lock<std::shared_mutex> wlock(mutex_);
                refCounts[typeIdx][resourceId];
                refCounts[typeIdx][resourceId].refCount++;
            }

            try {
                return std::any_cast<std::shared_future<ResourceHandle<T>>>(entryCopy.outwardFuture);
            } catch (...) { /* fall through to start new load */ }
        }

        // map speed to read-rate
        std::uint64_t rate = 0;
        switch (speed) {
            case LoadSpeed::Instant: rate = 0; break;
            case LoadSpeed::Fast:    rate = 50ULL * 1024 * 1024; break;
            case LoadSpeed::Normal:  rate = 10ULL * 1024 * 1024; break;
            case LoadSpeed::Slow:    rate = 1ULL  * 1024 * 1024; break;
        }

        using ByteBuffer = FileLoader::ByteBuffer;
        struct Asm : FileLoader::IAssembler<T, FileLoader::AssemblyMode::FullBuffer> {
            std::string id;
            explicit Asm(std::string i) : id(std::move(i)) {}
            std::future<std::shared_ptr<T>> AssembleFromFullBuffer(std::shared_ptr<ByteBuffer> buffer) override {
                auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
                try {
                    auto res = std::make_shared<T>(id);
                    if (!res->Load(*buffer)) {
                        prom->set_value(nullptr);
                    } else {
                        prom->set_value(res);
                    }
                } catch (...) { prom->set_exception(std::current_exception()); }
                return prom->get_future();
            }
        };

        static FileLoader::FileManager fm;
        FileLoader::FileLoadInfo info;
        info.path = path;
        info.initial_read_rate_bytes_per_sec = rate;

        auto handle = fm.LoadFile<T>(info, std::make_shared<Asm>(resourceId));

        // move handle to heap and use its future so we can store a setter lambda
        auto handleBox = std::make_shared<std::decay_t<decltype(handle)>>(std::move(handle));
        std::shared_future<std::shared_ptr<T>> internalFut = handleBox->GetFuture();

        // create outward future and store in inProgress so callers can coalesce
        std::shared_ptr<std::promise<ResourceHandle<T>>> outPromise = std::make_shared<std::promise<ResourceHandle<T>>>();
        auto outFut = outPromise->get_future().share();

        InProgressEntry entry;
        entry.outwardFuture = outFut;
        entry.setReadRate = [handleBox](std::uint64_t r) {
            try { handleBox->SetReadRateLimit(r); } catch(...) {}
        };
        entry.handleBox = handleBox;

        {
            std::unique_lock<std::shared_mutex> wlock(mutex_);
            inProgress[typeIdx][resourceId] = entry;
            refCounts[typeIdx][resourceId];
            refCounts[typeIdx][resourceId].refCount = 1;
        }

        // wait for the assembled resource and finalize storage
        std::thread([this, typeIdx, resourceId, internalFut, outPromise]() mutable {
            try {
                auto resPtr = internalFut.get();
                if (!resPtr) {
                    std::unique_lock<std::shared_mutex> wlock(mutex_);
                    auto it = inProgress.find(typeIdx);
                    if (it != inProgress.end()) {
                        it->second.erase(resourceId);
                        if (it->second.empty()) inProgress.erase(it);
                    }
                    refCounts[typeIdx].erase(resourceId);
                    outPromise->set_value(ResourceHandle<T>());
                    return;
                }

                {
                    std::unique_lock<std::shared_mutex> wlock(mutex_);
                    resources[typeIdx][resourceId] = resPtr;
                    refCounts[typeIdx][resourceId].resource = resPtr;
                    auto it = inProgress.find(typeIdx);
                    if (it != inProgress.end()) {
                        it->second.erase(resourceId);
                        if (it->second.empty()) inProgress.erase(it);
                    }
                }
                outPromise->set_value(ResourceHandle<T>(resourceId, this));
            } catch (...) {
                std::unique_lock<std::shared_mutex> wlock(mutex_);
                auto it = inProgress.find(typeIdx);
                if (it != inProgress.end()) {
                    it->second.erase(resourceId);
                    if (it->second.empty()) inProgress.erase(it);
                }
                refCounts[typeIdx].erase(resourceId);
                outPromise->set_exception(std::current_exception());
            }
        }).detach();

        return outFut;
    }

    template<typename T>
    T* GetResource(const std::string& resourceId) {
        // get type index
        auto typeIdx = std::type_index(typeid(T));

        // lock resources for reading
        std::shared_lock<std::shared_mutex> rlock(mutex_);

        // find type map and ensure it exists
        auto typeIt = resources.find(typeIdx);
        if (typeIt == resources.end()) return nullptr;

        // find resource by ID and return if found
        auto it = typeIt->second.find(resourceId);
        if (it != typeIt->second.end()) return static_cast<T*>(it->second.get());
        
        return nullptr;
    }

    template<typename T>
    bool HasResource(const std::string& resourceId) {
        // get type index
        auto typeIdx = std::type_index(typeid(T));

        // lock resources for reading
        std::shared_lock<std::shared_mutex> rlock(mutex_);

        // find type map and ensure it exists
        auto typeIt = resources.find(typeIdx);
        if (typeIt == resources.end()) return false;

        // check if resource ID exists in type map
        return typeIt->second.find(resourceId) != typeIt->second.end();
    }

    void Release(const std::string& resourceId) {
        std::unique_lock<std::shared_mutex> wlock(mutex_);
        // Need to search across types to find matching refCount entries
        for (auto itType = refCounts.begin(); itType != refCounts.end(); ++itType) {
            auto& map = itType->second;
            auto it = map.find(resourceId);
            if (it != map.end()) {
                it->second.refCount--;
                if (it->second.refCount <= 0) {
                    // unload and remove from resources
                    auto resTypeIt = resources.find(itType->first);
                    if (resTypeIt != resources.end()) {
                        auto rIt = resTypeIt->second.find(resourceId);
                        if (rIt != resTypeIt->second.end()) {
                            rIt->second->Unload();
                            resTypeIt->second.erase(rIt);
                        }
                    }
                    map.erase(it);
                }
                return;
            }
        }
    }

    void UnloadAll() {
        // Emergency cleanup method for system shutdown or major state changes
        for (auto& [type, typeResources] : resources) {
            for (auto& [id, resource] : typeResources) {
                resource->Unload();     // Ensure all resources clean up properly
            }
            typeResources.clear();      // Clear type-specific containers
        }
        refCounts.clear();              // Reset all reference counts
    }
};

#endif // VULKAN_ENGINE_RESOURCE_MANAGER_HPP