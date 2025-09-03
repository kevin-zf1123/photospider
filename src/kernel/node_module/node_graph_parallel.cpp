// FILE: src/kernel/node_module/node_graph_parallel.cpp (修正后的完整文件)

#include "node_graph.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include <set>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <variant>
#include <atomic>
#include <future>
#include <map>
#include <vector>

// 声明在 node_graph_compute.cpp 中定义的辅助函数
namespace ps {
    void execute_tile_task(const ps::TileTask& task, const TileOpFunc& tiled_op);
    cv::Rect calculate_halo(const cv::Rect& roi, int halo_size, const cv::Size& bounds);
}

namespace ps {

NodeOutput& NodeGraph::compute_parallel(int node_id, const std::string& cache_precision,
                                        bool force_recache, bool enable_timing,
                                        bool disable_disk_cache,
                                        std::vector<BenchmarkEvent>* benchmark_events)
{
    if (!has_node(node_id)) {
        throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
    }

    if (enable_timing) {
        clear_timing_results();
        total_io_time_ms = 0.0;
    }

    if (force_recache) {
        std::scoped_lock lock(graph_mutex_);
        try {
            auto deps = topo_postorder_from(node_id);
            for (int id : deps) {
                if (nodes.count(id)) {
                    auto& node_to_clear = nodes.at(id);
                    // 应用 Preserved 逻辑
                    if (!node_to_clear.preserved || id == node_id) {
                        node_to_clear.cached_output.reset();
                    }
                }
            }
        } catch (const GraphError&) {}
    }

    // --- 并行调度器核心逻辑 ---

    std::vector<int> execution_order;
    try {
        execution_order = topo_postorder_from(node_id);
    } catch (const GraphError& e) {
        throw GraphError(GraphErrc::Cycle, "Cannot parallel compute: " + std::string(e.what()));
    }
    
    // 依赖管理
    // [修正] 将 std::atomic<int> 改为 int，并用 mutex 保护
    std::map<int, int> dependency_count;
    std::map<int, std::vector<int>> dependents;
    std::queue<int> ready_queue;
    std::mutex queue_mutex;
    std::condition_variable cv_queue;
    std::atomic<int> completed_node_count = 0;

    // 1. 构建依赖图
    for (int current_node_id : execution_order) {
        auto& node = nodes.at(current_node_id);
        int num_deps = 0;
        
        auto process_dep = [&](int dep_id) {
            if (dep_id != -1 && has_node(dep_id)) {
                // 确保依赖节点也在当前的计算子图中
                if (std::find(execution_order.begin(), execution_order.end(), dep_id) != execution_order.end()) {
                    dependents[dep_id].push_back(current_node_id);
                    num_deps++;
                }
            }
        };
        
        for (const auto& input : node.image_inputs) process_dep(input.from_node_id);
        for (const auto& input : node.parameter_inputs) process_dep(input.from_node_id);

        dependency_count[current_node_id] = num_deps;

        if (num_deps == 0) {
            ready_queue.push(current_node_id);
        }
    }

    // 2. 启动工作线程
    unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    std::atomic<bool> done = false;
    // [新增] 异常安全处理机制
    std::atomic<bool> error_occurred = false;
    std::exception_ptr first_exception;
    std::mutex error_mutex;

    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&]{
            while (true) {
                int current_id = -1;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    cv_queue.wait(lock, [&]{ return !ready_queue.empty() || done.load(); });

                    if (done) return;
                    
                    current_id = ready_queue.front();
                    ready_queue.pop();
                }

                if (error_occurred) continue;

                try {
                    // [修正] 每个任务的 compute_internal 调用都有自己的 visiting map
                    std::unordered_map<int, bool> visiting;
                    compute_internal(current_id, cache_precision, visiting, enable_timing, !disable_disk_cache, benchmark_events);
                    
                    completed_node_count++;
                    
                    // 完成通知 (现在受 mutex 保护)
                    if (dependents.count(current_id)) {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        for (int dependent_id : dependents.at(current_id)) {
                            if (--dependency_count.at(dependent_id) == 0) {
                                ready_queue.push(dependent_id);
                                cv_queue.notify_one();
                            }
                        }
                    }
                    
                    // [修正] 检查是否所有节点都已完成
                    if (completed_node_count == execution_order.size()) {
                        done = true;
                        cv_queue.notify_all();
                    }

                } catch (...) {
                    // [新增] 捕获第一个发生的异常
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!error_occurred) {
                        first_exception = std::current_exception();
                        error_occurred = true;
                        done = true;
                        cv_queue.notify_all(); // 唤醒所有线程，让它们退出
                    }
                }
            }
        });
    }

    // 3. 等待最终节点完成或出错
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        cv_queue.wait(lock, [&]{ return done.load(); });
    }

    // 4. 清理工作线程
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // [新增] 如果有异常发生，在主线程中重新抛出
    if (error_occurred && first_exception) {
        std::rethrow_exception(first_exception);
    }
    
    if (enable_timing) {
        double total = 0.0;
        // timing_results 内部有自己的 timing_mutex_ 保护
        for(const auto& timing : timing_results.node_timings) {
            total += timing.elapsed_ms;
        }
        timing_results.total_ms = total;
    }

    return *nodes.at(node_id).cached_output;
}

} // namespace ps