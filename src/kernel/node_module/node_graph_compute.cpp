// NodeGraph compute (sequential)
#include "node_graph.hpp"
#include <chrono>
#include <unordered_map>
#include <variant>
namespace ps {

NodeOutput& NodeGraph::compute(int node_id, const std::string& cache_precision,
                               bool force_recache, bool enable_timing,
                               bool disable_disk_cache) {
    if (!has_node(node_id)) {
        throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
    }
    if (force_recache) {
        try {
            auto deps = topo_postorder_from(node_id);
            for (int id : deps) {
                if (nodes.count(id)) nodes.at(id).cached_output.reset();
            }
        } catch (const GraphError&) {}
    }
    std::unordered_map<int, bool> visiting;
    // When force_recache is requested, skip loading from disk cache to ensure true recomputation.
    return compute_internal(node_id, cache_precision, visiting, enable_timing,
                            /*allow_disk_cache=*/!disable_disk_cache);
}

NodeOutput& NodeGraph::compute_internal(int node_id, const std::string& cache_precision, std::unordered_map<int, bool>& visiting, bool enable_timing, bool allow_disk_cache){
    auto& node = nodes.at(node_id);
    std::string result_source = "unknown";
    auto start_time = std::chrono::high_resolution_clock::now();

    do {
        if (node.cached_output.has_value()) { result_source = "memory_cache"; break; }
        if (allow_disk_cache && try_load_from_disk_cache(node)) { result_source = "disk_cache"; break; }
        if (visiting[node_id]) { throw GraphError(GraphErrc::Cycle, "Cycle detected in graph involving node " + std::to_string(node_id)); }
        visiting[node_id] = true;

        // --- 依赖计算部分保持不变 ---
        node.runtime_parameters = node.parameters ? node.parameters : YAML::Node(YAML::NodeType::Map);
        for (const auto& p_input : node.parameter_inputs) {
            // ... (parameter input logic) ...
             if (p_input.from_node_id == -1) continue;
            if (!has_node(p_input.from_node_id)) {
                throw GraphError(GraphErrc::MissingDependency, "Node " + std::to_string(node.id) + " has missing parameter dependency: " + std::to_string(p_input.from_node_id));
            }
            const auto& upstream_output = compute_internal(p_input.from_node_id, cache_precision, visiting, enable_timing, allow_disk_cache);
            auto it = upstream_output.data.find(p_input.from_output_name);
            if (it == upstream_output.data.end()) {
                throw GraphError(GraphErrc::MissingDependency, "Node " + std::to_string(p_input.from_node_id) + " did not produce required output '" + p_input.from_output_name + "' for node " + std::to_string(node_id));
            }
            node.runtime_parameters[p_input.to_parameter_name] = it->second;
        }

        std::vector<const NodeOutput*> input_node_outputs;
        for (const auto& i_input : node.image_inputs) {
            if (i_input.from_node_id == -1) continue;
            if (!has_node(i_input.from_node_id)) {
                throw GraphError(GraphErrc::MissingDependency, "Node " + std::to_string(node.id) + " has missing image dependency: " + std::to_string(i_input.from_node_id));
            }
            const auto& upstream_output = compute_internal(i_input.from_node_id, cache_precision, visiting, enable_timing, allow_disk_cache);
            input_node_outputs.push_back(&upstream_output);
        }

        // --- 核心修改：使用 std::visit 处理 OpVariant ---
        auto op_func_opt = OpRegistry::instance().find(node.type, node.subtype);
        if (!op_func_opt) { throw GraphError(GraphErrc::NoOperation, "No op for " + node.type + ":" + node.subtype); }

        std::visit([&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                // 如果是整体计算函数，直接调用
                node.cached_output = op_func(node, input_node_outputs);
            } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                // 如果是分块计算函数，抛出错误，因为当前 compute_internal 还不支持分块调度
                // 这部分将在阶段四完成
                throw GraphError(GraphErrc::ComputeError, "Tiled operation found in non-tiled compute engine. Stage 4 required.");
            }
        }, *op_func_opt);

        result_source = "computed";
        save_cache_if_configured(node, cache_precision);
        visiting[node_id] = false;
    } while(false);

    // --- 计时和事件部分保持不变 ---
    if (enable_timing) {
        // ... (timing logic) ...
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
        {
            std::lock_guard<std::mutex> lk(timing_mutex_);
            this->timing_results.node_timings.push_back({node.id, node.name, elapsed.count(), result_source});
            this->timing_results.total_ms += elapsed.count();
        }
        push_compute_event(node.id, node.name, result_source, elapsed.count());
    } else {
        push_compute_event(node.id, node.name, result_source, 0.0);
    }
    return *node.cached_output;
}

void NodeGraph::clear_timing_results() {
    timing_results.node_timings.clear();
    timing_results.total_ms = 0.0;
}

} // namespace ps
