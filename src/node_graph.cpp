// FILE: src/node_graph.cpp
#include "node_graph.hpp"
#include "ops.hpp"
#include <fstream>
#include <vector>
#include <stack>
#include <algorithm>
#include <unordered_set>
#include <yaml-cpp/emitter.h>
#include <chrono>
#include <set>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <queue>


namespace ps {

// --- Private Helper to load from disk cache ---
bool NodeGraph::try_load_from_disk_cache(Node& node) {
    if (node.cached_output.has_value() || cache_root.empty() || node.caches.empty()) {
        return node.cached_output.has_value();
    }
    
    for (const auto& cache_entry : node.caches) {
        if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
            fs::path cache_file = node_cache_dir(node.id) / cache_entry.location;
            fs::path metadata_file = cache_file;
            metadata_file.replace_extension(".yml");

            if (fs::exists(cache_file) || fs::exists(metadata_file)) {
                 if (!quiet_) std::cout << "Loading cache for Node " << node.id << " from: " << fs::relative(node_cache_dir(node.id)).string() << std::endl;
                 NodeOutput loaded_output;
                 if (fs::exists(cache_file)) {
                    cv::Mat loaded_mat = cv::imread(cache_file.string(), cv::IMREAD_UNCHANGED);
                    if (!loaded_mat.empty()) {
                        double scale = 1.0;
                        if (loaded_mat.depth() == CV_8U) scale = 1.0 / 255.0;
                        else if (loaded_mat.depth() == CV_16U) scale = 1.0 / 65535.0;
                        loaded_mat.convertTo(loaded_output.image_matrix, CV_32F, scale);
                        loaded_output.image_umatrix = loaded_output.image_matrix.getUMat(cv::ACCESS_READ);
                    }
                 }
                 if (fs::exists(metadata_file)) {
                    YAML::Node meta = YAML::LoadFile(metadata_file.string());
                    for(auto it = meta.begin(); it != meta.end(); ++it) {
                        loaded_output.data[it->first.as<std::string>()] = it->second;
                    }
                 }
                 node.cached_output = std::move(loaded_output);
                 return true;
            }
        }
    }
    return false;
}

void NodeGraph::execute_op_for_node(int node_id, const std::string& cache_precision, bool enable_timing) {
    auto& node = nodes.at(node_id);
    std::string result_source = "unknown";
    auto start_time = std::chrono::high_resolution_clock::now();

    if (!quiet_) std::cout << "Computing node " << node.id << " (" << node.name << ") on thread " << std::this_thread::get_id() << "..." << std::endl;
    node.runtime_parameters = node.parameters ? node.parameters : YAML::Node(YAML::NodeType::Map);

    for (const auto& p_input : node.parameter_inputs) {
        if (p_input.from_node_id == -1) continue;
        const auto& upstream_output = nodes.at(p_input.from_node_id).cached_output;
        if (!upstream_output) {
             throw GraphError("Internal parallel error: dependency " + std::to_string(p_input.from_node_id) + " not computed for node " + std::to_string(node_id));
        }
        auto it = upstream_output->data.find(p_input.from_output_name);
        if (it == upstream_output->data.end()) {
            throw GraphError("Node " + std::to_string(p_input.from_node_id) + " did not produce required output '" + p_input.from_output_name + "' for node " + std::to_string(node_id));
        }
        node.runtime_parameters[p_input.to_parameter_name] = it->second;
    }

    std::vector<const NodeOutput*> input_node_outputs;
    for (const auto& i_input : node.image_inputs) {
        if (i_input.from_node_id == -1) continue;
        const auto& upstream_output = nodes.at(i_input.from_node_id).cached_output;
        if (!upstream_output) {
             throw GraphError("Internal parallel error: dependency " + std::to_string(i_input.from_node_id) + " not computed for node " + std::to_string(node_id));
        }
        if (upstream_output->image_matrix.empty() && upstream_output->image_umatrix.empty()){
            std::cerr << "Warning: Input image from node " << i_input.from_node_id << " is empty for node " << node.id << std::endl;
        }
        input_node_outputs.push_back(&*upstream_output);
    }

    auto op_func_opt = OpRegistry::instance().find(node.type, node.subtype);
    if (!op_func_opt) {
        throw GraphError("No operation registered for type=" + node.type + ", subtype=" + node.subtype);
    }

    node.cached_output = (*op_func_opt)(node, input_node_outputs);
    result_source = "computed";
    save_cache_if_configured(node, cache_precision);

    if (enable_timing) {
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
        this->timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), result_source});
    }
}


void NodeGraph::clear() {
    nodes.clear();
}

void NodeGraph::add_node(const Node& node) {
    if (has_node(node.id)) {
        throw GraphError("Node with id " + std::to_string(node.id) + " already exists.");
    }
    // Simple cycle detection based on declared inputs. A full check happens during compute.
    std::unordered_set<int> potential_inputs;
    for (const auto& input : node.image_inputs) potential_inputs.insert(input.from_node_id);
    for (const auto& input : node.parameter_inputs) potential_inputs.insert(input.from_node_id);

    for (int input_id : potential_inputs) {
        if (input_id != -1) {
            std::unordered_set<int> visited;
            if (is_ancestor(node.id, input_id, visited)) {
                throw GraphError("Adding node " + std::to_string(node.id) + " creates a cycle.");
            }
        }
    }
    nodes[node.id] = node;
}

bool NodeGraph::has_node(int id) const {
    return nodes.count(id) > 0;
}

void NodeGraph::load_yaml(const fs::path& yaml_path) {
    YAML::Node config;
    try {
        config = YAML::LoadFile(yaml_path.string());
    } catch (const std::exception& e) {
        throw GraphError("Failed to load YAML file " + yaml_path.string() + ": " + e.what());
    }
    if (!config.IsSequence()) throw GraphError("YAML root is not a sequence of nodes.");
    
    clear();
    for (const auto& n : config) {
        Node node = Node::from_yaml(n);
        add_node(node);
    }
}

void NodeGraph::save_yaml(const fs::path& yaml_path) const {
    YAML::Node n(YAML::NodeType::Sequence);
    std::vector<int> sorted_ids;
    for(const auto& pair : nodes) sorted_ids.push_back(pair.first);
    std::sort(sorted_ids.begin(), sorted_ids.end());

    for (int id : sorted_ids) {
        n.push_back(nodes.at(id).to_yaml());
    }

    std::ofstream fout(yaml_path);
    if (!fout) throw GraphError("Failed to open file for writing: " + yaml_path.string());
    fout << n;
}

NodeOutput& NodeGraph::compute(int node_id, const std::string& cache_precision, bool force_recache, bool enable_timing) {
    if (!has_node(node_id)) {
        throw GraphError("Cannot compute: node " + std::to_string(node_id) + " not found.");
    }
    if (force_recache) {
        try {
            auto deps = topo_postorder_from(node_id);
            for (int id : deps) {
                if (nodes.count(id)) {
                    nodes.at(id).cached_output.reset();
                }
            }
        } catch (const GraphError&) {
            // It's possible for a cycle to exist, ignore it for cache clearing.
        }
    }

    std::unordered_map<int, bool> visiting;
    // Call the internal function WITHOUT the force_recache flag.
    return compute_internal(node_id, cache_precision, visiting, enable_timing);
}

NodeOutput& NodeGraph::compute_internal(int node_id, const std::string& cache_precision, std::unordered_map<int, bool>& visiting, bool enable_timing){
    auto& node = nodes.at(node_id);
    std::string result_source = "unknown";
    
    auto start_time = std::chrono::high_resolution_clock::now();

    do {
        if (node.cached_output.has_value()) {
            result_source = "memory_cache";
            break; 
        }

        if (try_load_from_disk_cache(node)) {
            result_source = "disk_cache";
            break;
        }
        
        if (visiting[node_id]) {
            throw GraphError("Cycle detected in graph involving node " + std::to_string(node_id));
        }
        visiting[node_id] = true;

        if (!quiet_) std::cout << "Computing node " << node.id << " (" << node.name << ")..." << std::endl;
        node.runtime_parameters = node.parameters ? node.parameters : YAML::Node(YAML::NodeType::Map);

        for (const auto& p_input : node.parameter_inputs) {
            if (p_input.from_node_id == -1) continue;
            if (!has_node(p_input.from_node_id)) {
                throw GraphError("Node " + std::to_string(node.id) + " has missing parameter dependency: " + std::to_string(p_input.from_node_id));
            }
            const auto& upstream_output = compute_internal(p_input.from_node_id, cache_precision, visiting, enable_timing);
            auto it = upstream_output.data.find(p_input.from_output_name);
            if (it == upstream_output.data.end()) {
                throw GraphError("Node " + std::to_string(p_input.from_node_id) + " did not produce required output '" + p_input.from_output_name + "' for node " + std::to_string(node_id));
            }
            node.runtime_parameters[p_input.to_parameter_name] = it->second;
            if (!quiet_) std::cout << "  - Injected param '" << p_input.to_parameter_name << "' from " << p_input.from_node_id << ":" << p_input.from_output_name << std::endl;
        }

        std::vector<const NodeOutput*> input_node_outputs;
        for (const auto& i_input : node.image_inputs) {
            if (i_input.from_node_id == -1) continue;
            if (!has_node(i_input.from_node_id)) {
                throw GraphError("Node " + std::to_string(node.id) + " has missing image dependency: " + std::to_string(i_input.from_node_id));
            }
            const auto& upstream_output = compute_internal(i_input.from_node_id, cache_precision, visiting, enable_timing);
            if (upstream_output.image_matrix.empty() && upstream_output.image_umatrix.empty()){
                std::cerr << "Warning: Input image from node " << i_input.from_node_id << " is empty for node " << node.id << std::endl;
            }
            input_node_outputs.push_back(&upstream_output);
        }
        
        auto op_func_opt = OpRegistry::instance().find(node.type, node.subtype);
        if (!op_func_opt) {
            throw GraphError("No operation registered for type=" + node.type + ", subtype=" + node.subtype);
        }
        
        node.cached_output = (*op_func_opt)(node, input_node_outputs);
        result_source = "computed";
        save_cache_if_configured(node, cache_precision);
        visiting[node_id] = false;

    } while (false);

    if (enable_timing) {
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
        this->timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), result_source});
    }

    return *node.cached_output;
}

NodeOutput& NodeGraph::compute_parallel(int node_id, const std::string& cache_precision, bool force_recache, bool enable_timing) {
    if (!has_node(node_id)) {
        throw GraphError("Cannot compute: node " + std::to_string(node_id) + " not found.");
    }

    if (force_recache) {
        std::scoped_lock lock(graph_mutex_);
        try {
            auto deps = topo_postorder_from(node_id);
            for (int id : deps) {
                if (nodes.count(id)) {
                    nodes.at(id).cached_output.reset();
                }
            }
        } catch (const GraphError&) {}
    }

    // Do not early-return here; we want to record timing for cached results
    // and prune the work graph before deciding whether to spawn threads.

    std::set<int> subgraph_nodes_set;
    try {
        auto order = topo_postorder_from(node_id);
        subgraph_nodes_set.insert(order.begin(), order.end());
    } catch (const GraphError& e) {
        std::cerr << "Cannot parallelize graph with cycle, falling back to sequential execution. Error: " << e.what() << std::endl;
        return compute(node_id, cache_precision, false, enable_timing);
    }

    // Demand-driven scheduler: only traverse needed branches from the target.
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

    std::mutex final_node_mutex;
    std::condition_variable final_node_cv;
    bool final_node_done = false;

    for (unsigned int i = 0; i < num_threads; ++i) {
        workers.emplace_back([&] {
            while (true) {
                int current_id;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    cv_queue.wait(lock, [&] { return !ready_queue.empty() || done; });
                    if (done && ready_queue.empty()) return;
                    current_id = ready_queue.front();
                    ready_queue.pop();
                }

                // Try satisfy from cache first; otherwise expand inputs; compute when inputs ready.
                bool satisfied = false;
                bool should_compute = false;
                std::vector<int> inputs;
                auto start_time = std::chrono::high_resolution_clock::now();
                {
                    std::scoped_lock lock(graph_mutex_);
                    if (nodes.at(current_id).cached_output) {
                        satisfied = true;
                        if (enable_timing && !cache_timed.count(current_id)) {
                            cache_timed.insert(current_id);
                            auto end_time = std::chrono::high_resolution_clock::now();
                            std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
                            const auto& node = nodes.at(current_id);
                            timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), std::string("memory_cache")});
                        }
                    } else if (!force_recache && try_load_from_disk_cache(nodes.at(current_id))) {
                        satisfied = true;
                        if (enable_timing) {
                            auto end_time = std::chrono::high_resolution_clock::now();
                            std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
                            const auto& node = nodes.at(current_id);
                            timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), std::string("disk_cache")});
                        }
                    } else {
                        // Not satisfied, gather direct inputs.
                        const auto& node = nodes.at(current_id);
                        auto consider = [&](int from_id){ if (from_id != -1 && subgraph_nodes_set.count(from_id)) inputs.push_back(from_id); };
                        for (const auto& i : node.image_inputs) consider(i.from_node_id);
                        for (const auto& p : node.parameter_inputs) consider(p.from_node_id);

                        int pending = 0;
                        for (int child : inputs) {
                            if (nodes.at(child).cached_output) continue;
                            pending++;
                            waiters[child].push_back(current_id);
                        }
                        if (!dependency_count.count(current_id)) dependency_count[current_id] = 0;
                        dependency_count[current_id] = pending;
                        should_compute = (pending == 0);
                    }
                }

                if (satisfied) {
                    std::vector<int> to_notify;
                    {
                        std::scoped_lock lock(graph_mutex_);
                        if (waiters.count(current_id)) to_notify = waiters[current_id];
                    }
                    for (int dep : to_notify) {
                        if (--dependency_count[dep] == 0) {
                            std::scoped_lock q_lock(queue_mutex);
                            ready_queue.push(dep);
                            cv_queue.notify_one();
                        }
                    }

                    if (current_id == node_id) {
                        std::scoped_lock lock(final_node_mutex);
                        final_node_done = true;
                        final_node_cv.notify_one();
                    }
                    continue;
                }

                if (!should_compute) {
                    // Enqueue inputs for processing.
                    {
                        std::scoped_lock q_lock(queue_mutex);
                        for (int child : inputs) {
                            if (scheduled.insert(child).second) {
                                ready_queue.push(child);
                            }
                        }
                        cv_queue.notify_all();
                    }
                    continue;
                }

                // Compute current node now.
                {
                    std::scoped_lock lock(graph_mutex_);
                    if (!nodes.at(current_id).cached_output) {
                        // Will compute below while holding lock per existing pattern
                    }
                }
                {
                    std::scoped_lock lock(graph_mutex_);
                    if (!nodes.at(current_id).cached_output) {
                        execute_op_for_node(current_id, cache_precision, enable_timing);
                    }
                }

                // Notify dependents.
                std::vector<int> to_notify;
                {
                    std::scoped_lock lock(graph_mutex_);
                    if (waiters.count(current_id)) to_notify = waiters[current_id];
                }
                for (int dep : to_notify) {
                    if (--dependency_count[dep] == 0) {
                        std::scoped_lock q_lock(queue_mutex);
                        ready_queue.push(dep);
                        cv_queue.notify_one();
                    }
                }

                if (current_id == node_id) {
                    std::scoped_lock lock(final_node_mutex);
                    final_node_done = true;
                    final_node_cv.notify_one();
                }
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(final_node_mutex);
        final_node_cv.wait(lock, [&]{ return final_node_done || nodes.at(node_id).cached_output.has_value(); });
    }

    done = true;
    cv_queue.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
    
    return *nodes.at(node_id).cached_output;
}


void NodeGraph::clear_timing_results() {
    timing_results.node_timings.clear();
    timing_results.total_ms = 0.0;
}
std::vector<int> NodeGraph::ending_nodes() const {
    std::unordered_set<int> is_input_to_something;
    for (const auto& pair : nodes) {
        for (const auto& input : pair.second.image_inputs) is_input_to_something.insert(input.from_node_id);
        for (const auto& input : pair.second.parameter_inputs) is_input_to_something.insert(input.from_node_id);
    }
    std::vector<int> ends;
    for (const auto& pair : nodes) {
        if (is_input_to_something.find(pair.first) == is_input_to_something.end()) {
            ends.push_back(pair.first);
        }
    }
    return ends;
}

static void print_dep_tree_recursive(const NodeGraph& g, std::ostream& os, int node_id, int level, std::unordered_set<int>& path, bool show_parameters) {
    auto indent = [&](int l) { for (int i = 0; i < l; ++i) os << "  "; };

    // Print a blank line before each node/cycle for readability between siblings and levels.
    os << "\n";

    if (path.count(node_id)) {
        indent(level);
        os << "- ... (Cycle detected on Node " << node_id << ") ...\n";
        return;
    }
    path.insert(node_id);

    indent(level);
    const auto& node = g.nodes.at(node_id);
    os << "- Node " << node.id << " (" << node.name << " | " << node.type << ":" << node.subtype << ")\n";

    if (show_parameters && node.parameters && node.parameters.IsMap() && node.parameters.size() > 0) {
        // Print parameters with proper indentation. For map values, print as nested blocks.
        indent(level + 1);
        os << "static_params:\n";

        // Recursive dump for YAML maps with indentation.
        std::function<void(const YAML::Node&, int)> dump_map = [&](const YAML::Node& m, int lvl) {
            for (auto it = m.begin(); it != m.end(); ++it) {
                indent(lvl);
                std::string key;
                try { key = it->first.as<std::string>(); }
                catch(...) {
                    YAML::Emitter ke;
                    ke << it->first;
                    key = ke.c_str();
                }
                const YAML::Node& val = it->second;
                if (val.IsMap()) {
                    os << key << ":\n";
                    dump_map(val, lvl + 1);
                } else {
                    os << key << ": ";
                    YAML::Emitter ve;
                    // For scalars and sequences, keep compact flow style.
                    ve << YAML::Flow << val;
                    os << ve.c_str() << "\n";
                }
            }
        };

        // Print top-level parameters.
        for (auto it = node.parameters.begin(); it != node.parameters.end(); ++it) {
            const auto& key_node = it->first;
            const auto& val_node = it->second;
            std::string key;
            try { key = key_node.as<std::string>(); }
            catch(...) {
                YAML::Emitter ke;
                ke << key_node;
                key = ke.c_str();
            }

            if (val_node.IsMap()) {
                indent(level + 2);
                os << key << ":\n";
                dump_map(val_node, level + 3);
            } else {
                indent(level + 2);
                os << key << ": ";
                YAML::Emitter ve;
                ve << YAML::Flow << val_node;
                os << ve.c_str() << "\n";
            }
        }
    }

    for (const auto& input : node.image_inputs) {
        if (input.from_node_id != -1 && g.has_node(input.from_node_id)) {
            // Add a blank line before printing the input edge for readability.
            os << "\n";
            indent(level + 1);
            os << "(image from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(g, os, input.from_node_id, level + 2, path, show_parameters);
        }
    }
    for (const auto& input : node.parameter_inputs) {
        if (input.from_node_id != -1 && g.has_node(input.from_node_id)) {
            // Add a blank line before printing the parameter edge for readability.
            os << "\n";
            indent(level + 1);
            os << "(param '" << input.to_parameter_name << "' from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(g, os, input.from_node_id, level + 2, path, show_parameters);
        }
    }
    path.erase(node_id);
}

void NodeGraph::print_dependency_tree(std::ostream& os, bool show_parameters) const {
    os << "Dependency Tree (reversed from ending nodes):\n";
    auto ends = ending_nodes();
    if (ends.empty() && !nodes.empty()) os << "(Graph has cycles or is fully connected)\n";
    else if(nodes.empty()) os << "(Graph is empty)\n";

    for (int end_node_id : ends) {
        std::unordered_set<int> path;
        print_dep_tree_recursive(*this, os, end_node_id, 0, path, show_parameters);
    }
}
void NodeGraph::print_dependency_tree(std::ostream& os, int start_node_id, bool show_parameters) const {
    os << "Dependency Tree (starting from Node " << start_node_id << "):\n";
    if (!has_node(start_node_id)) {
        os << "(Node " << start_node_id << " not found in graph)\n";
        return;
    }
    
    std::unordered_set<int> path;
    print_dep_tree_recursive(*this, os, start_node_id, 0, path, show_parameters);
}

static void topo_postorder_util(const NodeGraph& g, int node_id, std::vector<int>& order, std::unordered_map<int, bool>& visited, std::unordered_map<int, bool>& recursion_stack) {
    visited[node_id] = true;
    recursion_stack[node_id] = true;
    const auto& node = g.nodes.at(node_id);

    auto process_dependencies = [&](int dep_id){
        if (dep_id == -1 || !g.has_node(dep_id)) return;
        if (!visited[dep_id]) {
            topo_postorder_util(g, dep_id, order, visited, recursion_stack);
        } else if (recursion_stack[dep_id]) {
            throw GraphError("Cycle detected in graph during traversal involving " + std::to_string(dep_id));
        }
    };

    for (const auto& input : node.image_inputs) process_dependencies(input.from_node_id);
    for (const auto& input : node.parameter_inputs) process_dependencies(input.from_node_id);
    
    order.push_back(node_id);
    recursion_stack[node_id] = false;
}

std::vector<int> NodeGraph::topo_postorder_from(int end_node_id) const {
    if (!has_node(end_node_id)) {
        throw GraphError("Node " + std::to_string(end_node_id) + " not in graph.");
    }
    std::vector<int> order;
    std::unordered_map<int, bool> visited;
    std::unordered_map<int, bool> recursion_stack;
    topo_postorder_util(*this, end_node_id, order, visited, recursion_stack);
    return order;
}

bool NodeGraph::is_ancestor(int potential_ancestor_id, int node_id, std::unordered_set<int>& visited) const {
    if (potential_ancestor_id == node_id) return true;
    if (visited.count(node_id)) return false;
    visited.insert(node_id);
    if (!has_node(node_id)) return false;

    const auto& node = nodes.at(node_id);
    for (const auto& input : node.image_inputs) {
        if (input.from_node_id != -1 && is_ancestor(potential_ancestor_id, input.from_node_id, visited)) return true;
    }
    for (const auto& input : node.parameter_inputs) {
        if (input.from_node_id != -1 && is_ancestor(potential_ancestor_id, input.from_node_id, visited)) return true;
    }
    return false;
}

fs::path NodeGraph::node_cache_dir(int node_id) const {
    return cache_root / std::to_string(node_id);
}

void NodeGraph::save_cache_if_configured(const Node& node, const std::string& cache_precision) const {
    if (cache_root.empty() || node.caches.empty() || !node.cached_output.has_value()) {
        return;
    }

    const auto& output = *node.cached_output;

    for (const auto& cache_entry : node.caches) {
        if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
            fs::path dir = node_cache_dir(node.id);
            fs::create_directories(dir);
            fs::path final_path = dir / cache_entry.location;

            cv::Mat mat_to_save;
            if (!output.image_umatrix.empty()) {
                mat_to_save = output.image_umatrix.getMat(cv::ACCESS_READ);
            } else if (!output.image_matrix.empty()) {
                mat_to_save = output.image_matrix;
            }

            if (!mat_to_save.empty()) {
                cv::Mat out_mat;
                if (cache_precision == "int16") {
                    mat_to_save.convertTo(out_mat, CV_16U, 65535.0);
                } else { // "int8"
                    mat_to_save.convertTo(out_mat, CV_8U, 255.0);
                }
                if (!quiet_) std::cout << "Saving cache image for Node " << node.id << " to: " << fs::relative(final_path).string() << std::endl;
                cv::imwrite(final_path.string(), out_mat);
            }

            if (!output.data.empty()) {
                fs::path meta_path = final_path;
                meta_path.replace_extension(".yml");
                
                YAML::Node meta_node;
                for(const auto& pair : output.data) {
                    meta_node[pair.first] = pair.second;
                }
                std::ofstream fout(meta_path);
                fout << meta_node;
                if (!quiet_) std::cout << "Saving cache metadata for Node " << node.id << " to: " << fs::relative(meta_path).string() << std::endl;
            }
        }
    }
}

void NodeGraph::clear_drive_cache() {
    if (!cache_root.empty() && fs::exists(cache_root)) {
        uintmax_t n = fs::remove_all(cache_root);
        std::cout << "Successfully cleared on-disk cache '" << cache_root.string() << "'. Removed " << n << " files/directories.\n";
        fs::create_directories(cache_root);
    }
}

void NodeGraph::clear_memory_cache() {
    for (auto& pair : nodes) {
        if (pair.second.cached_output.has_value()) {
            pair.second.cached_output.reset();
        }
    }
    std::cout << "In-memory node caches have been cleared.\n";
}

void NodeGraph::clear_cache() {
    clear_drive_cache();
    clear_memory_cache();
}

void NodeGraph::cache_all_nodes(const std::string& cache_precision) {
    std::cout << "Saving all in-memory node results to configured caches..." << std::endl;
    int saved_count = 0;
    for (const auto& pair : nodes) {
        if (pair.second.cached_output.has_value()) {
            save_cache_if_configured(pair.second, cache_precision);
            saved_count++;
        }
    }
    if (saved_count == 0) std::cout << "No nodes were found in memory to save." << std::endl;
}

void NodeGraph::free_transient_memory() {
    std::cout << "Freeing memory from transient intermediate nodes..." << std::endl;
    auto end_nodes_vec = this->ending_nodes();
    std::unordered_set<int> ending_node_ids(end_nodes_vec.begin(), end_nodes_vec.end());
    int freed_count = 0;
    for (auto& pair : nodes) {
        Node& node = pair.second;
        bool is_in_memory = node.cached_output.has_value();
        bool is_ending_node = ending_node_ids.count(node.id);
        if (is_in_memory && !is_ending_node) {
            node.cached_output.reset();
            freed_count++;
            std::cout << "  - Freed memory for intermediate Node " << node.id << " (" << node.name << ")" << std::endl;
        }
    }
    if (freed_count == 0) std::cout << "No transient nodes were found in memory to free." << std::endl;
}

void NodeGraph::synchronize_disk_cache(const std::string& cache_precision) {
    std::cout << "Synchronizing disk cache with current memory state..." << std::endl;
    this->cache_all_nodes(cache_precision);
    std::cout << "Checking for orphaned cache files to remove..." << std::endl;
    int removed_files = 0;
    int removed_dirs = 0;
    for (const auto& pair : nodes) {
        const Node& node = pair.second;
        if (!node.cached_output.has_value() && !node.caches.empty()) {
            fs::path dir_path = node_cache_dir(node.id);
            if (!fs::exists(dir_path)) {
                continue;
            }
            for (const auto& cache_entry : node.caches) {
                 if (!cache_entry.location.empty()) {
                    fs::path cache_file = node_cache_dir(node.id) / cache_entry.location;
                    fs::path meta_file = cache_file; meta_file.replace_extension(".yml");
                    if (fs::exists(cache_file)) { fs::remove(cache_file); removed_files++; }
                    if (fs::exists(meta_file)) { fs::remove(meta_file); removed_files++; }
                 }
            }
            if (fs::is_empty(dir_path)) {
                std::cout << "  - Removing empty cache directory: " << fs::relative(dir_path).string() << std::endl;
                fs::remove(dir_path);
                removed_dirs++;
            }
        }
    }
    if (removed_files > 0) {
        std::cout << "Removed " << removed_files << " orphaned cache file(s)." << std::endl;
    }
    if (removed_dirs > 0) {
        std::cout << "Removed " << removed_dirs << " empty cache director(y/ies)." << std::endl;
    }
    std::cout << "Disk cache synchronization complete." << std::endl;
}

// --- NEW FUNCTION ---
std::vector<int> NodeGraph::get_trees_containing_node(int node_id) const {
    std::vector<int> result_trees;
    auto all_ends = ending_nodes();
    for (int end_node : all_ends) {
        try {
            auto order = topo_postorder_from(end_node);
            if (std::find(order.begin(), order.end(), node_id) != order.end()) {
                result_trees.push_back(end_node);
            }
        } catch (const GraphError&) {
            // Ignore trees that might have cycles during this specific check
            continue;
        }
    }
    return result_trees;
}

} // namespace ps
