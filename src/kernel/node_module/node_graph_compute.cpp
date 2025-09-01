// in: src/kernel/node_module/node_graph_compute.cpp (REPLACE WITH THIS CORRECTED VERSION)
#include "node_graph.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器
#include <chrono>
#include <unordered_map>
#include <variant>

namespace ps {

// --- 阶段四新增：辅助函数 ---

/**
 * @brief 执行一个分块计算任务。
 *
 * @param task 包含节点、输入/输出分块的任务描述。
 * @param tiled_op 要执行的分块操作函数。
 */
void execute_tile_task(const ps::TileTask& task, const TileOpFunc& tiled_op) {
    // 直接调用新的分块操作函数
    tiled_op(*task.node, task.output_tile, task.input_tiles);
}

/**
 * @brief 为一个输出分块计算其依赖的输入分块的区域（包含光环）。
 *
 * @param roi 输出分块的感兴趣区域 (Region of Interest)。
 * @param halo_size 光环大小（在各方向上需要额外扩展的像素数）。
 * @param bounds 输入图像的完整尺寸，用于防止区域越界。
 * @return cv::Rect 计算出的、包含光环的输入区域。
 */
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size, const cv::Size& bounds) {
    if (halo_size <= 0) {
        return roi;
    }
    int x = std::max(0, roi.x - halo_size);
    int y = std::max(0, roi.y - halo_size);
    int right = std::min(bounds.width, roi.x + roi.width + halo_size);
    int bottom = std::min(bounds.height, roi.y + roi.height + halo_size);
    return cv::Rect(x, y, right - x, bottom - y);
}

// --- 阶段四核心：重构后的 compute_internal 函数 ---

NodeOutput& NodeGraph::compute_internal(int node_id, const std::string& cache_precision, std::unordered_map<int, bool>& visiting, bool enable_timing, bool allow_disk_cache) {
    auto& target_node = nodes.at(node_id);
    std::string result_source = "unknown";
    auto start_time = std::chrono::high_resolution_clock::now();

    do {
        // 1. 缓存检查 (逻辑不变)
        if (target_node.cached_output.has_value()) {
            result_source = "memory_cache";
            break;
        }
        if (allow_disk_cache && try_load_from_disk_cache(target_node)) {
            result_source = "disk_cache";
            break;
        }

        // 2. 依赖计算与参数解析 (逻辑不变)
        if (visiting[node_id]) {
            throw GraphError(GraphErrc::Cycle, "Cycle detected involving node " + std::to_string(node_id));
        }
        visiting[node_id] = true;

        target_node.runtime_parameters = target_node.parameters ? target_node.parameters : YAML::Node(YAML::NodeType::Map);
        for (const auto& p_input : target_node.parameter_inputs) {
            if (p_input.from_node_id == -1) continue;
            const auto& upstream_output = compute_internal(p_input.from_node_id, cache_precision, visiting, enable_timing, allow_disk_cache);
            auto it = upstream_output.data.find(p_input.from_output_name);
            if (it == upstream_output.data.end()) throw GraphError(GraphErrc::MissingDependency, "Node " + std::to_string(p_input.from_node_id) + " did not produce output '" + p_input.from_output_name + "'");
            target_node.runtime_parameters[p_input.to_parameter_name] = it->second;
        }

        std::vector<const NodeOutput*> monolithic_inputs;
        for (const auto& i_input : target_node.image_inputs) {
            if (i_input.from_node_id != -1) {
                monolithic_inputs.push_back(&compute_internal(i_input.from_node_id, cache_precision, visiting, enable_timing, allow_disk_cache));
            }
        }

        // 3. 获取操作函数
        auto op_variant_opt = OpRegistry::instance().find(target_node.type, target_node.subtype);
        if (!op_variant_opt) {
            throw GraphError(GraphErrc::NoOperation, "No operation for " + target_node.type + ":" + target_node.subtype);
        }

        // 4. 核心调度逻辑：使用 std::visit 分派任务
        std::visit([&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;
            
            // =======================================================
            // 执行路径 A: 整体计算 (Monolithic)
            // =======================================================
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                target_node.cached_output = op_func(target_node, monolithic_inputs);
            
            // =======================================================
            // 执行路径 B: 分块计算 (Tiled)
            // =======================================================
            } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                // a. 准备输出缓冲区
                if (monolithic_inputs.empty()) {
                    throw GraphError(GraphErrc::MissingDependency, "Tiled node '" + target_node.name + "' requires at least one image input to infer output size.");
                }
                const auto& first_input_buffer = monolithic_inputs[0]->image_buffer;
                // TODO: 实现更完善的输出尺寸推断机制。目前简化为与第一个输入相同。
                int out_width = first_input_buffer.width;
                int out_height = first_input_buffer.height;
                int out_channels = first_input_buffer.channels; // 简化假设
                ps::DataType out_type = first_input_buffer.type; // 简化假设

                target_node.cached_output = NodeOutput();
                auto& output_buffer = target_node.cached_output->image_buffer;
                output_buffer.width = out_width;
                output_buffer.height = out_height;
                output_buffer.channels = out_channels;
                output_buffer.type = out_type;
                
                // TODO: 替换为更通用的数据类型大小计算
                size_t pixel_size = sizeof(float); 
                output_buffer.step = out_width * out_channels * pixel_size;
                // 使用智能指针管理内存
                output_buffer.data.reset(new char[output_buffer.step * output_buffer.height], std::default_delete<char[]>());

                // b. 创建并执行分块任务 (单线程模拟调度)
                const int TILE_SIZE = 256; // 可配置的分块大小
                // TODO: 从操作元数据中获取光环大小
                const int HALO_SIZE = 16; 

                for (int y = 0; y < output_buffer.height; y += TILE_SIZE) {
                    for (int x = 0; x < output_buffer.width; x += TILE_SIZE) {
                        ps::TileTask task;
                        task.node = &target_node;

                        // 定义输出分块
                        task.output_tile.buffer = &output_buffer;
                        task.output_tile.roi = cv::Rect(x, y, 
                                                        std::min(TILE_SIZE, output_buffer.width - x),
                                                        std::min(TILE_SIZE, output_buffer.height - y));

                        // 为每个输入计算所需的分块 (包括 Halo)
                        for (const auto* input_node_output : monolithic_inputs) {
                            const auto& input_buffer = input_node_output->image_buffer;

                            ps::Tile input_tile;
                            // ===========================================================
                            // === FIX: 使用 const_cast 解决 const 正确性问题 ===
                            // 我们知道输入分块在 op 函数中是只读的，所以这个转换是安全的。
                            // 这是为了匹配 Tile 结构中非 const 的 buffer 指针成员。
                            input_tile.buffer = const_cast<ps::ImageBuffer*>(&input_buffer);
                            // ===========================================================
                            input_tile.roi = calculate_halo(task.output_tile.roi, HALO_SIZE, cv::Size(input_buffer.width, input_buffer.height));
                            task.input_tiles.push_back(input_tile);
                        }
                        
                        // 执行这个分块任务
                        execute_tile_task(task, op_func);
                    }
                }
            }
        }, *op_variant_opt);

        result_source = "computed";
        save_cache_if_configured(target_node, cache_precision);
        visiting[node_id] = false;

    } while(false);

    // 5. 计时和事件记录 (逻辑不变)
    if (enable_timing) {
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end_time - start_time;
        {
            std::lock_guard<std::mutex> lk(timing_mutex_);
            this->timing_results.node_timings.push_back({target_node.id, target_node.name, elapsed.count(), result_source});
            this->timing_results.total_ms += elapsed.count();
        }
        push_compute_event(target_node.id, target_node.name, result_source, elapsed.count());
    } else {
        push_compute_event(target_node.id, target_node.name, result_source, 0.0);
    }
    
    return *target_node.cached_output;
}


// --- 保持 compute() 和 clear_timing_results() 不变 ---

NodeOutput& NodeGraph::compute(int node_id, const std::string& cache_precision,
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
    std::unordered_map<int, bool> visiting;
    return compute_internal(node_id, cache_precision, visiting, enable_timing, !disable_disk_cache);
}

void NodeGraph::clear_timing_results() {
    std::lock_guard<std::mutex> lk(timing_mutex_);
    timing_results.node_timings.clear();
    timing_results.total_ms = 0.0;
}

} // namespace ps