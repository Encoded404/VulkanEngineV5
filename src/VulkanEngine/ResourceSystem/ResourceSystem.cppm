module;

#include <string>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <shared_mutex>
#include <future>
#include <thread>
#include <mutex>
#include <type_traits>
#include <any>
#include <filesystem>
#include <functional>
#include <utility>
#include <cstdint>
#include <FileLoader/FileLoader.hpp>
#include <logging/logging.hpp>

export module VulkanEngine.ResourceSystem;

export namespace VulkanEngine {
    class Resource {
    private:
        std::string resourceId_;
        bool loaded_ = false;

    public:
        explicit Resource(std::string id);
        virtual ~Resource() = default;

        [[nodiscard]] const std::string& GetId() const;
        [[nodiscard]] bool IsLoaded() const;

        bool Load();
        bool Load(const FileLoader::ByteBuffer& buf);
        void Unload();

    protected:
        virtual bool DoLoad() = 0;
        virtual bool DoUnload() = 0;
        virtual bool DoLoadFromBuffer(const FileLoader::ByteBuffer& buf);
    };

    class ResourceManager;

    template<typename T>
    class ResourceHandle {
    private:
        std::string resourceId_;
        ResourceManager* resourceManager_;

    public:
        ResourceHandle() : resourceManager_(nullptr) {}

        ResourceHandle(std::string id, ResourceManager* manager)
            : resourceId_(std::move(id)), resourceManager_(manager) {}

        T* Get() const;
        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] const std::string& GetId() const { return resourceId_; }

        T* operator->() const { return Get(); }
        T& operator*() const { return *Get(); }
        operator bool() const { return IsValid(); }
    };

    class ResourceManager {
    private:
        std::unordered_map<std::type_index,
                           std::unordered_map<std::string, std::shared_ptr<Resource>>> resources_;

        struct ResourceData {
            std::shared_ptr<Resource> resource; //NOLINT(misc-non-private-member-variables-in-classes)
            int refCount {0}; //NOLINT(misc-non-private-member-variables-in-classes)
            ResourceData() : resource(nullptr) {}
        };
        std::unordered_map<std::type_index,
                           std::unordered_map<std::string, ResourceData>> refCounts_;

        mutable std::shared_mutex mutex_;

        struct InProgressEntry {
            std::any outwardFuture;
            std::function<void(std::uint64_t)> setReadRate;
            std::any handleBox;
        };

        std::unordered_map<std::type_index,
                           std::unordered_map<std::string, InProgressEntry>> inProgress_;

    public:
        enum class LoadSpeed {
            Instant,
            Fast,
            Normal,
            Slow
        };

        template<typename T>
        ResourceHandle<T> LoadFromFile(const std::filesystem::path& path, LoadSpeed speed = LoadSpeed::Normal, std::string resource_id = {}) {
            static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");
            auto type_idx = std::type_index(typeid(T));

            resource_id = resource_id.empty() ? path.string() : resource_id;

            {
                const std::shared_lock<std::shared_mutex> rlock(mutex_);
                auto type_it = resources_.find(type_idx);
                if (type_it != resources_.end()) {
                    auto it = type_it->second.find(resource_id);
                    if (it != type_it->second.end()) {
                        ++refCounts_[type_idx][resource_id].refCount;
                        return ResourceHandle<T>(resource_id, this);
                    }
                }
            }

            {
                InProgressEntry entry_copy;
                bool found = false;
                {
                    const std::shared_lock<std::shared_mutex> rlock(mutex_);
                    auto in_prog_type_it = inProgress_.find(type_idx);
                    if (in_prog_type_it != inProgress_.end()) {
                        auto fut_it = in_prog_type_it->second.find(resource_id);
                        if (fut_it != in_prog_type_it->second.end()) {
                            entry_copy = fut_it->second;
                            found = true;
                        }
                    }
                }

                if (found) {
                    try { if (entry_copy.setReadRate) entry_copy.setReadRate(0); } catch(...) {LOGIFACE_LOG(warn, "Failed to set read rate to 0 for resource '" + resource_id + "'");}

                    {
                        const std::unique_lock<std::shared_mutex> wlock(mutex_);
                        refCounts_[type_idx][resource_id];
                        ++refCounts_[type_idx][resource_id].refCount;
                    }

                    try {
                        auto out_fut = std::any_cast<std::shared_future<ResourceHandle<T>>>(entry_copy.outwardFuture);
                        return out_fut.get();
                    } catch (...) {
                        LOGIFACE_LOG(warn, "Failed to retrieve shared load result for resource '" + resource_id + "'");
                        return ResourceHandle<T>();
                    }
                }
            }

            std::uint64_t rate = 0;
            switch (speed) {
                case LoadSpeed::Instant: rate = 0; break;
                case LoadSpeed::Fast:    rate = 50ULL * 1024 * 1024; break;
                case LoadSpeed::Normal:  rate = 10ULL * 1024 * 1024; break;
                case LoadSpeed::Slow:    rate = 1ULL  * 1024 * 1024; break;
            }

            using ByteBuffer = FileLoader::ByteBuffer;
            struct Asm : FileLoader::IAssembler<T, FileLoader::AssemblyMode::FullBuffer> {
                std::string id; //NOLINT(misc-non-private-member-variables-in-classes)
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

            auto handle = fm.LoadFile<T>(info, std::make_shared<Asm>(resource_id));

            std::shared_ptr<std::promise<ResourceHandle<T>>> out_promise = std::make_shared<std::promise<ResourceHandle<T>>>();
            auto out_fut = out_promise->get_future().share();

            auto handle_box = std::make_shared<std::decay_t<decltype(handle)>>(std::move(handle));

            InProgressEntry entry;
            entry.outwardFuture = out_fut;
            entry.setReadRate = [handle_box](std::uint64_t r) {
                try { handle_box->SetReadRateLimit(r); } catch(...) {LOGIFACE_LOG(warn, "Failed to set read rate to " + std::to_string(r) + " for resource with handle");}
            };
            entry.handleBox = handle_box;

            {
                const std::unique_lock<std::shared_mutex> wlock(mutex_);
                inProgress_[type_idx][resource_id] = entry;
                refCounts_[type_idx][resource_id];
                refCounts_[type_idx][resource_id].refCount = 1;
            }

            std::shared_future<std::shared_ptr<T>> internal_fut = handle_box->GetFuture();
            std::shared_ptr<T> res_ptr;
            try {
                res_ptr = internal_fut.get();
            } catch (...) {
                std::unique_lock<std::shared_mutex> const wlock(mutex_);
                auto it = inProgress_.find(type_idx);
                if (it != inProgress_.end()) {
                    it->second.erase(resource_id);
                    if (it->second.empty()) inProgress_.erase(it);
                }
                refCounts_[type_idx].erase(resource_id);
                out_promise->set_value(ResourceHandle<T>());
                return ResourceHandle<T>();
            }

            if (!res_ptr) {
                std::unique_lock<std::shared_mutex> const wlock(mutex_);
                auto it = inProgress_.find(type_idx);
                if (it != inProgress_.end()) {
                    it->second.erase(resource_id);
                    if (it->second.empty()) inProgress_.erase(it);
                }
                refCounts_[type_idx].erase(resource_id);
                out_promise->set_value(ResourceHandle<T>());
                return ResourceHandle<T>();
            }

            {
                std::unique_lock<std::shared_mutex> const wlock(mutex_);
                resources_[type_idx][resource_id] = res_ptr;
                refCounts_[type_idx][resource_id].resource = res_ptr;
                auto it = inProgress_.find(type_idx);
                if (it != inProgress_.end()) {
                    it->second.erase(resource_id);
                    if (it->second.empty()) inProgress_.erase(it);
                }
            }
            out_promise->set_value(ResourceHandle<T>(resource_id, this));
            return ResourceHandle<T>(resource_id, this);
        }

        template<typename T>
        std::shared_future<ResourceHandle<T>> LoadFromFileAsync(const std::filesystem::path& path, LoadSpeed speed = LoadSpeed::Normal, std::string resource_id = {}) {
            static_assert(std::is_base_of_v<Resource, T>, "T must derive from Resource");
            auto type_idx = std::type_index(typeid(T));

            resource_id = resource_id.empty() ? path.string() : resource_id;

            InProgressEntry entry_copy;
            bool entry_found = false;
            {
                std::shared_lock<std::shared_mutex> const rlock(mutex_);
                if (auto type_it = resources_.find(type_idx); type_it != resources_.end()) {
                    if (auto it = type_it->second.find(resource_id); it != type_it->second.end()) {
                        std::promise<ResourceHandle<T>> p;
                        p.set_value(ResourceHandle<T>(resource_id, this));
                        return p.get_future().share();
                    }
                }

                if (auto in_prog_type_it = inProgress_.find(type_idx); in_prog_type_it != inProgress_.end()) {
                    if (auto fut_it = in_prog_type_it->second.find(resource_id); fut_it != in_prog_type_it->second.end()) {
                        entry_copy = fut_it->second;
                        entry_found = true;
                    }
                }
            }

            if (entry_found) {
                {
                    std::unique_lock<std::shared_mutex> const wlock(mutex_);
                    refCounts_[type_idx][resource_id];
                    ++refCounts_[type_idx][resource_id].refCount;
                }

                try {
                    return std::any_cast<std::shared_future<ResourceHandle<T>>>(entry_copy.outwardFuture);
                } catch (...) {
                    LOGIFACE_LOG(warn, "Failed to retrieve shared load result for resource '" + resource_id + "' in async load");
                }
            }

            std::uint64_t rate = 0;
            switch (speed) {
                case LoadSpeed::Instant: rate = 0; break;
                case LoadSpeed::Fast:    rate = 50ULL * 1024 * 1024; break;
                case LoadSpeed::Normal:  rate = 10ULL * 1024 * 1024; break;
                case LoadSpeed::Slow:    rate = 1ULL  * 1024 * 1024; break;
            }

            using ByteBuffer = FileLoader::ByteBuffer;
            struct Asm : FileLoader::IAssembler<T, FileLoader::AssemblyMode::FullBuffer> {
                std::string id; //NOLINT(misc-non-private-member-variables-in-classes)
                explicit Asm(std::string i) : id(std::move(i)) {}
                std::future<std::shared_ptr<T>> AssembleFromFullBuffer(std::shared_ptr<ByteBuffer> buffer) override {
                    auto prom = std::make_shared<std::promise<std::shared_ptr<T>>>();
                    try {
                        if (auto res = std::make_shared<T>(id); !res->Load(*buffer)) {
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

            auto handle = fm.LoadFile<T>(info, std::make_shared<Asm>(resource_id));

            auto handle_box = std::make_shared<std::decay_t<decltype(handle)>>(std::move(handle));
            std::shared_future<std::shared_ptr<T>> internal_fut = handle_box->GetFuture();

            std::shared_ptr<std::promise<ResourceHandle<T>>> out_promise = std::make_shared<std::promise<ResourceHandle<T>>>();
            auto out_fut = out_promise->get_future().share();

            InProgressEntry entry;
            entry.outwardFuture = out_fut;
            entry.setReadRate = [handle_box](std::uint64_t r) {
                try { handle_box->SetReadRateLimit(r); } catch(...) {LOGIFACE_LOG(warn, "Failed to set read rate to " + std::to_string(r) + " for resource with handle in async load");}
            };
            entry.handleBox = handle_box;

            {
                std::unique_lock<std::shared_mutex> const wlock(mutex_);
                inProgress_[type_idx][resource_id] = entry;
                refCounts_[type_idx][resource_id];
                refCounts_[type_idx][resource_id].refCount = 1;
            }

            std::thread([this, type_idx, resource_id, internal_fut, out_promise]() mutable {
                try {
                    auto res_ptr = internal_fut.get();
                    if (!res_ptr) {
                        std::unique_lock<std::shared_mutex> const wlock(mutex_);
                        auto it = inProgress_.find(type_idx);
                        if (it != inProgress_.end()) {
                            it->second.erase(resource_id);
                            if (it->second.empty()) inProgress_.erase(it);
                        }
                        refCounts_[type_idx].erase(resource_id);
                        out_promise->set_value(ResourceHandle<T>());
                        return;
                    }

                    {
                        std::unique_lock<std::shared_mutex> const wlock(mutex_);
                        resources_[type_idx][resource_id] = res_ptr;
                        refCounts_[type_idx][resource_id].resource = res_ptr;
                        auto it = inProgress_.find(type_idx);
                        if (it != inProgress_.end()) {
                            it->second.erase(resource_id);
                            if (it->second.empty()) inProgress_.erase(it);
                        }
                    }
                    out_promise->set_value(ResourceHandle<T>(resource_id, this));
                } catch (...) {
                    std::unique_lock<std::shared_mutex> const wlock(mutex_);
                    if (auto it = inProgress_.find(type_idx); it != inProgress_.end()) {
                        it->second.erase(resource_id);
                        if (it->second.empty()) inProgress_.erase(it);
                    }
                    refCounts_[type_idx].erase(resource_id);
                    out_promise->set_exception(std::current_exception());
                }
            }).detach();

            return out_fut;
        }

        template<typename T>
        T* GetResource(const std::string& resource_id) {
            const auto type_idx = std::type_index(typeid(T));
            std::shared_lock<std::shared_mutex> const rlock(mutex_);

            const auto type_it = resources_.find(type_idx);
            if (type_it == resources_.end()) return nullptr;

            if (const auto it = type_it->second.find(resource_id); it != type_it->second.end()) return static_cast<T*>(it->second.get());

            return nullptr;
        }

        template<typename T>
        bool HasResource(const std::string& resource_id) {
            auto type_idx = std::type_index(typeid(T));
            std::shared_lock<std::shared_mutex> const rlock(mutex_);

            auto type_it = resources_.find(type_idx);
            if (type_it == resources_.end()) return false;

            return type_it->second.contains(resource_id);
        }

        void Release(const std::string& resource_id);

        void UnloadAll();
    };

    template<typename T>
    T* ResourceHandle<T>::Get() const {
        if (!resourceManager_) return nullptr;
        return resourceManager_->GetResource<T>(resourceId_);
    }

    template<typename T>
    bool ResourceHandle<T>::IsValid() const {
        return resourceManager_ && resourceManager_->HasResource<T>(resourceId_);
    }
}