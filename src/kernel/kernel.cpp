// Photospider kernel: Kernel implementation
#include "kernel/kernel.hpp"

#include <filesystem>
#include <iostream>

namespace ps {

std::optional<std::string> Kernel::load_graph(const std::string& name,
                                              const std::string& root_dir,
                                              const std::string& yaml_path,
                                              const std::string& config_path) {
    if (graphs_.count(name)) return std::nullopt;
    // Always place sessions under the provided root_dir/name (CLI passes "sessions")
    GraphRuntime::Info info{ name, std::filesystem::path(root_dir) / name,
                             yaml_path, config_path };
    auto rt = std::make_unique<GraphRuntime>(info);
    // Ensure per-graph files are present under the root for isolation
    try {
        std::filesystem::create_directories(info.root);
        auto yaml_target = info.root / "content.yaml"; // content.yaml per requirement
        // Always refresh content.yaml from source
        if (!info.yaml.empty() && std::filesystem::exists(info.yaml)) {
            std::filesystem::copy_file(info.yaml, yaml_target, std::filesystem::copy_options::overwrite_existing);
        }
        // Copy config into session as config.yaml if provided
        if (!config_path.empty() && std::filesystem::exists(config_path)) {
            auto cfg_target = info.root / "config.yaml";
            std::filesystem::copy_file(config_path, cfg_target, std::filesystem::copy_options::overwrite_existing);
        }
    } catch (...) {
        // Non-fatal; continue with original yaml path
    }
    // Start worker thread before posting any jobs to avoid deadlock
    rt->start();
    // Load YAML synchronously on worker
    try {
        rt->post([yaml = info.root / "content.yaml", fallback = info.yaml](NodeGraph& g){
            if (std::filesystem::exists(yaml)) g.load_yaml(yaml);
            else g.load_yaml(fallback);
            return 0;
        }).get();
    } catch (const std::exception& e) {
        std::cerr << "Failed to load YAML for graph '" << name << "': " << e.what() << std::endl;
        return std::nullopt;
    }
    graphs_[name] = std::move(rt);
    return name;
}

bool Kernel::close_graph(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    it->second->stop();
    graphs_.erase(it);
    return true;
}

std::vector<std::string> Kernel::list_graphs() const {
    std::vector<std::string> names;
    names.reserve(graphs_.size());
    for (const auto& kv : graphs_) names.push_back(kv.first);
    return names;
}

bool Kernel::compute(const std::string& name, int node_id, const std::string& cache_precision,
                     bool force_recache, bool enable_timing, bool parallel, bool quiet) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    it->second->post([=](NodeGraph& g){
        g.clear_timing_results();
        bool prev_quiet = g.is_quiet();
        g.set_quiet(quiet);
        if (parallel) g.compute_parallel(node_id, cache_precision, force_recache, enable_timing);
        else g.compute(node_id, cache_precision, force_recache, enable_timing);
        g.set_quiet(prev_quiet);
        return 0;
    });
    return true;
}

std::optional<TimingCollector> Kernel::get_timing(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([](NodeGraph& g){ return g.timing_results; }).get();
    } catch (...) {
        return std::nullopt;
    }
}

bool Kernel::reload_graph_yaml(const std::string& name, const std::string& yaml_path) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([=](NodeGraph& g){ g.load_yaml(yaml_path); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::save_graph_yaml(const std::string& name, const std::string& yaml_path) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([=](NodeGraph& g){ g.save_yaml(yaml_path); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::clear_drive_cache(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([](NodeGraph& g){ g.clear_drive_cache(); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::clear_memory_cache(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([](NodeGraph& g){ g.clear_memory_cache(); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::clear_cache(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([](NodeGraph& g){ g.clear_cache(); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::cache_all_nodes(const std::string& name, const std::string& cache_precision) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([=](NodeGraph& g){ g.cache_all_nodes(cache_precision); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::free_transient_memory(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([](NodeGraph& g){ g.free_transient_memory(); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::synchronize_disk_cache(const std::string& name, const std::string& cache_precision) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([=](NodeGraph& g){ g.synchronize_disk_cache(cache_precision); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::clear_graph(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([](NodeGraph& g){ g.clear(); return 0; }).get(); return true; }
    catch (...) { return false; }
}

std::optional<std::string> Kernel::dump_dependency_tree(const std::string& name, std::optional<int> node_id, bool show_parameters) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([=](NodeGraph& g){ std::stringstream ss; if (node_id) g.print_dependency_tree(ss, *node_id, show_parameters); else g.print_dependency_tree(ss, show_parameters); return ss.str(); }).get();
    } catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::ending_nodes(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try { return it->second->post([](NodeGraph& g){ return g.ending_nodes(); }).get(); }
    catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::topo_postorder_from(const std::string& name, int end_node_id) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try { return it->second->post([=](NodeGraph& g){ return g.topo_postorder_from(end_node_id); }).get(); }
    catch (...) { return std::nullopt; }
}

std::optional<std::map<int, std::vector<int>>> Kernel::traversal_orders(const std::string& name) {
    auto ends = ending_nodes(name);
    if (!ends) return std::nullopt;
    std::map<int, std::vector<int>> out;
    for (int end : *ends) {
        auto order = topo_postorder_from(name, end);
        if (!order) return std::nullopt;
        out[end] = *order;
    }
    return out;
}

std::optional<cv::Mat> Kernel::compute_and_get_image(const std::string& name, int node_id, const std::string& cache_precision,
                                                     bool force_recache, bool enable_timing, bool parallel) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([=](NodeGraph& g){
            g.clear_timing_results();
            const auto& out = parallel ? g.compute_parallel(node_id, cache_precision, force_recache, enable_timing)
                                       : g.compute(node_id, cache_precision, force_recache, enable_timing);
            // Prefer UMat if available, else Mat
            if (!out.image_umatrix.empty()) return out.image_umatrix.getMat(cv::ACCESS_READ).clone();
            return out.image_matrix.clone();
        }).get();
    } catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::trees_containing_node(const std::string& name, int node_id) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([=](NodeGraph& g){ return g.get_trees_containing_node(node_id); }).get();
    } catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::list_node_ids(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([](NodeGraph& g){
            std::vector<int> ids; ids.reserve(g.nodes.size());
            for (auto& kv : g.nodes) ids.push_back(kv.first);
            std::sort(ids.begin(), ids.end());
            return ids;
        }).get();
    } catch (...) { return std::nullopt; }
}

std::optional<std::string> Kernel::get_node_yaml(const std::string& name, int node_id) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([=](NodeGraph& g){
            if (!g.has_node(node_id)) throw std::runtime_error("node not found");
            auto n = g.nodes.at(node_id).to_yaml();
            std::stringstream ss; ss << n; return ss.str();
        }).get();
    } catch (...) { return std::nullopt; }
}

bool Kernel::set_node_yaml(const std::string& name, int node_id, const std::string& yaml_text) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        return it->second->post([=](NodeGraph& g){
            if (!g.has_node(node_id)) return false;
            YAML::Node root = YAML::Load(yaml_text);
            ps::Node updated = ps::Node::from_yaml(root);
            // Force id to requested id to prevent mismatch
            updated.id = node_id;
            // Update in-place without redoing full cycle detection (keeps current behavior simple)
            g.nodes[node_id] = std::move(updated);
            return true;
        }).get();
    } catch (...) { return false; }
}

} // namespace ps
