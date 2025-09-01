// NodeGraph parallel compute
//
// Scheduler overview:
// - Per compute invocation, we build a local ready queue and spawn a small worker pool.
// - Each node runs exactly once per invocation; a node becomes ready only when all its inputs are ready.
// - We track dependencies with two maps guarded by queue_mutex:
//     waiters[dep] -> set of dependents waiting for 'dep' to finish
//     dependency_count[node] -> number of unfinished dependencies for 'node'
// - When a node completes (either from memory_cache, disk_cache, or computed), we decrement the counter of
//   each dependent and push it to the ready queue only when its counter reaches zero. This prevents premature
//   execution and duplicate scheduling.
// - The worker threads exit cleanly when the final node completes or on error; the main thread joins all workers.
#include "node_graph.hpp"
#include <set>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <variant>

namespace ps {

NodeOutput& NodeGraph::compute_parallel(int node_id, const std::string& cache_precision,
                                        bool force_recache, bool enable_timing,
                                        bool disable_disk_cache) {
    if (!has_node(node_id)) {
        throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
    }

    if (force_recache) {
        std::scoped_lock lock(graph_mutex_);
        try {
            auto deps = topo_postorder_from(node_id);
            for (int id : deps) {
                if (nodes.count(id)) nodes.at(id).cached_output.reset();
            }
        } catch (const GraphError&) {}
    }

    std::set<int> subgraph_nodes_set;
    try {
        auto order = topo_postorder_from(node_id);
        subgraph_nodes_set.insert(order.begin(), order.end());
    } catch (const GraphError&) {
        return compute(node_id, cache_precision, false, enable_timing, disable_disk_cache);
    }

    // queue_mutex protects: ready_queue, scheduled, waiters, dependency_count
    std::unordered_map<int, std::atomic<int>> dependency_count;
    std::unordered_map<int, std::unordered_set<int>> waiters;
    std::unordered_set<int> scheduled;
    std::unordered_set<int> cache_timed;
    std::queue<int> ready_queue;
    std::mutex queue_mutex;
    std::condition_variable cv_queue;
    {
        std::scoped_lock lock(queue_mutex);
        ready_queue.push(node_id);
    }
    scheduled.insert(node_id);

    unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    std::atomic<bool> done = false;
    std::atomic<bool> error_flag = false;
    std::exception_ptr first_error;
    std::mutex error_mutex;

    std::mutex final_node_mutex;
    std::condition_variable final_node_cv;
    bool final_node_done = false;

    for (unsigned int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]{
            try {
            while (true) {
                int current_id = -1;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    // Wake when new work arrives or when shutting down.
                    cv_queue.wait(lock, [&]{ return done || !ready_queue.empty(); });
                    if (done && ready_queue.empty()) return;
                    current_id = ready_queue.front(); ready_queue.pop();
                }

                auto& node = nodes.at(current_id);
                // Must hold queue_mutex inside this helper (it acquires it itself) to update dependents safely.
                auto notify_dependents = [&](int finished_id){
                    std::scoped_lock lock(queue_mutex);
                    auto itw = waiters.find(finished_id);
                    if (itw == waiters.end()) return;
                    for (int dep_node : itw->second) {
                        int new_count = --dependency_count[dep_node];
                        if (new_count == 0) {
                            ready_queue.push(dep_node);
                            cv_queue.notify_one();
                        }
                    }
                    waiters.erase(itw);
                };

                if (node.cached_output.has_value()) {
                    if (enable_timing && !cache_timed.count(current_id)) {
                        std::lock_guard<std::mutex> lk(timing_mutex_);
                        cache_timed.insert(current_id);
                        timing_results.node_timings.push_back({node.id, node.name, 0.0, "memory_cache"});
                    }
                    push_compute_event(node.id, node.name, "memory_cache", 0.0);
                    notify_dependents(current_id);
                } else if (!disable_disk_cache && try_load_from_disk_cache(node)) {
                    if (enable_timing) {
                        std::lock_guard<std::mutex> lk(timing_mutex_);
                        timing_results.node_timings.push_back({node.id, node.name, 0.0, "disk_cache"});
                    }
                    push_compute_event(node.id, node.name, "disk_cache", 0.0);
                    notify_dependents(current_id);
                } else {
                    // Check dependencies readiness; schedule missing ones and wait until all ready.
                    // Re-evaluate and register atomically under queue_mutex to avoid races with finishing deps.
                    std::vector<int> all_deps;
                    all_deps.reserve(node.image_inputs.size() + node.parameter_inputs.size());
                    for (const auto& input : node.image_inputs) { if (input.from_node_id != -1) all_deps.push_back(input.from_node_id); }
                    for (const auto& input : node.parameter_inputs) { if (input.from_node_id != -1) all_deps.push_back(input.from_node_id); }

                    int unresolved = 0;
                    {
                        std::scoped_lock lock(queue_mutex);
                        for (int dep : all_deps) {
                            if (!nodes.count(dep)) throw GraphError(GraphErrc::MissingDependency, "Missing dependency: " + std::to_string(dep));
                            if (!nodes.at(dep).cached_output) {
                                unresolved++;
                                waiters[dep].insert(current_id);
                                if (!scheduled.count(dep)) {
                                    scheduled.insert(dep);
                                    ready_queue.push(dep);
                                    cv_queue.notify_one();
                                }
                            }
                        }
                        if (unresolved > 0) {
                            dependency_count[current_id] = unresolved;
                        }
                    }
                    if (unresolved > 0) continue;

                    // Execute op for node (at this point, all inputs are ready and immutable for this invocation).
                    auto start = std::chrono::high_resolution_clock::now();
                    std::vector<const NodeOutput*> input_node_outputs;
                    for (const auto& i_input : node.image_inputs) {
                        if (i_input.from_node_id != -1 && nodes.count(i_input.from_node_id)) {
                            input_node_outputs.push_back(&*nodes.at(i_input.from_node_id).cached_output);
                        }
                    }
                    auto op_func_opt = OpRegistry::instance().find(node.type, node.subtype);
                    if (!op_func_opt) {
                        throw GraphError(GraphErrc::NoOperation, "No operation registered for type=" + node.type + ", subtype=" + node.subtype);
                    }

                    // 🔴 REMOVE: node.cached_output = (*op_func_opt)(node, input_node_outputs);

                    // ✅ ADD: 使用 std::visit 来处理 variant
                    std::visit([&](auto&& op_func) {
                        using T = std::decay_t<decltype(op_func)>;
                        if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                            node.cached_output = op_func(node, input_node_outputs);
                        } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                            // 并行引擎目前也不支持分块调度，这将在阶段四完成
                            throw GraphError(GraphErrc::ComputeError, "Tiled operation found in parallel non-tiled compute engine. Stage 4 required.");
                        }
                    }, *op_func_opt);


                    save_cache_if_configured(node, cache_precision);
                    if (enable_timing) {
                        auto end = std::chrono::high_resolution_clock::now();
                        std::chrono::duration<double, std::milli> elapsed = end - start;
                        {
                            std::lock_guard<std::mutex> lk(timing_mutex_);
                            timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), "computed"});
                            timing_results.total_ms += elapsed.count();
                        }
                        push_compute_event(node.id, node.name, "computed", elapsed.count());
                    } else {
                        push_compute_event(node.id, node.name, "computed", 0.0);
                    }

                    notify_dependents(current_id);
                }

                if (current_id == node_id) {
                    std::scoped_lock lock(final_node_mutex);
                    final_node_done = true; final_node_cv.notify_one();
                }
            }
            } catch (...) {
                {
                    std::scoped_lock lk(error_mutex);
                    if (!error_flag) {
                        error_flag = true;
                        first_error = std::current_exception();
                    }
                }
                done = true;
                cv_queue.notify_all();
                return;
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(final_node_mutex);
        final_node_cv.wait(lock, [&]{ return final_node_done || nodes.at(node_id).cached_output.has_value() || error_flag; });
    }

    // Signal workers to stop and wait for all to exit before returning.
    done = true; cv_queue.notify_all();
    final_node_cv.notify_all();
    for (auto& worker : workers) worker.join();
    if (error_flag && first_error) {
        std::rethrow_exception(first_error);
    }
    return *nodes.at(node_id).cached_output;
}

} // namespace ps
