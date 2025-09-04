// Photospider kernel: GraphRuntime implementation (Objective-C++)
#include "kernel/graph_runtime.hpp"

#include <filesystem>
#include <random>

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

namespace ps {

// 在 .mm 文件中定义 GpuContext 结构体，以隐藏 Metal 细节
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
        // 在无法获取设备时打印警告，避免静默失败
        // TODO: 使用更正式的日志系统
        fprintf(stderr, "Warning: Could not create default Metal device.\n");
    }
#else
    // 在非 Apple 平台上，gpu_context_ 保持为空
#endif
}

GraphRuntime::~GraphRuntime() { 
    stop(); 
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
    running_ = true;

    unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    local_task_queues_.resize(num_threads);
    local_queue_mutexes_.reserve(num_threads);
    for (unsigned int i = 0; i < num_threads; ++i) {
        local_queue_mutexes_.push_back(std::make_unique<std::mutex>());
    }

    for (unsigned int i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&GraphRuntime::run_loop, this, i);
    }
}

void GraphRuntime::stop() {
    if (!running_) return;
    
    running_ = false;
    cv_task_available_.notify_all();
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void GraphRuntime::run_loop(int thread_id) {
    while (running_) {
        Task task;
        bool found_task = false;

        {
            std::lock_guard<std::mutex> lock(exception_mutex_);
            if (first_exception_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lock(*local_queue_mutexes_[thread_id]);
            if (!local_task_queues_[thread_id].empty()) {
                task = std::move(local_task_queues_[thread_id].front());
                local_task_queues_[thread_id].pop_front();
                found_task = true;
            }
        }

        if (!found_task) {
            int num_threads = workers_.size();
            if (num_threads > 1) {
                std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<int> dist(0, num_threads - 2);
                int victim_offset = dist(rng) + 1;
                int victim_thread = (thread_id + victim_offset) % num_threads;
            
                std::lock_guard<std::mutex> lock(*local_queue_mutexes_[victim_thread]);
                if (!local_task_queues_[victim_thread].empty()) {
                    task = std::move(local_task_queues_[victim_thread].back());
                    local_task_queues_[victim_thread].pop_back();
                    found_task = true;
                }
            }
        }

        if (!found_task) {
            std::unique_lock<std::mutex> lock(global_queue_mutex_);
            // 使用无条件的 wait，线程会一直休眠直到被唤醒
            cv_task_available_.wait(lock, [&]{ 
                return !global_task_queue_.empty() || !running_; 
            });
            
            if (!running_) return;

            // 此时可以确信队列非空（或程序正在退出）
            if (!global_task_queue_.empty()) {
                task = std::move(global_task_queue_.front());
                global_task_queue_.pop();
                found_task = true;
            }
        }
        
        if (found_task && task) {
            task();
        }
    }
}

void GraphRuntime::push_ready_task(Task&& task) {
    {
        std::lock_guard<std::mutex> lock(global_queue_mutex_);
        global_task_queue_.push(std::move(task));
    }
    // 唤醒一个等待全局队列的线程
    cv_task_available_.notify_one();
}

void GraphRuntime::notify_task_complete() {
    if (--tasks_remaining_ == 0) {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        cv_completion_.notify_one();
    }
}

void GraphRuntime::set_exception(std::exception_ptr e) {
    std::lock_guard<std::mutex> lock(exception_mutex_);
    if (!first_exception_) {
        first_exception_ = e;
        std::lock_guard<std::mutex> cv_lock(completion_mutex_);
        cv_completion_.notify_one();
    }
}

void GraphRuntime::execute_task_graph_and_wait(std::shared_ptr<TaskGraph> task_graph) {
    if (task_graph->tasks.empty()) return;
    
    first_exception_ = nullptr;
    tasks_remaining_ = task_graph->tasks.size();
    
    {
        std::lock_guard<std::mutex> lock(global_queue_mutex_);
        for (int node_id : task_graph->initial_ready_nodes) {
            global_task_queue_.push(std::move(task_graph->tasks.at(node_id)));
        }
    }
    cv_task_available_.notify_all();

    {
        std::unique_lock<std::mutex> lock(completion_mutex_);
        cv_completion_.wait(lock, [&]{ 
            return tasks_remaining_.load() == 0 || first_exception_ != nullptr;
        });
    }

    if (first_exception_) {
        std::rethrow_exception(first_exception_);
    }
}

} // namespace ps