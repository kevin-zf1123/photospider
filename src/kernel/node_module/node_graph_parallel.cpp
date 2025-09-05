// FILE: src/kernel/node_module/node_graph_parallel.cpp (重构后的完整文件)

#include "node_graph.hpp"
#include "kernel/graph_runtime.hpp"
#include <algorithm>
#include <vector>
#include <map>
#include <atomic>
#include <exception>
#include <random>
#include <unordered_set>
#include <unordered_map>

namespace ps {

NodeOutput& NodeGraph::compute_parallel(
    GraphRuntime& runtime,
    int node_id, 
    const std::string& cache_precision,
    bool force_recache, 
    bool enable_timing,
    bool disable_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events)
{
    // [健壮性修复] 将整个设置过程包裹在 try...catch 中
    try {
        if (!has_node(node_id)) {
            throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
        }

        if (enable_timing) {
            clear_timing_results();
            total_io_time_ms = 0.0;
        }

        auto execution_order = topo_postorder_from(node_id);
        std::unordered_set<int> execution_set(execution_order.begin(), execution_order.end());

        if (force_recache) {
            std::scoped_lock lock(graph_mutex_);
            for (int id : execution_order) {
                if (nodes.count(id)) {
                    auto& node_to_clear = nodes.at(id);
                    if (!node_to_clear.preserved || id == node_id) {
                        node_to_clear.cached_output.reset();
                    }
                }
            }
        }
        
        // --- 核心修复：建立稀疏 ID 到稠密索引的映射 ---
        std::unordered_map<int, int> id_to_idx;
        id_to_idx.reserve(execution_order.size());
        for (size_t i = 0; i < execution_order.size(); ++i) {
            id_to_idx[execution_order[i]] = i;
        }

        size_t num_nodes = execution_order.size();
        
        // --- 使用基于稠密索引的 std::vector 进行依赖管理 ---
        std::vector<std::atomic<int>> dependency_counters(num_nodes);
        std::vector<std::vector<int>> dependents_map(num_nodes);
        std::vector<Task> all_tasks;
        all_tasks.resize(num_nodes);
        
        for (size_t i = 0; i < num_nodes; ++i) {
            int current_node_id = execution_order[i];
            auto& node = nodes.at(current_node_id);
            int num_deps = 0;
            
            auto count_dep = [&](int dep_id) {
                if (dep_id != -1 && execution_set.count(dep_id)) {
                    // 使用稠密索引构建依赖关系
                    int dep_idx = id_to_idx.at(dep_id);
                    dependents_map[dep_idx].push_back(i); // i 是当前节点的稠密索引
                    num_deps++;
                }
            };
            
            for (const auto& input : node.image_inputs) count_dep(input.from_node_id);
            for (const auto& input : node.parameter_inputs) count_dep(input.from_node_id);

            dependency_counters[i] = num_deps;
        }
        
        // --- 为每个节点（按稠密索引）创建任务 ---
        for (size_t i = 0; i < num_nodes; ++i) {
            int current_node_id = execution_order[i];
            int current_node_idx = i;

            auto inner_task = [this, &runtime, &dependency_counters, &dependents_map, &execution_order, &all_tasks,
                               current_node_id, current_node_idx,
                               cache_precision, enable_timing, disable_disk_cache, benchmark_events] () {
                try {
                    // 1. 执行核心计算
                    std::unordered_map<int, bool> visiting;
                    compute_internal(current_node_id, cache_precision, visiting, enable_timing, !disable_disk_cache, benchmark_events);

                    // 2. 触发后续依赖任务
                    for (int dependent_idx : dependents_map[current_node_idx]) {
                        if (--dependency_counters[dependent_idx] == 0) {
                            runtime.submit_ready_task_any_thread(std::move(all_tasks[dependent_idx]));
                        }
                    }
                } catch (const cv::Exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + std::string(e.what()))
                    ));
                } catch (const std::exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                    ));
                } catch (...) {
                     runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
                    ));
                }
            };
            
            // --- 包装任务以处理异常和完成计数 ---
            all_tasks[i] = [inner_task = std::move(inner_task), &runtime]() {
                inner_task();
                runtime.dec_graph_tasks_to_complete();
            };
        }
        
        // --- 提交初始就绪任务 ---
        std::vector<Task> initial_tasks;
        for (size_t i = 0; i < num_nodes; ++i) {
            if (dependency_counters[i] == 0) {
                initial_tasks.push_back(std::move(all_tasks[i]));
            }
        }

        if (execution_order.empty() && has_node(node_id)) {
             // 处理图中只有一个节点的情况
            if(!nodes.at(node_id).cached_output.has_value()){
                 std::unordered_map<int, bool> visiting;
                 compute_internal(node_id, cache_precision, visiting, enable_timing, !disable_disk_cache, benchmark_events);
            }
        } else {
            runtime.submit_initial_tasks(std::move(initial_tasks), execution_order.size());
            runtime.wait_for_completion();
        }


        // --- 后续处理（计时、结果返回） ---
        if (enable_timing) {
            double total = 0.0;
            {
                std::lock_guard lk(timing_mutex_);
                for(const auto& timing : timing_results.node_timings) {
                    total += timing.elapsed_ms;
                }
                timing_results.total_ms = total;
            }
        }

        if (!nodes.at(node_id).cached_output) {
            throw GraphError(GraphErrc::ComputeError, "Parallel computation finished but target node has no output. An upstream error likely occurred.");
        }
        return *nodes.at(node_id).cached_output;

    } catch (...) {
        // 捕获在本函数内（任务提交前）抛出的异常，并传递给运行时
        runtime.set_exception(std::current_exception());
        // 等待运行时处理异常并唤醒
        runtime.wait_for_completion();
        // wait_for_completion 内部会重新抛出异常，这里我们只需确保函数有返回值
        // 在实际情况下，由于 rethrow，代码不会执行到这里
        throw GraphError(GraphErrc::Unknown, "Caught pre-flight exception during parallel compute setup.");
    }
}

} // namespace ps