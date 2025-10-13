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
            rt->post([this, yaml = final_yaml_to_load](GraphModel& g){
                io_service_.load(g, yaml);
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
                     bool disable_disk_cache, bool nosave,
                     std::vector<BenchmarkEvent>* benchmark_events) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        auto& runtime = *it->second;
        if (!runtime.running()) runtime.start();
        auto& model = runtime.model();
        runtime.post([nosave](GraphModel& g){ g.set_skip_save_cache(nosave); return 0; }).get();
        model.set_quiet(quiet);

        if (parallel) {
            ComputeService compute_service(traversal_service_, cache_service_, runtime.event_service());
            compute_service.compute_parallel(model,
                                             runtime,
                                             node_id,
                                             cache_precision,
                                             force_recache,
                                             enable_timing,
                                             disable_disk_cache,
                                             benchmark_events);
        } else {
            const int id = node_id;
            const std::string precision = cache_precision;
            const bool frc = force_recache;
            const bool timing = enable_timing;
            const bool disable_dc = disable_disk_cache;
            auto fut = runtime.post([this, &runtime, id, precision, frc, timing, disable_dc, benchmark_events](GraphModel& g){
                ComputeService compute_service(traversal_service_, cache_service_, runtime.event_service());
                compute_service.compute(g, id, precision, frc, timing, disable_dc, benchmark_events);
                return 0;
            });
            fut.get();
            runtime.wait_for_completion();
        }

        last_error_.erase(name);
        runtime.post([](GraphModel& g){ g.set_skip_save_cache(false); return 0; }).get();
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
        return it->second->post([](GraphModel& g){ return g.timing_results; }).get();
    } catch (...) { return std::nullopt; }
}

bool Kernel::reload_graph_yaml(const std::string& name, const std::string& yaml_path) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        std::filesystem::path path = yaml_path;
        it->second->post([this, path](GraphModel& g){ io_service_.load(g, path); return 0; }).get();
        return true;
    }
    catch (...) { return false; }
}

bool Kernel::save_graph_yaml(const std::string& name, const std::string& yaml_path) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        std::filesystem::path path = yaml_path;
        it->second->post([this, path](GraphModel& g){ io_service_.save(g, path); return 0; }).get();
        return true;
    }
    catch (...) { return false; }
}

bool Kernel::clear_drive_cache(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([this](GraphModel& g){ cache_service_.clear_drive_cache(g); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::clear_memory_cache(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([this](GraphModel& g){ cache_service_.clear_memory_cache(g); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::clear_cache(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([this](GraphModel& g){ cache_service_.clear_cache(g); return 0; }).get(); return true; }
    catch (...) { return false; }
}

bool Kernel::cache_all_nodes(const std::string& name, const std::string& cache_precision) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        it->second->post([this, cache_precision](GraphModel& g){ cache_service_.cache_all_nodes(g, cache_precision); return 0; }).get();
        return true;
    }
    catch (...) { return false; }
}

bool Kernel::free_transient_memory(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([this](GraphModel& g){ cache_service_.free_transient_memory(g); return 0; }).get(); return true; }
    catch (...) { return false; }
}

std::optional<GraphModel::DriveClearResult> Kernel::clear_drive_cache_stats(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this](GraphModel& g){ return cache_service_.clear_drive_cache(g); }).get();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<GraphModel::MemoryClearResult> Kernel::clear_memory_cache_stats(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this](GraphModel& g){ return cache_service_.clear_memory_cache(g); }).get();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<GraphModel::CacheSaveResult> Kernel::cache_all_nodes_stats(const std::string& name, const std::string& cache_precision) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this, cache_precision](GraphModel& g){ return cache_service_.cache_all_nodes(g, cache_precision); }).get();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<GraphModel::MemoryClearResult> Kernel::free_transient_memory_stats(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this](GraphModel& g){ return cache_service_.free_transient_memory(g); }).get();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<GraphModel::DiskSyncResult> Kernel::synchronize_disk_cache_stats(const std::string& name, const std::string& cache_precision) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this, cache_precision](GraphModel& g){ return cache_service_.synchronize_disk_cache(g, cache_precision); }).get();
    } catch (...) {
        return std::nullopt;
    }
}

bool Kernel::synchronize_disk_cache(const std::string& name, const std::string& cache_precision) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try {
        it->second->post([this, cache_precision](GraphModel& g){ cache_service_.synchronize_disk_cache(g, cache_precision); return 0; }).get();
        return true;
    }
    catch (...) { return false; }
}

bool Kernel::clear_graph(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return false;
    try { it->second->post([](GraphModel& g){ g.clear(); return 0; }).get(); return true; }
    catch (...) { return false; }
}

std::optional<std::string> Kernel::dump_dependency_tree(const std::string& name, std::optional<int> node_id, bool show_parameters) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this, node_id, show_parameters](GraphModel& g){
            std::stringstream ss;
            if (node_id) {
                traversal_service_.print_dependency_tree(g, ss, *node_id, show_parameters);
            } else {
                traversal_service_.print_dependency_tree(g, ss, show_parameters);
            }
            return ss.str();
        }).get();
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
    try { return it->second->post([this](GraphModel& g){ return traversal_service_.ending_nodes(g); }).get(); }
    catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::topo_postorder_from(const std::string& name, int end_node_id) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try { return it->second->post([this, end_node_id](GraphModel& g){ return traversal_service_.topo_postorder_from(g, end_node_id); }).get(); }
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
        return it->second->post([this](GraphModel& g){
            std::map<int, std::vector<Kernel::TraversalNodeInfo>> result;
            auto ends = traversal_service_.ending_nodes(g);
            for (int end : ends) {
                try {
                    auto order = traversal_service_.topo_postorder_from(g, end);
                    std::vector<Kernel::TraversalNodeInfo> vec; vec.reserve(order.size());
                    for (int nid : order) {
                        const auto& node = g.nodes.at(nid);
                        bool mem = node.cached_output.has_value();
                        bool on_disk = false;
                        if (!node.caches.empty()) {
                            for (const auto& cache : node.caches) {
                                std::filesystem::path cache_file = cache_service_.node_cache_dir(g, node.id) / cache.location;
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
        if (!runtime.running()) runtime.start();
        auto& model = runtime.model();

        if (parallel) {
            ComputeService compute_service(traversal_service_, cache_service_, runtime.event_service());
            output = compute_service.compute_parallel(model,
                                                      runtime,
                                                      node_id,
                                                      cache_precision,
                                                      force_recache,
                                                      enable_timing,
                                                      disable_disk_cache,
                                                      benchmark_events);
        } else {
            std::future<NodeOutput> fut = runtime.post([this, &runtime, node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events](GraphModel& g) -> NodeOutput {
                ComputeService compute_service(traversal_service_, cache_service_, runtime.event_service());
                NodeOutput& ref = compute_service.compute(g,
                                                          node_id,
                                                          cache_precision,
                                                          force_recache,
                                                          enable_timing,
                                                          disable_disk_cache,
                                                          benchmark_events);
                return ref;
            });
            output = fut.get();
            runtime.wait_for_completion();
        }
        
        if (output.image_buffer.width == 0) return std::nullopt;
        return toCvMat(output.image_buffer).clone();
    } catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::trees_containing_node(const std::string& name, int node_id) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([this, node_id](GraphModel& g){ return traversal_service_.get_trees_containing_node(g, node_id); }).get();
    } catch (...) { return std::nullopt; }
}

std::optional<std::vector<int>> Kernel::list_node_ids(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) return std::nullopt;
    try {
        return it->second->post([](GraphModel& g){
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
        return it->second->post([=](GraphModel& g){
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
        return it->second->post([=](GraphModel& g){
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
                                                   bool disable_disk_cache, bool nosave,
                                                   std::vector<BenchmarkEvent>* benchmark_events) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
        return std::nullopt;
    }

    // Capture all invocation arguments by value to avoid dangling references when executed asynchronously.
    const int id = node_id;
    const std::string precision = cache_precision;
    const bool frc = force_recache;
    const bool timing = enable_timing;
    const bool par = parallel;
    const bool q = quiet;
    const bool disable_dc = disable_disk_cache;
    const std::string name_copy = name;
    GraphRuntime* const runtime_ptr = it->second.get();

    if (par) {
        return std::optional<std::future<bool>>(std::async(std::launch::async, [this, runtime_ptr, id, precision, frc, timing, q, disable_dc, nosave, name_copy, benchmark_events]() {
            try {
                if (!runtime_ptr->running()) runtime_ptr->start();
                GraphModel& model = runtime_ptr->model();
                ComputeService compute_service(traversal_service_, cache_service_, runtime_ptr->event_service());
                if (timing) {
                    compute_service.clear_timing_results(model);
                }
                bool prev_quiet = model.is_quiet();
                model.set_quiet(q);
                model.set_skip_save_cache(nosave);
                compute_service.compute_parallel(model,
                                                  *runtime_ptr,
                                                  id,
                                                  precision,
                                                  frc,
                                                  timing,
                                                  disable_dc,
                                                  benchmark_events);
                model.set_skip_save_cache(false);
                model.set_quiet(prev_quiet);
                last_error_.erase(name_copy);
                return true;
            } catch (const GraphError& ge) {
                last_error_[name_copy] = { ge.code(), ge.what() };
                return false;
            } catch (const std::exception& e) {
                std::stringstream ss;
                ss << "std::exception: " << e.what() << " (while computing node " << id << ")";
                last_error_[name_copy] = { GraphErrc::Unknown, ss.str() };
                return false;
            }
        }));
    }

    if (!it->second->running()) it->second->start();
    GraphRuntime* runtime_capture = it->second.get();
    return it->second->post([=](GraphModel& g) {
        try {
            ComputeService compute_service(traversal_service_, cache_service_, runtime_capture->event_service());
            if (timing) {
                compute_service.clear_timing_results(g);
            }
            bool prev_quiet = g.is_quiet();
            g.set_quiet(q);
            g.set_skip_save_cache(nosave);
            compute_service.compute(g,
                                    id,
                                    precision,
                                    frc,
                                    timing,
                                    disable_dc,
                                    benchmark_events);
            g.set_skip_save_cache(false);
            g.set_quiet(prev_quiet);
            last_error_.erase(name_copy);
            return true;
        } catch (const GraphError& ge) {
            last_error_[name_copy] = { ge.code(), ge.what() };
            return false;
        } catch (const std::exception& e) {
            std::stringstream ss;
            ss << "std::exception: " << e.what() << " (while computing node " << id << ")";
            last_error_[name_copy] = { GraphErrc::Unknown, ss.str() };
            return false;
        }
    });
}

std::optional<std::vector<GraphEventService::ComputeEvent>> Kernel::drain_compute_events(const std::string& name) {
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
        return it->second->post([](GraphModel& g) {
            return g.total_io_time_ms.load();
        }).get();
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace ps
