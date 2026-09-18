module;

export module VulkanShared.Teardown;

import std;

export namespace VulkanShared {

/// Identifier for a task registered with a TeardownScheduler.
using TeardownId = std::size_t;

/// Executes teardown tasks concurrently, honoring explicit dependencies.
///
/// Intended for the shutdown path: after the GPU has been drained
/// (`waitIdle`), the resource destruction of independent subsystems can
/// proceed in parallel. A task starts only after every task in its
/// `dependencies` list has completed, so ownership/shared-object ordering can
/// be encoded (e.g. "destroy the shared backend only after the system that
/// holds the last reference has been torn down").
///
/// Rules:
///   - Add all tasks before calling `Run()` (Add after Run throws).
///   - `Run()` joins every worker before returning and rethrows the first task
///     exception once the whole batch has been attempted, so the caller knows
///     every destruction finished before the Vulkan device/instance are
///     released.
class TeardownScheduler {
public:
    explicit TeardownScheduler(std::size_t worker_count)
        : worker_count_(std::clamp<std::size_t>(worker_count, 1, 16)) {}

    TeardownScheduler(const TeardownScheduler&) = delete;
    TeardownScheduler& operator=(const TeardownScheduler&) = delete;

    ~TeardownScheduler() {
        try {
            Run();
        } catch (...) {
            // Destructors must not throw; the caller that used Run() explicitly
            // has already seen any exception.
        }
    }

    TeardownId Add(std::string name, std::function<void()> task = {},
                   std::vector<TeardownId> dependencies = {}) {
        std::lock_guard lock(mutex_);
        if (run_) {
            throw std::logic_error("TeardownScheduler: Add called after Run");
        }
        const TeardownId id = tasks_.size();
        Task t;
        t.name = std::move(name);
        t.fn = std::move(task);
        t.pending_deps = dependencies.size();
        for (const TeardownId dep : dependencies) {
            if (dep >= id) {
                throw std::out_of_range(
                    "TeardownScheduler: dependency refers to a task that has not been added yet");
            }
            // One edge per declared dependency (duplicates are honored).
            tasks_[dep].dependents.push_back(id);
        }
        tasks_.push_back(std::move(t));
        return id;
    }

    void Run() {
        {
            std::lock_guard lock(mutex_);
            if (run_) {
                return;
            }
            run_ = true;
            if (HasCycle()) {
                cycle_ = true;
            } else {
                for (std::size_t i = 0; i < tasks_.size(); ++i) {
                    if (tasks_[i].pending_deps == 0) {
                        ready_.push_back(i);
                    }
                }
                remaining_ = tasks_.size();
            }
        }
        if (cycle_) {
            throw std::logic_error("TeardownScheduler: dependency cycle detected");
        }
        if (remaining_ == 0) {
            std::exception_ptr err = first_exception_;
            if (err) {
                std::rethrow_exception(err);
            }
            return;
        }

        std::vector<std::thread> workers;
        workers.reserve(worker_count_);
        const std::size_t spawn = std::min(worker_count_, tasks_.size());
        for (std::size_t i = 0; i < spawn; ++i) {
            workers.emplace_back([this] { WorkerLoop(); });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        std::exception_ptr err = first_exception_;
        if (err) {
            std::rethrow_exception(err);
        }
    }

private:
    struct Task {
        std::string name;
        std::function<void()> fn;
        std::size_t pending_deps = 0;
        std::vector<TeardownId> dependents{};
    };

    [[nodiscard]] bool HasCycle() {
        std::vector<std::size_t> indegree(tasks_.size());
        for (std::size_t i = 0; i < tasks_.size(); ++i) {
            indegree[i] = tasks_[i].pending_deps;
        }
        std::vector<TeardownId> ready;
        for (std::size_t i = 0; i < tasks_.size(); ++i) {
            if (indegree[i] == 0) {
                ready.push_back(i);
            }
        }
        std::size_t processed = 0;
        while (!ready.empty()) {
            const TeardownId u = ready.back();
            ready.pop_back();
            ++processed;
            for (const TeardownId v : tasks_[u].dependents) {
                if (--indegree[v] == 0) {
                    ready.push_back(v);
                }
            }
        }
        return processed != tasks_.size();
    }

    void WorkerLoop() {
        for (;;) {
            TeardownId id{};
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return !ready_.empty() || remaining_ == 0; });
                if (remaining_ == 0 && ready_.empty()) {
                    return;
                }
                id = ready_.front();
                ready_.pop_front();
            }
            ExecuteTask(id);
        }
    }

    void ExecuteTask(TeardownId id) {
        std::exception_ptr error{};
        try {
            if (tasks_[id].fn) {
                tasks_[id].fn();
            }
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::lock_guard lock(mutex_);
            if (error && first_exception_ == nullptr) {
                first_exception_ = error;
            }
            --remaining_;
            for (const TeardownId v : tasks_[id].dependents) {
                if (--tasks_[v].pending_deps == 0) {
                    ready_.push_back(v);
                }
            }
        }
        cv_.notify_all();
    }

    std::vector<Task> tasks_;
    std::deque<TeardownId> ready_;
    std::size_t remaining_ = 0;
    std::size_t worker_count_ = 2;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool run_ = false;
    bool cycle_ = false;
    std::exception_ptr first_exception_{};
};

/// Runs `fn` on a detached worker thread (fire-and-forget).
///
/// ONLY use this for work that (a) owns everything it touches and (b) may be
/// abandoned if the process exits while it is still running - e.g. joining a
/// file-watcher thread that blocks for its polling interval. Never use it for
/// GPU resources that must be destroyed before the Vulkan device/instance are
/// torn down; those must go through a TeardownScheduler so the caller can wait
/// for them.
template<typename Fn>
void RunDetached(Fn&& fn) {
    std::thread{[fn = std::forward<Fn>(fn)]() mutable {
        try {
            fn();
        } catch (...) {
            // Fire-and-forget: never let a background teardown kill the process.
        }
    }}.detach();
}

} // namespace VulkanShared
