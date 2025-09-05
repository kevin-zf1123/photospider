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
                int out_w = inputs_for_tiling.empty() ? as_int_flexible(target_node.runtime_parameters, "width", 256) : inputs_for_tiling[0]->image_buffer.width;
                int out_h = inputs_for_tiling.empty() ? as_int_flexible(target_node.runtime_parameters, "height", 256) : inputs_for_tiling[0]->image_buffer.height;
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
        for (size_t i = 0; i < num_nodes; ++i) {
            const auto& n = nodes.at(execution_order[i]);
            resolved_ops[i] = OpRegistry::instance().find(n.type, n.subtype);
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
                               &id_to_idx, &temp_results, &resolved_ops, current_node_id, current_node_idx,
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

                        NodeOutput result;
                        try {
                            std::visit([&](auto&& op_func) {
                                using T = std::decay_t<decltype(op_func)>;
                                if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                                    result = op_func(target_node, inputs_ready);
                                } else if constexpr (std::is_same_v<T, TileOpFunc>) {
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
                                        const std::string strategy = as_str(runtime_params, "merge_strategy", "resize");
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

                                    int out_w = inputs_for_tiling.empty() ? as_int_flexible(runtime_params, "width", 256) : inputs_for_tiling[0]->image_buffer.width;
                                    int out_h = inputs_for_tiling.empty() ? as_int_flexible(runtime_params, "height", 256) : inputs_for_tiling[0]->image_buffer.height;
                                    int out_c = inputs_for_tiling.empty() ? 1 : inputs_for_tiling[0]->image_buffer.channels;
                                    auto out_t = inputs_for_tiling.empty() ? ps::DataType::FLOAT32 : inputs_for_tiling[0]->image_buffer.type;

                                    result = NodeOutput();
                                    auto& ob = result.image_buffer;
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
                                                in_tile.roi = needs_halo
                                                    ? calculate_halo(task.output_tile.roi, HALO_SIZE, {in_out->image_buffer.width, in_out->image_buffer.height})
                                                    : task.output_tile.roi;
                                                task.input_tiles.push_back(in_tile);
                                            }
                                            execute_tile_task(task, op_func);
                                        }
                                    }
                                }
                            }, *op_opt);
                        } catch (const std::exception& e) {
                            throw;
                        }

                        temp_results[current_node_idx] = std::move(result);

                        if (enable_timing) {
                            current_event.execution_end_time = std::chrono::high_resolution_clock::now();
                            double ms = std::chrono::duration<double, std::milli>(current_event.execution_end_time - current_event.execution_start_time).count();
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
                            runtime.submit_ready_task_any_thread(std::move(all_tasks[dependent_idx]));
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
