module;

export module VulkanShared.DeviceScope;

import std;

export namespace VulkanShared {

// Serializes device-scope host access that the Vulkan API declares externally
// synchronized on the VkQueue objects. vkDeviceWaitIdle is defined as
// vkQueueWaitIdle on every queue of the device, so two threads idling (or
// idling while another thread submits) touch the same VkQueue concurrently and
// that is a valid-usage violation. Engine teardown destroys many GPU owners in
// parallel and several destructors idle the device themselves, so every such
// call takes this scope.
//
// The scope is non-recursive: a guarded call must not reach another guarded
// call. The shutdown idle task and the per-destructor idles are mutually
// exclusive by construction, so no nesting occurs.
class DeviceScopeGuard {
public:
    DeviceScopeGuard() : mutex_(&Mutex()) { mutex_->lock(); }
    ~DeviceScopeGuard() { mutex_->unlock(); }

    DeviceScopeGuard(const DeviceScopeGuard&) = delete;
    DeviceScopeGuard& operator=(const DeviceScopeGuard&) = delete;
    DeviceScopeGuard(DeviceScopeGuard&&) = delete;
    DeviceScopeGuard& operator=(DeviceScopeGuard&&) = delete;

private:
    // Function-local static: the mutex outlives every teardown worker, and its
    // initialization is thread-safe without a static init-order dependency.
    [[nodiscard]] static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }

    std::mutex* mutex_;
};

} // namespace VulkanShared
