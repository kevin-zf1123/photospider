// FILE: src/kernel/node_module/node_graph_parallel.cpp (重构后的完整文件)

#include "node_graph.hpp"
#include "kernel/graph_runtime.hpp"
#include <algorithm>
#include <vector>
#include <map>
#include <atomic>
#include <exception>
#include <random>

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
                    if (!node_to_clear.preserved || id == node_id) {
                        node_to_clear.cached_output.reset();
                    }
                }
            }
        } catch (const GraphError&) {}
    }

    auto task_graph_ptr = std::make_shared<TaskGraph>();
    auto& task_graph = *task_graph_ptr;
    auto execution_order = topo_postorder_from(node_id);

    std::map<int, int> dep_counts;
    for (int current_node_id : execution_order) {
        auto& node = nodes.at(current_node_id);
        int num_deps = 0;
        
        auto count_dep = [&](int dep_id) {
            if (dep_id != -1 && has_node(dep_id)) {
                if (std::find(execution_order.begin(), execution_order.end(), dep_id) != execution_order.end()) {
                    task_graph.dependents_map[dep_id].push_back(current_node_id);
                    num_deps++;
                }
            }
        };
        
        for (const auto& input : node.image_inputs) count_dep(input.from_node_id);
        for (const auto& input : node.parameter_inputs) count_dep(input.from_node_id);

        dep_counts[current_node_id] = num_deps;
        if (num_deps == 0) {
            task_graph.initial_ready_nodes.push_back(current_node_id);
        }
    }

    for(auto const& [key, val] : dep_counts) {
        task_graph.dependency_counters[key] = val;
    }

    for (int current_node_id : execution_order) {
        task_graph.tasks[current_node_id] = [
            this, &runtime, task_graph_ptr,
            id = current_node_id, 
            cache_precision, enable_timing, disable_disk_cache, benchmark_events
        ]() {
            static thread_local std::mt19937 generator(std::random_device{}());
            std::uniform_int_distribution<int> distribution(0, std::thread::hardware_concurrency() - 1);
            int thread_id = distribution(generator);
            
            try {
                std::unordered_map<int, bool> visiting;
                this->compute_internal(id, cache_precision, visiting, enable_timing, !disable_disk_cache, benchmark_events);

                if (task_graph_ptr->dependents_map.count(id)) {
                    for (int dependent_id : task_graph_ptr->dependents_map.at(id)) {
                        if (--task_graph_ptr->dependency_counters.at(dependent_id) == 0) {
                            runtime.push_ready_task(std::move(task_graph_ptr->tasks.at(dependent_id)));
                        }
                    }
                }
            } catch (...) {
                runtime.set_exception(std::current_exception());
            }
            runtime.notify_task_complete();
        };
    }
    
    runtime.execute_task_graph_and_wait(task_graph_ptr);

    if (enable_timing) {
        double total = 0.0;
        for(const auto& timing : timing_results.node_timings) {
            total += timing.elapsed_ms;
        }
        timing_results.total_ms = total;
    }

    return *nodes.at(node_id).cached_output;
}

} // namespace ps