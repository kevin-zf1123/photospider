// NodeGraph parallel compute
#include "node_graph.hpp"
#include <set>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace ps {

NodeOutput& NodeGraph::compute_parallel(int node_id, const std::string& cache_precision, bool force_recache, bool enable_timing) {
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
        return compute(node_id, cache_precision, false, enable_timing);
    }

    std::unordered_map<int, std::atomic<int>> dependency_count;
    std::unordered_map<int, std::vector<int>> waiters;
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
                    cv_queue.wait(lock, [&]{ return done || !ready_queue.empty(); });
                    if (done && ready_queue.empty()) return;
                    current_id = ready_queue.front(); ready_queue.pop();
                }

                auto& node = nodes.at(current_id);

                if (node.cached_output.has_value()) {
                    if (enable_timing && !cache_timed.count(current_id)) {
                        cache_timed.insert(current_id);
                        timing_results.node_timings.push_back({node.id, node.name, 0.0, "memory_cache"});
                    }
                    push_compute_event(node.id, node.name, "memory_cache", 0.0);
                } else if (try_load_from_disk_cache(node)) {
                    if (enable_timing) timing_results.node_timings.push_back({node.id, node.name, 0.0, "disk_cache"});
                    push_compute_event(node.id, node.name, "disk_cache", 0.0);
                } else {
                    // Compute dependencies first
                    int deps_scheduled = 0;
                    for (const auto& input : node.image_inputs) {
                        int dep = input.from_node_id; if (dep == -1) continue;
                        if (!nodes.at(dep).cached_output && !scheduled.count(dep)) {
                            scheduled.insert(dep);
                            {
                                std::scoped_lock lock(queue_mutex);
                                ready_queue.push(dep);
                                cv_queue.notify_one();
                            }
                            deps_scheduled++;
                            waiters[dep].push_back(current_id);
                        }
                    }
                    for (const auto& input : node.parameter_inputs) {
                        int dep = input.from_node_id; if (dep == -1) continue;
                        if (!nodes.at(dep).cached_output && !scheduled.count(dep)) {
                            scheduled.insert(dep);
                            {
                                std::scoped_lock lock(queue_mutex);
                                ready_queue.push(dep);
                                cv_queue.notify_one();
                            }
                            deps_scheduled++;
                            waiters[dep].push_back(current_id);
                        }
                    }
                    if (deps_scheduled > 0) continue;

                    // Execute op for node
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
                    node.cached_output = (*op_func_opt)(node, input_node_outputs);
                    save_cache_if_configured(node, cache_precision);
                    if (enable_timing) {
                        auto end = std::chrono::high_resolution_clock::now();
                        std::chrono::duration<double, std::milli> elapsed = end - start;
                        timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), "computed"});
                        push_compute_event(node.id, node.name, "computed", elapsed.count());
                    } else {
                        push_compute_event(node.id, node.name, "computed", 0.0);
                    }

                    // Wake waiters
                    for (int dep : waiters[current_id]) {
                        {
                            std::scoped_lock lock(queue_mutex);
                            ready_queue.push(dep);
                            cv_queue.notify_one();
                        }
                    }
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
        final_node_cv.wait(lock, [&]{ return final_node_done || nodes.at(node_id).cached_output.has_value(); });
    }

    done = true; cv_queue.notify_all();
    for (auto& worker : workers) worker.join();
    if (error_flag && first_error) {
        std::rethrow_exception(first_error);
    }
    return *nodes.at(node_id).cached_output;
}

} // namespace ps
