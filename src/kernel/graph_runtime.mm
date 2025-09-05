// Photospider kernel: GraphRuntime implementation (Objective-C++)
#include "kernel/graph_runtime.hpp"

#include <filesystem>
#include <random>
#include <numeric>

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

namespace ps {

thread_local int GraphRuntime::tls_worker_id_ = -1;

struct GraphRuntime::GpuContext {
#ifdef __APPLE__
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
#endif
};

GraphRuntime::GraphRuntime(const Info& info)
    : info_(info), graph_(info.root / "cache") {
    std::filesystem::create_directories(info_.root);
    std::filesystem::create_directories(info_.root / "cache");

#ifdef __APPLE__
    gpu_context_ = std::make_unique<GpuContext>();
    gpu_context_->device = MTLCreateSystemDefaultDevice();
    if (gpu_context_->device) {
        gpu_context_->commandQueue = [gpu_context_->device newCommandQueue];
    } else {
        fprintf(stderr, "Warning: Could not create default Metal device.\n");
    }
#else
    // On non-Apple platforms, gpu_context_ remains null
#endif
}

GraphRuntime::~GraphRuntime() { 
    stop(); 
}

int GraphRuntime::this_worker_id() {
    return tls_worker_id_;
}

id GraphRuntime::get_metal_device() {
#ifdef __APPLE__
    return gpu_context_ ? gpu_context_->device : nil;
#else
    return nullptr;
#endif
}

id GraphRuntime::get_metal_command_queue() {
#ifdef __APPLE__
    return gpu_context_ ? gpu_context_->commandQueue : nil;
#else
    return nullptr;
#endif
}


void GraphRuntime::start() {
    if (running_) return;
    
    has_exception_.store(false, std::memory_order_relaxed);
    first_exception_ = nullptr;
    // Reset scheduler counters to a known baseline in case of prior stop/start cycles
    ready_task_count_.store(0, std::memory_order_relaxed);
    sleeping_thread_count_.store(0, std::memory_order_relaxed);
    tasks_to_complete_.store(0, std::memory_order_relaxed);
    running_ = true;

    num_workers_ = std::max(1u, std::thread::hardware_concurrency());
    local_task_queues_.resize(num_workers_);
    local_queue_mutexes_.reserve(num_workers_);
    for (unsigned int i = 0; i < num_workers_; ++i) {
        local_queue_mutexes_.push_back(std::make_unique<std::mutex>());
    }

    workers_.reserve(num_workers_);
    for (unsigned int i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&GraphRuntime::run_loop, this, i);
    }
}

void GraphRuntime::stop() {
    if (!running_) return;
    
    running_ = false;
    cv_task_available_.notify_all();
    cv_completion_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    local_task_queues_.clear();
    local_queue_mutexes_.clear();
    // Drain any pending global tasks to avoid dangling function targets after shutdown
    {
        std::lock_guard<std::mutex> lock(global_queue_mutex_);
        while (!global_task_queue_.empty()) global_task_queue_.pop();
    }
}

std::optional<Task> GraphRuntime::steal_task(int stealer_id) {
    int n = static_cast<int>(num_workers_);
    if (n <= 1) return std::nullopt;

    static thread_local std::mt19937 rng(std::random_device{}() + stealer_id);
    int start = std::uniform_int_distribution<int>(0, n - 2)(rng);

    for (int i = 0; i < n - 1; ++i) {
        int victim_id = (start + i) % (n - 1);
        if (victim_id >= stealer_id) victim_id++;

        if (victim_id < 0 || victim_id >= static_cast<int>(local_queue_mutexes_.size()) ||
            victim_id >= static_cast<int>(local_task_queues_.size())) {
            continue;
        }

        std::lock_guard<std::mutex> lock(*local_queue_mutexes_[victim_id]);
        if (!local_task_queues_[victim_id].empty()) {
            Task stolen_task = std::move(local_task_queues_[victim_id].front());
            local_task_queues_[victim_id].pop_front();
            return stolen_task;
        }
    }
    return std::nullopt;
}

void GraphRuntime::run_loop(int thread_id) {
    tls_worker_id_ = thread_id;

    while (running_) {
        if (has_exception_.load(std::memory_order_acquire)) {
            return;
        }

        Task task;
        bool found_task = false;

        {
            if (thread_id >= 0 && thread_id < static_cast<int>(local_queue_mutexes_.size())) {
                std::lock_guard<std::mutex> lock(*local_queue_mutexes_[thread_id]);
                if (thread_id < static_cast<int>(local_task_queues_.size()) && !local_task_queues_[thread_id].empty()) {
                    task = std::move(local_task_queues_[thread_id].back());
                    local_task_queues_[thread_id].pop_back();
                    found_task = true;
                }
            }
        }

        if (!found_task) {
            auto stolen_task = steal_task(thread_id);
            if (stolen_task) {
                task = std::move(*stolen_task);
                found_task = true;
            }
        }

        if (!found_task) {
            std::lock_guard<std::mutex> lock(global_queue_mutex_);
            if (!global_task_queue_.empty()) {
                task = std::move(global_task_queue_.front());
                global_task_queue_.pop();
                found_task = true;
            }
        }
        
        if (found_task) {
            ready_task_count_.fetch_sub(1, std::memory_order_release);
            try {
                if (task) {
                    task();
                } else {
                    // Unexpected empty task – surface as exception so the scheduler unwinds safely
                    set_exception(std::make_exception_ptr(std::runtime_error("GraphRuntime: empty task invoked")));
                }
            } catch (...) {
                set_exception(std::current_exception());
            }
        } else {
            sleeping_thread_count_.fetch_add(1, std::memory_order_release);
            
            std::unique_lock<std::mutex> lock(global_queue_mutex_);

            if (ready_task_count_.load(std::memory_order_acquire) > 0) {
                sleeping_thread_count_.fetch_sub(1, std::memory_order_relaxed);
                continue; 
            }
            
            cv_task_available_.wait(lock, [&]{ 
                return ready_task_count_.load(std::memory_order_acquire) > 0 || !running_; 
            });

            sleeping_thread_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}


void GraphRuntime::submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count) {
    has_exception_.store(false, std::memory_order_relaxed);
    first_exception_ = nullptr;
    tasks_to_complete_.store(total_task_count, std::memory_order_relaxed);
    
    if (tasks_to_complete_.load() == 0) {
        std::lock_guard<std::mutex> lk(completion_mutex_);
        cv_completion_.notify_one();
        return;
    }

    int num_threads = static_cast<int>(num_workers_);
    if (num_threads == 0 || tasks.empty()) {
        return;
    }

    static thread_local std::mt19937 rng(std::random_device{}());
    for (size_t i = 0; i < tasks.size(); ++i) {
        int target_thread = std::uniform_int_distribution<int>(0, num_threads - 1)(rng);
        std::lock_guard<std::mutex> lock(*local_queue_mutexes_[target_thread]);
        local_task_queues_[target_thread].push_back(std::move(tasks[i]));
    }
    
    {
        std::lock_guard<std::mutex> lock(global_queue_mutex_);
        ready_task_count_.fetch_add(tasks.size(), std::memory_order_release);
    }
    cv_task_available_.notify_all();
}

void GraphRuntime::submit_ready_task_any_thread(Task&& task) {
    if (!task) {
        // Do not enqueue empty tasks; let producer decide completion semantics
        return;
    }
    {
        std::lock_guard<std::mutex> lock(global_queue_mutex_);
        global_task_queue_.push(std::move(task));
        ready_task_count_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_task_available_.notify_one();
}

void GraphRuntime::submit_ready_task_from_worker(Task&& task) {
    int worker_id = this_worker_id();  // -1 表示当前不是 worker 线程
    if (worker_id < 0 || worker_id >= static_cast<int>(local_task_queues_.size())) {
        // 兜底：不是 worker，就走任意线程安全投递
        submit_ready_task_any_thread(std::move(task));
        return;
    }
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(*local_queue_mutexes_[worker_id]);
        local_task_queues_[worker_id].push_back(std::move(task));
        ready_task_count_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_task_available_.notify_one();
}

void GraphRuntime::dec_graph_tasks_to_complete() {
    if (tasks_to_complete_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lk(completion_mutex_);
        cv_completion_.notify_one();
    }
}

void GraphRuntime::wait_for_completion() {
    {
        std::unique_lock<std::mutex> lock(completion_mutex_);
        cv_completion_.wait(lock, [&]{ 
            return tasks_to_complete_.load(std::memory_order_acquire) == 0 || has_exception_.load(std::memory_order_acquire) || !running_;
        });
    }

    if (has_exception_.load(std::memory_order_relaxed)) {
        stop();
        start(); 
        std::lock_guard<std::mutex> lock(exception_mutex_);
        std::rethrow_exception(first_exception_);
    }
}

void GraphRuntime::set_exception(std::exception_ptr e) {
    if (!has_exception_.exchange(true, std::memory_order_acq_rel)) {
        std::lock_guard<std::mutex> lock(exception_mutex_);
        first_exception_ = e;
        running_ = false; 
        cv_task_available_.notify_all();
        {
            std::lock_guard<std::mutex> lk_comp(completion_mutex_);
            cv_completion_.notify_all();
        }
    }
}

} // namespace ps
