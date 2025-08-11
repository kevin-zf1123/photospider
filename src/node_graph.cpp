#include "node_graph.hpp"
#include "ops.hpp"
#include <fstream>
#include <vector>
#include <stack>
#include <algorithm>
#include <unordered_set>
#include <yaml-cpp/emitter.h>
#include <chrono>

namespace ps {

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

const NodeOutput& NodeGraph::compute(int node_id, bool force_recache, bool enable_timing) {
    if (!has_node(node_id)) {
        throw GraphError("Cannot compute: node " + std::to_string(node_id) + " not found.");
    }
    std::unordered_map<int, bool> visiting;
    return compute_internal(node_id, visiting, force_recache, enable_timing);
}

const NodeOutput& NodeGraph::compute_internal(int node_id, std::unordered_map<int, bool>& visiting, bool force_recache, bool enable_timing) {
    auto& node = nodes.at(node_id);
    std::string result_source = "unknown";
    
    // Timer starts at the very beginning of the process for this node.
    auto start_time = std::chrono::high_resolution_clock::now();

    // We use a do-while(false) loop with `break` statements as a structured
    // alternative to `goto` for jumping to the common logging logic at the end.
    do {
        // 1. Check for in-memory cache
        if (!force_recache && node.cached_output.has_value()) {
            result_source = "memory_cache";
            break; // Result is ready, break to the end for logging.
        }

        // 2. Check for on-disk cache
        fs::path metadata_file;
        if (!force_recache && !cache_root.empty() && !node.caches.empty()) {
            for (const auto& cache_entry : node.caches) {
                if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
                    fs::path cache_file = node_cache_dir(node_id) / cache_entry.location;
                    metadata_file = cache_file;
                    metadata_file.replace_extension(".yml");

                    if (fs::exists(cache_file) || fs::exists(metadata_file)) {
                         std::cout << "Loading cache for Node " << node.id << " from: " << fs::relative(node_cache_dir(node_id)).string() << std::endl;
                         NodeOutput loaded_output;
                         if (fs::exists(cache_file)) {
                            loaded_output.image_matrix = cv::imread(cache_file.string(), cv::IMREAD_UNCHANGED);
                         }
                         if (fs::exists(metadata_file)) {
                            YAML::Node meta = YAML::LoadFile(metadata_file.string());
                            for(auto it = meta.begin(); it != meta.end(); ++it) {
                                loaded_output.data[it->first.as<std::string>()] = it->second;
                            }
                         }
                         node.cached_output = std::move(loaded_output);
                         result_source = "disk_cache";
                         break; // Found on disk, break to the end.
                    }
                }
            }
            if (result_source == "disk_cache") break; // Exit the do-while loop if disk cache was loaded
        }
        
        // 3. If no cache hit, proceed with computation
        if (visiting[node_id]) {
            throw GraphError("Cycle detected in graph involving node " + std::to_string(node_id));
        }
        visiting[node_id] = true;

        std::cout << "Computing node " << node.id << " (" << node.name << ")..." << std::endl;
        node.runtime_parameters = node.parameters ? node.parameters : YAML::Node(YAML::NodeType::Map);

        // Compute dependencies recursively...
        for (const auto& p_input : node.parameter_inputs) {
            if (p_input.from_node_id == -1) continue;
            if (!has_node(p_input.from_node_id)) {
                throw GraphError("Node " + std::to_string(node.id) + " has missing parameter dependency: " + std::to_string(p_input.from_node_id));
            }
            const auto& upstream_output = compute_internal(p_input.from_node_id, visiting, force_recache, enable_timing);
            auto it = upstream_output.data.find(p_input.from_output_name);
            if (it == upstream_output.data.end()) {
                throw GraphError("Node " + std::to_string(p_input.from_node_id) + " did not produce required output '" + p_input.from_output_name + "' for node " + std::to_string(node_id));
            }
            node.runtime_parameters[p_input.to_parameter_name] = it->second;
            std::cout << "  - Injected param '" << p_input.to_parameter_name << "' from " << p_input.from_node_id << ":" << p_input.from_output_name << std::endl;
        }

        std::vector<cv::Mat> input_images;
        for (const auto& i_input : node.image_inputs) {
            if (i_input.from_node_id == -1) continue;
            if (!has_node(i_input.from_node_id)) {
                throw GraphError("Node " + std::to_string(node.id) + " has missing image dependency: " + std::to_string(i_input.from_node_id));
            }
            const auto& upstream_output = compute_internal(i_input.from_node_id, visiting, force_recache, enable_timing);
            if (upstream_output.image_matrix.empty()){
                std::cerr << "Warning: Input image from node " << i_input.from_node_id << " is empty for node " << node.id << std::endl;
            }
            input_images.push_back(upstream_output.image_matrix);
        }
        
        // Find and execute the operation function
        auto op_func_opt = OpRegistry::instance().find(node.type, node.subtype);
        if (!op_func_opt) {
            throw GraphError("No operation registered for type=" + node.type + ", subtype=" + node.subtype);
        }
        
        node.cached_output = (*op_func_opt)(node, input_images);
        result_source = "computed";
        save_cache_if_configured(node);
        visiting[node_id] = false;

    } while (false);

    // This is the common exit point for logging.
    if (enable_timing) {
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
        this->timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), result_source});
    }

    return *node.cached_output;
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

static void print_dep_tree_recursive(const NodeGraph& g, std::ostream& os, int node_id, int level, std::unordered_set<int>& path) {
    auto indent = [&](int l) { for (int i = 0; i < l; ++i) os << "  "; };

    if (path.count(node_id)) {
        indent(level);
        os << "- ... (Cycle detected on Node " << node_id << ") ...\n";
        return;
    }
    path.insert(node_id);

    indent(level);
    const auto& node = g.nodes.at(node_id);
    os << "- Node " << node.id << " (" << node.name << " | " << node.type << ":" << node.subtype << ")\n";

    if (node.parameters && node.parameters.IsMap() && node.parameters.size() > 0) {
        YAML::Emitter emitter;
        emitter << YAML::Flow << node.parameters;
        indent(level + 1);
        os << "static_params: " << emitter.c_str() << "\n";
    }

    for (const auto& input : node.image_inputs) {
        if (input.from_node_id != -1 && g.has_node(input.from_node_id)) {
             indent(level + 1);
             os << "(image from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(g, os, input.from_node_id, level + 2, path);
        }
    }
    for (const auto& input : node.parameter_inputs) {
        if (input.from_node_id != -1 && g.has_node(input.from_node_id)) {
            indent(level + 1);
            os << "(param '" << input.to_parameter_name << "' from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(g, os, input.from_node_id, level + 2, path);
        }
    }
    path.erase(node_id);
}

void NodeGraph::print_dependency_tree(std::ostream& os) const {
    os << "Dependency Tree (reversed from ending nodes):\n";
    auto ends = ending_nodes();
    if (ends.empty() && !nodes.empty()) os << "(Graph has cycles or is fully connected)\n";
    else if(nodes.empty()) os << "(Graph is empty)\n";

    for (int end_node_id : ends) {
        std::unordered_set<int> path;
        print_dep_tree_recursive(*this, os, end_node_id, 0, path);
    }
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

// --- THIS IS THE RESTORED FUNCTION DEFINITION ---
fs::path NodeGraph::node_cache_dir(int node_id) const {
    return cache_root / std::to_string(node_id);
}

void NodeGraph::save_cache_if_configured(const Node& node) const {
    if (cache_root.empty() || node.caches.empty() || !node.cached_output.has_value()) {
        return;
    }

    const auto& output = *node.cached_output;

    for (const auto& cache_entry : node.caches) {
        if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
            fs::path dir = node_cache_dir(node.id);
            fs::create_directories(dir);
            fs::path final_path = dir / cache_entry.location;

            if (!output.image_matrix.empty()) {
                std::cout << "Saving cache image for Node " << node.id << " to: " << fs::relative(final_path).string() << std::endl;
                cv::imwrite(final_path.string(), output.image_matrix);
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
                std::cout << "Saving cache metadata for Node " << node.id << " to: " << fs::relative(meta_path).string() << std::endl;
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

void NodeGraph::cache_all_nodes() {
    std::cout << "Saving all in-memory node results to configured caches..." << std::endl;
    int saved_count = 0;
    for (const auto& pair : nodes) {
        if (pair.second.cached_output.has_value()) {
            save_cache_if_configured(pair.second);
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

void NodeGraph::synchronize_disk_cache() {
    std::cout << "Synchronizing disk cache with current memory state..." << std::endl;
    this->cache_all_nodes();
    std::cout << "Checking for orphaned cache files to remove..." << std::endl;
    int removed_count = 0;
    for (const auto& pair : nodes) {
        const Node& node = pair.second;
        if (!node.cached_output.has_value() && !node.caches.empty()) {
            for (const auto& cache_entry : node.caches) {
                 if (!cache_entry.location.empty()) {
                    fs::path cache_file = node_cache_dir(node.id) / cache_entry.location;
                    fs::path meta_file = cache_file; meta_file.replace_extension(".yml");
                    if (fs::exists(cache_file)) { fs::remove(cache_file); removed_count++; }
                    if (fs::exists(meta_file)) { fs::remove(meta_file); removed_count++; }
                 }
            }
        }
    }
    if (removed_count > 0) std::cout << "Removed " << removed_count << " orphaned cache file(s)." << std::endl;
    std::cout << "Disk cache synchronization complete." << std::endl;
}

} // namespace ps