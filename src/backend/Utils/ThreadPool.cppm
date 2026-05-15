module;

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

export module VulkanBackend.Utils.ThreadPool;

export namespace VulkanEngine {

class ThreadPool {
public:
    explicit ThreadPool(unsigned int thread_count = 0)
        : worker_count_(thread_count == 0
            ? std::max(1u, std::thread::hardware_concurrency() > 1
                ? std::thread::hardware_concurrency() - 1
                : 1u)
            : thread_count)
    {
        workers_.reserve(worker_count_);
        for (unsigned int i = 0; i < worker_count_; ++i) {
            workers_.emplace_back(&ThreadPool::WorkerMain, this);
        }
    }

    ~ThreadPool() {
        {
            const std::scoped_lock lock(mutex_);
            shutdown_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template<typename Fn>
    void ParallelFor(std::size_t count, Fn&& fn) {
        if (count == 0) return;

        if (count == 1 || worker_count_ == 0) {
            for (std::size_t i = 0; i < count; ++i) {
                fn(i);
            }
            return;
        }

        {
            const std::scoped_lock lock(mutex_);
            task_.func = std::forward<Fn>(fn);
            task_.next_index.store(0, std::memory_order_relaxed);
            task_.end_index = count;
            active_workers_.store(worker_count_, std::memory_order_release);
            generation_.store(generation_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        }

        cv_.notify_all();

        while (true) {
            const std::size_t index = task_.next_index.fetch_add(1, std::memory_order_acq_rel);
            if (index >= task_.end_index) break;
            task_.func(index);
        }

        WaitForCompletion();
    }

    [[nodiscard]] unsigned int ThreadCount() const noexcept {
        return worker_count_;
    }

private:
    void WorkerMain() {
        unsigned int local_gen = 0;

        while (true) {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this, &local_gen] {
                return shutdown_.load(std::memory_order_acquire)
                    || generation_.load(std::memory_order_acquire) != local_gen;
            });

            if (shutdown_.load(std::memory_order_acquire)) {
                return;
            }

            local_gen = generation_.load(std::memory_order_acquire);
            lock.unlock();

            while (true) {
                const std::size_t index = task_.next_index.fetch_add(1, std::memory_order_acq_rel);
                if (index >= task_.end_index) break;
                task_.func(index);
            }

            if (active_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                const std::scoped_lock lock(mutex_);
                completion_cv_.notify_one();
            }
        }
    }

    void WaitForCompletion() {
        std::unique_lock lock(mutex_);
        completion_cv_.wait(lock, [this] {
            return active_workers_.load(std::memory_order_acquire) == 0;
        });
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable completion_cv_;

    std::atomic<bool> shutdown_{false};
    std::atomic<unsigned int> generation_{0};
    std::atomic<unsigned int> active_workers_{0};

    struct {
        std::function<void(std::size_t)> func;
        std::atomic<std::size_t> next_index{0};
        std::size_t end_index = 0;
    } task_;

    unsigned int worker_count_ = 0;
};

}
