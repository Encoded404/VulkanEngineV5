module;

export module VulkanShared.ThreadPool;

import std;

export namespace VulkanShared {

class ThreadPool {
public:
    explicit ThreadPool(unsigned int thread_count = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template<typename F>
    [[nodiscard]] std::future<std::invoke_result_t<F>> Enqueue(F&& fn) {
        using ResultType = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<F>(fn));
        auto fut = task->get_future();
        EnqueueRaw([task] { (*task)(); });
        return fut;
    }

    template<typename Fn>
    void ParallelFor(std::size_t count, Fn&& fn) {
        if (count == 0) return;
        if (count == 1 || workers_.empty()) {
            fn(0);
            return;
        }

        auto next = std::make_shared<std::atomic<std::size_t>>(0);
        auto remaining = std::make_shared<std::atomic<unsigned int>>(
            static_cast<unsigned int>(workers_.size()));
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();

        for (std::size_t i = 1; i < workers_.size(); ++i) {
            EnqueueRaw([next, remaining, done, count, &fn] {
                while (true) {
                    auto idx = next->fetch_add(1, std::memory_order_acq_rel);
                    if (idx >= count) break;
                    fn(idx);
                }
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    done->set_value();
                }
            });
        }

        while (true) {
            auto idx = next->fetch_add(1, std::memory_order_acq_rel);
            if (idx >= count) break;
            fn(idx);
        }
        if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            done->set_value();
        }

        fut.wait();
    }

    void WaitForIdle();

    [[nodiscard]] unsigned int ThreadCount() const noexcept;

    static ThreadPool& Global();

private:
    void EnqueueRaw(std::function<void()> task);
    void WorkerMain();

    std::vector<std::jthread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> task_queue_;
    std::atomic<bool> shutdown_{false};
    std::atomic<unsigned int> idle_count_{0};
    unsigned int worker_count_ = 0;
};

ThreadPool::ThreadPool(unsigned int thread_count)
    : worker_count_(thread_count == 0
        ? std::max(1u, std::thread::hardware_concurrency() - 1u)
        : thread_count)
{
    workers_.reserve(worker_count_);
    for (unsigned int i = 0; i < worker_count_; ++i) {
        workers_.emplace_back(&ThreadPool::WorkerMain, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void ThreadPool::EnqueueRaw(std::function<void()> task) {
    {
        const std::scoped_lock lock(mutex_);
        task_queue_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::WorkerMain() {
    while (!shutdown_.load(std::memory_order_acquire)) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            idle_count_.fetch_add(1, std::memory_order_relaxed);
            cv_.wait(lock, [this] {
                return shutdown_.load(std::memory_order_acquire) || !task_queue_.empty();
            });
            idle_count_.fetch_sub(1, std::memory_order_relaxed);
            if (shutdown_ && task_queue_.empty()) return;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }
        task();
    }
}

void ThreadPool::WaitForIdle() {
    while (idle_count_.load(std::memory_order_relaxed) != worker_count_) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    const std::scoped_lock lock(mutex_);
}

unsigned int ThreadPool::ThreadCount() const noexcept {
    return worker_count_;
}

ThreadPool& ThreadPool::Global() {
    static ThreadPool pool(std::max(1u, std::thread::hardware_concurrency() - 1u));
    return pool;
}

} // namespace VulkanShared
