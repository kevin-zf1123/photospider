// FILE: src/kernel/node_module/node_graph_parallel.cpp (重构后的完整文件)

#include "node_graph.hpp"
#include "kernel/graph_runtime.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/param_utils.hpp"
#include <algorithm>
#include <vector>
#include <map>
#include <atomic>
#include <exception>
#include <random>
#include <unordered_set>
#include <unordered_map>

namespace ps {

// Forward declarations from compute module (same namespace)
void execute_tile_task(const ps::TileTask& task, const TileOpFunc& tiled_op);
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size, const cv::Size& bounds);

// New: non-recursive compute used by the parallel scheduler. Assumes dependencies are ready.
NodeOutput& NodeGraph::compute_node_no_recurse(int node_id,
                                               const std::string& cache_precision,
                                               bool enable_timing,
                                               bool allow_disk_cache,
                                               std::vector<BenchmarkEvent>* benchmark_events) {
    auto& target_node = nodes.at(node_id);
    // Fast path: already computed
    if (target_node.cached_output.has_value()) return *target_node.cached_output;

    // Optionally load from disk cache for this node itself
    if (allow_disk_cache) {
        (void)try_load_from_disk_cache(target_node);
        if (target_node.cached_output.has_value()) return *target_node.cached_output;
    }

    // Ensure visibility of upstream writes before reading their cached outputs
    std::atomic_thread_fence(std::memory_order_acquire);

    // Build runtime parameters starting from static parameters.
    // Use deep clone to avoid yaml-cpp memory_holder merges across threads.
    target_node.runtime_parameters =
        target_node.parameters ? YAML::Clone(target_node.parameters) : YAML::Node(YAML::NodeType::Map);

    // Read parameter inputs from already computed parents
    for (const auto& p_input : target_node.parameter_inputs) {
        if (p_input.from_node_id < 0) continue;
        const auto itn = nodes.find(p_input.from_node_id);
        if (itn == nodes.end() || !itn->second.cached_output.has_value()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "Parallel scheduler bug: parameter input not ready for node " + std::to_string(node_id));
        }
        const auto& up_out = *itn->second.cached_output;
        auto it = up_out.data.find(p_input.from_output_name);
        if (it == up_out.data.end()) {
            throw GraphError(GraphErrc::MissingDependency,
                "Node " + std::to_string(p_input.from_node_id) +
                " did not produce output '" + p_input.from_output_name + "'");
        }
        target_node.runtime_parameters[p_input.to_parameter_name] = it->second;
    }

    // Gather image inputs from parents (must be ready)
    std::vector<const NodeOutput*> inputs_ready;
    inputs_ready.reserve(target_node.image_inputs.size());
    for (const auto& i_input : target_node.image_inputs) {
        if (i_input.from_node_id < 0) continue;
        const auto itn = nodes.find(i_input.from_node_id);
        if (itn == nodes.end() || !itn->second.cached_output.has_value()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "Parallel scheduler bug: image input not ready for node " + std::to_string(node_id));
        }
        inputs_ready.push_back(&*itn->second.cached_output);
    }

    auto op_opt = OpRegistry::instance().find(target_node.type, target_node.subtype);
    if (!op_opt) {
        throw GraphError(GraphErrc::NoOperation,
                         "No op for " + target_node.type + ":" + target_node.subtype);
    }

    // Timing event for benchmarking
    BenchmarkEvent current_event;
    current_event.node_id = node_id;
    current_event.op_name = make_key(target_node.type, target_node.subtype);
    current_event.dependency_start_time = std::chrono::high_resolution_clock::now();
    current_event.execution_start_time = current_event.dependency_start_time;

    try {
        std::visit([&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                target_node.cached_output = op_func(target_node, inputs_ready);
            } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                // Prepare normalized inputs similar to compute_internal
                std::vector<NodeOutput> normalized_storage;
                std::vector<const NodeOutput*> inputs_for_tiling = inputs_ready;

                bool is_mixing = (target_node.type == "image_mixing");
                if (is_mixing && inputs_ready.size() >= 2) {
                    const auto& base_buffer = inputs_ready[0]->image_buffer;
                    if (base_buffer.width == 0 || base_buffer.height == 0) {
                        throw GraphError(GraphErrc::InvalidParameter, "Base image for image_mixing is empty.");
                    }
                    const int base_w = base_buffer.width;
                    const int base_h = base_buffer.height;
                    const int base_c = base_buffer.channels;
                    const std::string strategy = as_str(target_node.runtime_parameters, "merge_strategy", "resize");
                    normalized_storage.reserve(inputs_ready.size() - 1);
                    for (size_t i = 1; i < inputs_ready.size(); ++i) {
                        const auto& current_buffer = inputs_ready[i]->image_buffer;
                        if (current_buffer.width == 0 || current_buffer.height == 0) {
                            throw GraphError(GraphErrc::InvalidParameter, "Secondary image for image_mixing is empty.");
                        }
                        cv::Mat current_mat = toCvMat(current_buffer);
                        if (current_mat.cols != base_w || current_mat.rows != base_h) {
                            if (strategy == "resize") {
                                cv::resize(current_mat, current_mat, cv::Size(base_w, base_h), 0, 0, cv::INTER_LINEAR);
                            } else if (strategy == "crop") {
                                cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w), std::min(current_mat.rows, base_h));
                                cv::Mat cropped = cv::Mat::zeros(base_h, base_w, current_mat.type());
                                current_mat(crop_roi).copyTo(cropped(crop_roi));
                                current_mat = cropped;
                            } else {
                                throw GraphError(GraphErrc::InvalidParameter, "Unsupported merge_strategy for tiled mixing.");
                            }
                        }
                        if (current_mat.channels() != base_c) {
                            if (current_mat.channels() == 1 && (base_c == 3 || base_c == 4)) {
                                std::vector<cv::Mat> planes(base_c, current_mat);
                                cv::merge(planes, current_mat);
                            } else if ((current_mat.channels() == 3 || current_mat.channels() == 4) && base_c == 1) {
                                cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
                            } else if (current_mat.channels() == 4 && base_c == 3) {
                                cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
                            } else if (current_mat.channels() == 3 && base_c == 4) {
                                cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
                            } else {
                                throw GraphError(GraphErrc::InvalidParameter, "Unsupported channel conversion in tiled mixing.");
                            }
                        }
                        NodeOutput tmp; tmp.image_buffer = fromCvMat(current_mat);
                        normalized_storage.push_back(std::move(tmp));
                        inputs_for_tiling[i] = &normalized_storage.back();
                    }
                }

                // Infer output shape
                int out_w = inputs_for_tiling.empty() ? as_int_flexible(target_node.runtime_parameters, "width", 256)
                                                      : inputs_for_tiling[0]->image_buffer.width;
                int out_h = inputs_for_tiling.empty() ? as_int_flexible(target_node.runtime_parameters, "height", 256)
                                                      : inputs_for_tiling[0]->image_buffer.height;
                int out_c = inputs_for_tiling.empty() ? 1 : inputs_for_tiling[0]->image_buffer.channels;
                auto out_t = inputs_for_tiling.empty() ? ps::DataType::FLOAT32 : inputs_for_tiling[0]->image_buffer.type;

                target_node.cached_output = NodeOutput();
                auto& ob = target_node.cached_output->image_buffer;
                ob.width = out_w; ob.height = out_h; ob.channels = out_c; ob.type = out_t;
                size_t pix_sz = sizeof(float);
                ob.step = out_w * out_c * pix_sz;
                ob.data.reset(new char[ob.step * ob.height], std::default_delete<char[]>());

                const int TILE_SIZE = 256, HALO_SIZE = 16;
                for (int y = 0; y < ob.height; y += TILE_SIZE) {
                    for (int x = 0; x < ob.width; x += TILE_SIZE) {
                        ps::TileTask task;
                        task.node = &target_node;
                        task.output_tile.buffer = &ob;
                        task.output_tile.roi = cv::Rect(x, y, std::min(TILE_SIZE, ob.width - x), std::min(TILE_SIZE, ob.height - y));
                        const bool needs_halo = (target_node.type == "image_process" && target_node.subtype == "gaussian_blur");
                        for (auto const* in_out : inputs_for_tiling) {
                            ps::Tile in_tile;
                            in_tile.buffer = const_cast<ps::ImageBuffer*>(&in_out->image_buffer);
                            if (needs_halo) {
                                in_tile.roi = calculate_halo(task.output_tile.roi, HALO_SIZE, {in_out->image_buffer.width, in_out->image_buffer.height});
                            } else {
                                in_tile.roi = task.output_tile.roi;
                            }
                            task.input_tiles.push_back(in_tile);
                        }
                        execute_tile_task(task, op_func);
                    }
                }
            }
        }, *op_opt);
    } catch (const cv::Exception& e) {
        throw GraphError(GraphErrc::ComputeError,
                         "Node " + std::to_string(target_node.id) + " (" + target_node.name + ") failed: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw GraphError(GraphErrc::ComputeError,
                         "Node " + std::to_string(target_node.id) + " (" + target_node.name + ") failed: " + std::string(e.what()));
    }

    // Save disk cache if configured
    save_cache_if_configured(target_node, cache_precision);

    // Timing & events
    if (enable_timing) {
        current_event.execution_end_time = std::chrono::high_resolution_clock::now();
        current_event.source = "computed";
        current_event.execution_duration_ms = std::chrono::duration<double, std::milli>(current_event.execution_end_time - current_event.execution_start_time).count();
        if (benchmark_events) benchmark_events->push_back(current_event);
        {
            std::lock_guard lk(timing_mutex_);
            timing_results.node_timings.push_back({ target_node.id, target_node.name, current_event.execution_duration_ms, "computed" });
        }
        push_compute_event(target_node.id, target_node.name, "computed", current_event.execution_duration_ms);
    } else {
        push_compute_event(target_node.id, target_node.name, "computed", 0.0);
    }
    return *target_node.cached_output;
}

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
                    nodes.at(id).cached_output.reset();
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
        // Scheme B: temporary results storage per node index
        std::vector<std::optional<NodeOutput>> temp_results(num_nodes);
        // Pre-resolve ops on main thread to avoid concurrent registry access
        std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops(num_nodes);
        std::vector<std::optional<OpMetadata>> resolved_meta(num_nodes);
        for (size_t i = 0; i < num_nodes; ++i) {
            const auto& n = nodes.at(execution_order[i]);
            resolved_ops[i] = OpRegistry::instance().find(n.type, n.subtype);
            resolved_meta[i] = OpRegistry::instance().get_metadata(n.type, n.subtype);
        }
        
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
                               &id_to_idx, &temp_results, &resolved_ops, &resolved_meta, current_node_id, current_node_idx,
                               cache_precision, enable_timing, disable_disk_cache, benchmark_events, force_recache] () {
                // 1) Pure compute into temp_results without mutating NodeGraph
                try {
                    const Node& target_node = nodes.at(current_node_id);
                    bool allow_disk_cache = (!disable_disk_cache) && (!force_recache);

                    // Ensure upstream writes visible
                    std::atomic_thread_fence(std::memory_order_acquire);

                    auto get_upstream_output = [&](int up_id) -> const NodeOutput* {
                        if (up_id < 0) return nullptr;
                        auto itn = nodes.find(up_id);
                        if (itn == nodes.end()) return nullptr;
                        auto it_idx = id_to_idx.find(up_id);
                        if (it_idx != id_to_idx.end()) {
                            int up_idx = it_idx->second;
                            if (temp_results[up_idx].has_value()) return &*temp_results[up_idx];
                        }
                        if (itn->second.cached_output.has_value()) return &*itn->second.cached_output;
                        return nullptr;
                    };

                    // Memory or disk fast path
                    if (!target_node.cached_output.has_value() && allow_disk_cache && !temp_results[current_node_idx].has_value()) {
                        NodeOutput from_disk;
                        if (try_load_from_disk_cache_into(target_node, from_disk)) {
                            temp_results[current_node_idx] = std::move(from_disk);
                            if (enable_timing) {
                                // Log a zero-cost event indicating disk cache hit (IO time tracked separately)
                                BenchmarkEvent ev;
                                ev.node_id = current_node_id;
                                ev.op_name = make_key(target_node.type, target_node.subtype);
                                ev.dependency_start_time = std::chrono::high_resolution_clock::now();
                                ev.execution_start_time = ev.dependency_start_time;
                                ev.execution_end_time = ev.execution_start_time;
                                ev.execution_duration_ms = 0.0;
                                ev.source = "disk_cache";
                                if (benchmark_events) {
                                    std::lock_guard lk(timing_mutex_);
                                    benchmark_events->push_back(ev);
                                }
                                {
                                    std::lock_guard lk(timing_mutex_);
                                    timing_results.node_timings.push_back({ target_node.id, target_node.name, 0.0, "disk_cache" });
                                }
                                push_compute_event(target_node.id, target_node.name, "disk_cache", 0.0);
                            }
                        }
                    }

                    if (!target_node.cached_output.has_value() && !temp_results[current_node_idx].has_value()) {
                        YAML::Node runtime_params = target_node.parameters ? YAML::Clone(target_node.parameters)
                                                                           : YAML::Node(YAML::NodeType::Map);
                        for (const auto& p_input : target_node.parameter_inputs) {
                            if (p_input.from_node_id < 0) continue;
                            auto const* up_out = get_upstream_output(p_input.from_node_id);
                            if (!up_out) {
                                throw GraphError(GraphErrc::MissingDependency, "Parameter input not ready for node " + std::to_string(current_node_id));
                            }
                            auto it = up_out->data.find(p_input.from_output_name);
                            if (it == up_out->data.end()) {
                                throw GraphError(GraphErrc::MissingDependency,
                                    "Node " + std::to_string(p_input.from_node_id) + " missing output '" + p_input.from_output_name + "'");
                            }
                            runtime_params[p_input.to_parameter_name] = it->second;
                        }

                        std::vector<const NodeOutput*> inputs_ready;
                        inputs_ready.reserve(target_node.image_inputs.size());
                        for (const auto& i_input : target_node.image_inputs) {
                            if (i_input.from_node_id < 0) continue;
                            auto const* up_out = get_upstream_output(i_input.from_node_id);
                            if (!up_out) {
                                throw GraphError(GraphErrc::MissingDependency, "Image input not ready for node " + std::to_string(current_node_id));
                            }
                            inputs_ready.push_back(up_out);
                        }

                        const auto& op_opt = resolved_ops[current_node_idx];
                        if (!op_opt.has_value()) {
                            throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type + ":" + target_node.subtype);
                        }

                        BenchmarkEvent current_event;
                        current_event.node_id = current_node_id;
                        current_event.op_name = make_key(target_node.type, target_node.subtype);
                        current_event.dependency_start_time = std::chrono::high_resolution_clock::now();
                        current_event.execution_start_time = current_event.dependency_start_time;

                        // Build execution node with resolved runtime parameters
                        Node node_for_exec = target_node;
                        node_for_exec.runtime_parameters = runtime_params;

                        NodeOutput result;
                        bool tiled_dispatched = false;
                        try {
                            std::visit([&](auto&& op_func) {
                                using T = std::decay_t<decltype(op_func)>;
                                if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                                    result = op_func(node_for_exec, inputs_ready);
                                } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                                    // Build normalized inputs that must outlive tile micro-tasks
                                    std::vector<NodeOutput> normalized_storage_local;
                                    std::vector<const NodeOutput*> inputs_for_tiling = inputs_ready;
                                    bool is_mixing = (target_node.type == "image_mixing");
                                    if (is_mixing && inputs_ready.size() >= 2) {
                                        const auto& base_buffer = inputs_ready[0]->image_buffer;
                                        if (base_buffer.width == 0 || base_buffer.height == 0) {
                                            throw GraphError(GraphErrc::InvalidParameter, "Base image for image_mixing is empty.");
                                        }
                                        const int base_w = base_buffer.width;
                                        const int base_h = base_buffer.height;
                                        const int base_c = base_buffer.channels;
                                        const std::string strategy = as_str(node_for_exec.runtime_parameters, "merge_strategy", "resize");
                                        normalized_storage_local.reserve(inputs_ready.size() - 1);
                                        for (size_t i = 1; i < inputs_ready.size(); ++i) {
                                            const auto& current_buffer = inputs_ready[i]->image_buffer;
                                            if (current_buffer.width == 0 || current_buffer.height == 0) {
                                                throw GraphError(GraphErrc::InvalidParameter, "Secondary image for image_mixing is empty.");
                                            }
                                            cv::Mat current_mat = toCvMat(current_buffer);
                                            if (current_mat.cols != base_w || current_mat.rows != base_h) {
                                                if (strategy == "resize") {
                                                    cv::resize(current_mat, current_mat, cv::Size(base_w, base_h), 0, 0, cv::INTER_LINEAR);
                                                } else if (strategy == "crop") {
                                                    cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w), std::min(current_mat.rows, base_h));
                                                    cv::Mat cropped = cv::Mat::zeros(base_h, base_w, current_mat.type());
                                                    current_mat(crop_roi).copyTo(cropped(crop_roi));
                                                    current_mat = cropped;
                                                } else {
                                                    throw GraphError(GraphErrc::InvalidParameter, "Unsupported merge_strategy for tiled mixing.");
                                                }
                                            }
                                            if (current_mat.channels() != base_c) {
                                                if (current_mat.channels() == 1 && (base_c == 3 || base_c == 4)) {
                                                    std::vector<cv::Mat> planes(base_c, current_mat);
                                                    cv::merge(planes, current_mat);
                                                } else if ((current_mat.channels() == 3 || current_mat.channels() == 4) && base_c == 1) {
                                                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
                                                } else if (current_mat.channels() == 4 && base_c == 3) {
                                                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
                                                } else if (current_mat.channels() == 3 && base_c == 4) {
                                                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
                                                } else {
                                                    throw GraphError(GraphErrc::InvalidParameter, "Unsupported channel conversion in tiled mixing.");
                                                }
                                            }
                                            NodeOutput tmp; tmp.image_buffer = fromCvMat(current_mat);
                                            normalized_storage_local.push_back(std::move(tmp));
                                            inputs_for_tiling[i] = &normalized_storage_local.back();
                                        }
                                    }

                                    // Prepare shared store for normalized inputs to extend lifetime
                                    auto norm_store_sp = std::make_shared<std::vector<NodeOutput>>(std::move(normalized_storage_local));
                                    // Build input_ptrs that reference shared store for secondary images
                                    std::vector<const NodeOutput*> input_ptrs = inputs_ready;
                                    if (is_mixing && inputs_ready.size() >= 2) {
                                        for (size_t i = 1, k = 0; i < inputs_ready.size(); ++i, ++k) {
                                            if (k < norm_store_sp->size()) input_ptrs[i] = &(*norm_store_sp)[k];
                                        }
                                    }

                                    // Infer output shape
                                    int out_w = input_ptrs.empty() ? as_int_flexible(node_for_exec.runtime_parameters, "width", 256)
                                                                   : input_ptrs[0]->image_buffer.width;
                                    int out_h = input_ptrs.empty() ? as_int_flexible(node_for_exec.runtime_parameters, "height", 256)
                                                                   : input_ptrs[0]->image_buffer.height;
                                    int out_c = input_ptrs.empty() ? 1 : input_ptrs[0]->image_buffer.channels;
                                    auto out_t = input_ptrs.empty() ? ps::DataType::FLOAT32 : input_ptrs[0]->image_buffer.type;

                                    // Allocate output buffer in temp_results (visible for tiles)
                                    temp_results[current_node_idx] = NodeOutput{};
                                    auto& ob = temp_results[current_node_idx]->image_buffer;
                                    ob.width = out_w; ob.height = out_h; ob.channels = out_c; ob.type = out_t;
                                    ob.step = static_cast<size_t>(out_w) * out_c * sizeof(float);
                                    ob.data.reset(new char[ob.step * ob.height], std::default_delete<char[]>());

                                    // Tile size from metadata preference
                                    int tile_size = 128;
                                    if (resolved_meta[current_node_idx].has_value()) {
                                        auto pref = resolved_meta[current_node_idx]->tile_preference;
                                        if (pref == TileSizePreference::MICRO) tile_size = 16;
                                        else if (pref == TileSizePreference::MACRO) tile_size = 256;
                                    }
                                    const bool needs_halo = (node_for_exec.type == "image_process" && node_for_exec.subtype.find("gaussian_blur") != std::string::npos);
                                    const int HALO_SIZE = 16;

                                    // Plan tiles and spawn micro tasks
                                    int tiles_x = (out_w + tile_size - 1) / tile_size;
                                    int tiles_y = (out_h + tile_size - 1) / tile_size;
                                    int total_tiles = tiles_x * tiles_y;
                                    runtime.inc_graph_tasks_to_complete(total_tiles);
                                    auto remaining = std::make_shared<std::atomic<int>>(total_tiles);
                                    auto start_tp = std::make_shared<std::chrono::high_resolution_clock::time_point>(std::chrono::high_resolution_clock::now());

                                    for (int ty = 0; ty < tiles_y; ++ty) {
                                        for (int tx = 0; tx < tiles_x; ++tx) {
                                            int x = tx * tile_size;
                                            int y = ty * tile_size;
                                            int w = std::min(tile_size, out_w - x);
                                            int h = std::min(tile_size, out_h - y);
                                            Task tile_task = [this, &runtime, &dependents_map, &dependency_counters, &all_tasks, &temp_results,
                                                              x, y, w, h, needs_halo, HALO_SIZE, input_ptrs, norm_store_sp, remaining, start_tp,
                                                              current_node_idx, current_node_id, node_for_exec, op_func, benchmark_events, enable_timing]() {
                                                try {
                                                    TileTask tt;
                                                    tt.node = &node_for_exec;
                                                    tt.output_tile.buffer = &temp_results[current_node_idx]->image_buffer;
                                                    tt.output_tile.roi = cv::Rect(x, y, w, h);
                                                    for (auto const* in_out : input_ptrs) {
                                                        Tile in_tile;
                                                        in_tile.buffer = const_cast<ImageBuffer*>(&in_out->image_buffer);
                                                        in_tile.roi = needs_halo ? calculate_halo(tt.output_tile.roi, HALO_SIZE, {in_out->image_buffer.width, in_out->image_buffer.height})
                                                                                 : tt.output_tile.roi;
                                                        tt.input_tiles.push_back(std::move(in_tile));
                                                    }
                                                    execute_tile_task(tt, op_func);
                                                    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                                        std::atomic_thread_fence(std::memory_order_release);
                                                        if (enable_timing) {
                                                            auto end_tp = std::chrono::high_resolution_clock::now();
                                                            double exec_ms = std::chrono::duration<double, std::milli>(end_tp - *start_tp).count();
                                                            BenchmarkEvent ev; ev.node_id = current_node_id; ev.op_name = make_key(node_for_exec.type, node_for_exec.subtype);
                                                            ev.execution_start_time = *start_tp; ev.dependency_start_time = *start_tp; ev.execution_end_time = end_tp; ev.execution_duration_ms = exec_ms; ev.source = "computed";
                                                            if (benchmark_events) {
                                                                std::lock_guard lk(timing_mutex_);
                                                                benchmark_events->push_back(ev);
                                                            }
                                                            {
                                                                std::lock_guard lk(timing_mutex_);
                                                                timing_results.node_timings.push_back({ current_node_id, nodes.at(current_node_id).name, exec_ms, std::string("computed") });
                                                            }
                                                            push_compute_event(current_node_id, nodes.at(current_node_id).name, "computed", exec_ms);
                                                        } else {
                                                            push_compute_event(current_node_id, nodes.at(current_node_id).name, "computed", 0.0);
                                                        }
                                                        for (int dependent_idx : dependents_map[current_node_idx]) {
                                                            if (--dependency_counters[dependent_idx] == 0) {
                                                                runtime.submit_ready_task_from_worker(std::move(all_tasks[dependent_idx]));
                                                            }
                                                        }
                                                    }
                                                } catch (const std::exception& e) {
                                                    runtime.set_exception(std::make_exception_ptr(
                                                        GraphError(GraphErrc::ComputeError, std::string("Tile stage at node ") + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                                                    ));
                                                } catch (...) {
                                                    runtime.set_exception(std::make_exception_ptr(
                                                        GraphError(GraphErrc::ComputeError, std::string("Tile stage at node ") + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
                                                    ));
                                                }
                                                runtime.dec_graph_tasks_to_complete();
                                            };
                                            runtime.submit_ready_task_from_worker(std::move(tile_task));
                                        }
                                    }
                                    tiled_dispatched = true;
                                }
                            }, *op_opt);
                        } catch (const std::exception& e) {
                            throw;
                        }

                        if (tiled_dispatched) {
                            // Tiled micro-tasks handle timing and dependent scheduling; skip monolithic finalization.
                            return;
                        }
                        temp_results[current_node_idx] = std::move(result);

                        if (enable_timing) {
                            current_event.execution_end_time = std::chrono::high_resolution_clock::now();
                            double ms = std::chrono::duration<double, std::milli>(current_event.execution_end_time - current_event.execution_start_time).count();
                            current_event.source = "computed";
                            current_event.execution_duration_ms = ms;
                            if (benchmark_events) {
                                std::lock_guard lk(timing_mutex_);
                                benchmark_events->push_back(current_event);
                            }
                            {
                                std::lock_guard lk(timing_mutex_);
                                timing_results.node_timings.push_back({ target_node.id, target_node.name, ms, "computed" });
                            }
                            push_compute_event(target_node.id, target_node.name, "computed", ms);
                        } else {
                            push_compute_event(target_node.id, target_node.name, "computed", 0.0);
                        }
                    }
                } catch (const cv::Exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Compute stage at node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + std::string(e.what()))
                    ));
                    return;
                } catch (const std::exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Compute stage at node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                    ));
                    return;
                } catch (...) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Compute stage at node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
                    ));
                    return;
                }

                // 2) 触发后续依赖任务（捕获并精确标注阶段）
                try {
                    // Ensure all writes to temp_results are visible before scheduling dependents
                    std::atomic_thread_fence(std::memory_order_release);
                    for (int dependent_idx : dependents_map[current_node_idx]) {
                        if (--dependency_counters[dependent_idx] == 0) {
                            runtime.submit_ready_task_from_worker(std::move(all_tasks[dependent_idx]));
                        }
                    }
                } catch (const std::out_of_range& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Scheduling stage after node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: out_of_range: " + std::string(e.what()))
                    ));
                } catch (const std::exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Scheduling stage after node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                    ));
                } catch (...) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Scheduling stage after node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
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

        // Commit results: write back to nodes and save caches
        {
            std::scoped_lock lock(graph_mutex_);
            for (size_t i = 0; i < num_nodes; ++i) {
                if (temp_results[i].has_value()) {
                    int nid = execution_order[i];
                    nodes.at(nid).cached_output = std::move(*temp_results[i]);
                    save_cache_if_configured(nodes.at(nid), cache_precision);
                }
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
