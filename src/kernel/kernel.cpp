// Photospider kernel: Kernel implementation
#include "kernel/kernel.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <future>

namespace ps {

id Kernel::get_metal_device(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
        return nullptr;
    }
    return it->second->get_metal_device();
}


std::optional<std::string> Kernel::load_graph(const std::string& name,
                                              const std::string& root_dir,
                                              const std::string& yaml_path,
                                              const std::string& config_path) {
    if (graphs_.count(name)) return std::nullopt;

    // [功能修复] 当 yaml_path 为空时，推断默认的 session 文件路径
    std::string effective_yaml_path = yaml_path;
    if (effective_yaml_path.empty()) {
        effective_yaml_path = (std::filesystem::path(root_dir) / name / "content.yaml").string();
    }


    GraphRuntime::Info info{ name, std::filesystem::path(root_dir) / name,
                             effective_yaml_path, config_path };
    auto rt = std::make_unique<GraphRuntime>(info);
    try {
        std::filesystem::create_directories(info.root);
        auto yaml_target = info.root / "content.yaml";
        
        // [功能修复] 确保即使 yaml_path 最初为空，我们仍使用 effective_yaml_path
        if (!info.yaml.empty() && std::filesystem::exists(info.yaml)) {
            // 如果提供了 yaml_path（非空），则总是拷贝
            if (!yaml_path.empty()) {
                 std::filesystem::copy_file(info.yaml, yaml_target, std::filesystem::copy_options::overwrite_existing);
            }
        }

        if (!config_path.empty() && std::filesystem::exists(config_path)) {
            auto cfg_target = info.root / "config.yaml";
            std::filesystem::copy_file(config_path, cfg_target, std::filesystem::copy_options::overwrite_existing);
        }
    } catch (...) {}
    rt->start();
    
    // [功能修复] 使用推断出的 yaml_target 进行加载
    auto final_yaml_to_load = info.root / "content.yaml";

    if (std::filesystem::exists(final_yaml_to_load)) {
        try {
            rt->post([yaml = final_yaml_to_load](NodeGraph& g){
                g.load_yaml(yaml);
                return 0;
            }).get();
        } catch (const std::exception& e) {
            std::cerr << "Failed to load YAML for graph '" << name << "': " << e.what() << std::endl;
            return std::nullopt;
        }
    } else if (!yaml_path.empty()) {
        // 如果提供了原始yaml_path但目标不存在，说明拷贝失败或源文件不存在
        std::cerr << "Warning: source YAML file not found for graph '" << name << "': " << yaml_path << std::endl;
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
                     bool force_recache, bool enable_timing, bool parallel, bool quiet,
                     bool disable_disk_cache,
                     std::vector<BenchmarkEvent>* benchmark_events) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        auto& runtime = *it->second;
        auto& graph = runtime.get_nodegraph();
        graph.set_quiet(quiet);
        
        if (parallel) {
            graph.compute_parallel(runtime, node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
        } else {
            auto fut = runtime.post([&](NodeGraph& g){
                g.compute(node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
                return 0;
            });
            fut.get();
        }

        last_error_.erase(name);
        return true;
    } catch (const GraphError& ge) {
        last_error_[name] = { ge.code(), ge.what() };
        return false;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "std::exception during compute: " << e.what()
        << " (while computing node " << node_id << ")";
        last_error_[name] = { GraphErrc::Unknown, ss.str() };
        return false;
    } catch (...) {
        last_error_[name] = { GraphErrc::Unknown, std::string("unknown error") };
        return false;
    }
}

std::optional<TimingCollector> Kernel::get_timing(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([](NodeGraph& g){ return g.timing_results; }).get();
    } catch (...) { return std::nullopt; }
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

std::optional<Kernel::LastError> Kernel::last_error(const std::string& name) const {
    auto it = last_error_.find(name);
    if (it == last_error_.end()) return std::nullopt;
    return it->second;
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

std::optional<std::map<int, std::vector<Kernel::TraversalNodeInfo>>> Kernel::traversal_details(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([=](NodeGraph& g){
            std::map<int, std::vector<Kernel::TraversalNodeInfo>> result;
            auto ends = g.ending_nodes();
            for (int end : ends) {
                try {
                    auto order = g.topo_postorder_from(end);
                    std::vector<Kernel::TraversalNodeInfo> vec; vec.reserve(order.size());
                    for (int nid : order) {
                        const auto& node = g.nodes.at(nid);
                        bool mem = node.cached_output.has_value();
                        bool on_disk = false;
                        if (!node.caches.empty()) {
                            for (const auto& cache : node.caches) {
                                std::filesystem::path cache_file = g.node_cache_dir(node.id) / cache.location;
                                std::filesystem::path meta_file = cache_file; meta_file.replace_extension(".yml");
                                if (std::filesystem::exists(cache_file) || std::filesystem::exists(meta_file)) { on_disk = true; break; }
                            }
                        }
                        vec.push_back(Kernel::TraversalNodeInfo{ node.id, node.name, mem, on_disk });
                    }
                    result[end] = std::move(vec);
                } catch (...) {}
            }
            return result;
        }).get();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<cv::Mat> Kernel::compute_and_get_image(const std::string& name, int node_id, const std::string& cache_precision,
                                                     bool force_recache, bool enable_timing, bool parallel,
                                                     bool disable_disk_cache,
                                                     std::vector<BenchmarkEvent>* benchmark_events) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        NodeOutput output;
        auto& runtime = *it->second;
        auto& graph = runtime.get_nodegraph();

        if (parallel) {
            output = graph.compute_parallel(runtime, node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
        } else {
             std::future<NodeOutput> fut = runtime.post([&](NodeGraph& g) -> NodeOutput {
                return g.compute(node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
            });
            output = fut.get();
        }
        
        if (output.image_buffer.width == 0) return std::nullopt;
        return toCvMat(output.image_buffer).clone();
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
            updated.id = node_id;
            g.nodes[node_id] = std::move(updated);
            return true;
        }).get();
    } catch (...) { return false; }
}

std::optional<std::future<bool>> Kernel::compute_async(const std::string& name, int node_id, const std::string& cache_precision,
                                                      bool force_recache, bool enable_timing, bool parallel, bool quiet,
                                                      bool disable_disk_cache,
                                                      std::vector<BenchmarkEvent>* benchmark_events) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
        return std::nullopt;
    }

    return it->second->post([&, runtime_ptr = it->second.get()](NodeGraph& g) {
        try {
            g.clear_timing_results();
            bool prev_quiet = g.is_quiet();
            g.set_quiet(quiet);
            
            if (parallel) {
                g.compute_parallel(*runtime_ptr, node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
            } else {
                g.compute(node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
            }
            
            g.set_quiet(prev_quiet);
            last_error_.erase(name);
            return true;
        } catch (const GraphError& ge) {
            last_error_[name] = { ge.code(), ge.what() };
            return false;
        } catch (const std::exception& e) {
            last_error_[name] = { GraphErrc::Unknown, e.what() };
            return false;
        }
    });
}

std::optional<std::vector<NodeGraph::ComputeEvent>> Kernel::drain_compute_events(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->drain_compute_events_now();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> Kernel::get_last_io_time(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
        return std::nullopt;
    }
    try {
        return it->second->post([](NodeGraph& g) {
            return g.total_io_time_ms.load();
        }).get();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace ps