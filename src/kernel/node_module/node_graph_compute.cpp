/**
 * @文件说明
 * 本文件包含了图计算引擎中节点执行和依赖管理的核心逻辑，以及实现分块图像计算任务的辅助函数。
 *
 * 主要包含以下功能模块：
 *
 * 1. 辅助函数:
 *    - execute_tile_task:
 *         用于执行一个分块计算任务，调用给定的分块操作函数。
 *    - calculate_halo:
 *         为输出分块计算并返回包含光环的输入区域，确保计算过程中不会超出输入图像的边界。
 *
 * 2. 核心计算逻辑:
 *    - compute_internal:
 *         递归计算并返回指定节点的输出。该函数执行以下几个阶段：
 *         a. 缓存查找：优先从内存或磁盘缓存中加载已存在的节点计算结果。
 *         b. 依赖解析：递归计算所有上游节点，解析输入参数及图像数据。若存在循环依赖或缺失依赖则抛出异常。
 *         c. 操作分派：根据节点类型和子类型，使用 std::visit 调用对应的操作函数:
 *              - 整体（Monolithic）操作：直接计算并返回完整的图像输出。
 *              - 分块（Tiled）操作：将图像按分块进行处理，每个分块可能扩展光环区域以满足边界需求。
 *         d. 时间计量与事件记录：统计节点计算所耗费的时间，并更新内部性能指标与事件记录。
 *
 *    - compute:
 *         对外接口，用于计算并返回指定节点的输出。该函数在开始计算前可能清除部分缓存，并在计算完成后更新总的执行时间。
 *
 * 3. 定时统计:
 *    - clear_timing_results:
 *         清除所有节点计时记录，并重置总计时，用于重新开始性能统计。
 *
 * @注意事项：
 * - 程序中对于分块计算设有固定的 TILE_SIZE 和 HALO_SIZE，目前 HALO_SIZE 被固定为 16 像素。
 * - 异常处理机制保证了循环依赖、缺失依赖和无效操作类型都能被及时捕捉和处理。
 * - 依赖于全局缓存与磁盘缓存机制，以提升图计算的性能，但需要根据具体应用进行配置。
 */
#include "node_graph.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器
#include "kernel/param_utils.hpp"
#include <chrono>
#include <unordered_map>
#include <variant>

namespace ps {

// --- 阶段四新增：辅助函数 ---

/**
 * @brief 执行一个分块计算任务。
 *
 * 本函数接受一个包含节点、输出分块和输入分块的任务描述（TileTask 结构体）以及一个
 * 分块操作函数，并对任务中的节点直接调用操作函数，从而在指定的输出分块中生成结果。
 *
 * @param task 包含以下内容：
 *             - node: 要操作的节点，
 *             - output_tile: 存放结果的输出分块，
 *             - input_tiles: 用于操作的输入分块集合。
 * @param tiled_op 分块操作函数，用于处理节点和其分块数据。
 */
void execute_tile_task(const ps::TileTask& task, const TileOpFunc& tiled_op) {
    // 直接调用新的分块操作函数
    tiled_op(*task.node, task.output_tile, task.input_tiles);
}

/**
 * @brief 为输出分块计算依赖的输入分块区域（包含光环）。
 *
 * 本函数通过在输出分块的感兴趣区域（ROI）周围添加指定大小的光环，
 * 来计算相应输入分块的区域，同时确保扩展后的区域不会超出输入图像的边界。
 * 如果光环大小非正，则直接返回原始的ROI。
 *
 * @param roi 输出分块对应的原始感兴趣区域（ROI）。
 * @param halo_size 光环大小，在各个方向上需要额外扩展的像素数。
 * @param bounds 输入图像的完整尺寸（宽和高），用于防止计算出的区域超出边界。
 *
 * @return 扩展后的包含光环的ROI区域。
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

/**
 * @brief 内部计算函数，用于递归计算图中指定节点的输出结果。
 *
 * 该函数执行下面几个主要步骤：
 * 1. 缓存检查：
 *    - 如果节点已有内存缓存，则直接返回该输出；
 *    - 如果允许使用磁盘缓存，则尝试从磁盘中加载缓存数据。
 *
 * 2. 依赖计算与参数解析：
 *    - 检测并避免依赖循环，一旦检测到循环则抛出异常；
 *    - 为节点设置运行时参数，并递归计算所有参数输入对应的上游节点的输出；
 *    - 收集所有图像输入，为后续计算准备。
 *
 * 3. 操作函数调度：
 *    - 根据节点类型和子类型查询对应的操作函数（可能为整体计算或分块计算）；
 *    - 使用 std::visit 分派任务：
 *      - 对于整体（Monolithic）操作，直接调用对应函数生成完整输出；
 *      - 对于分块（Tiled）操作，根据输入推断输出尺寸，分配输出缓冲区，
 *        并以单线程方式对图像进行分块计算，每个分块调用操作函数处理。
 *
 * 4. 计时与事件记录：
 *    - 若启用计时，则统计计算耗时；
 *    - 将计算结果（包括节点ID、名称、耗时、数据来源）记录入 timing_results 及事件队列。
 *
 * @param node_id          待计算节点的 ID。
 * @param cache_precision  缓存精度标识，用于磁盘缓存逻辑。
 * @param visiting         用于记录当前访问状态的映射，防止依赖循环。
 * @param enable_timing    是否启用计时统计。
 * @param allow_disk_cache 是否允许从磁盘加载缓存。
 *
 * @return 返回计算后生成的节点输出 (NodeOutput)。
 *
 * @throw GraphError 当检测到依赖循环、缺失依赖输出或未能找到适配的操作函数时，会抛出异常。
 */
NodeOutput& NodeGraph::compute_internal(int node_id, const std::string& cache_precision, std::unordered_map<int, bool>& visiting, bool enable_timing, bool allow_disk_cache) {
    auto& target_node = nodes.at(node_id);
    std::string result_source = "unknown";
    
    auto start_time = std::chrono::high_resolution_clock::now();

    do { // 使用 do-while(false) 结构来方便地 break
        // 1. 缓存检查 (逻辑不变)
        if (target_node.cached_output.has_value()) { result_source = "memory_cache"; break; }
        if (allow_disk_cache && try_load_from_disk_cache(target_node)) { result_source = "disk_cache"; break; }

        // 2. 依赖计算与参数解析 (逻辑不变)
        if (visiting[node_id]) throw GraphError(GraphErrc::Cycle, "Cycle detected: " + std::to_string(node_id));
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
        if (!op_variant_opt) throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type + ":" + target_node.subtype);

        // 重置计时器，准备开始真正的计算
        start_time = std::chrono::high_resolution_clock::now();

        // 4. *** 核心调度逻辑：使用 std::visit 分派任务 ***
        std::visit([&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;
            
            // --- 路径 A: Monolithic (整体计算) ---
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                // 直接调用整体操作函数，它会返回完整的 NodeOutput
                target_node.cached_output = op_func(target_node, monolithic_inputs);
            } 
            // --- 路径 B: Tiled (分块计算) ---
            else if constexpr (std::is_same_v<T, TileOpFunc>) {
                if (monolithic_inputs.empty() && target_node.type != "image_generator") {
                     throw GraphError(GraphErrc::MissingDependency, "Tiled node '" + target_node.name + "' requires at least one image input to infer output size.");
                }
                
                // 推断输出尺寸 (暂时简化为与第一个输入相同)
                int out_width = monolithic_inputs.empty() ? as_int_flexible(target_node.runtime_parameters, "width", 256) : monolithic_inputs[0]->image_buffer.width;
                int out_height = monolithic_inputs.empty() ? as_int_flexible(target_node.runtime_parameters, "height", 256) : monolithic_inputs[0]->image_buffer.height;
                int out_channels = monolithic_inputs.empty() ? 1 : monolithic_inputs[0]->image_buffer.channels;
                ps::DataType out_type = monolithic_inputs.empty() ? ps::DataType::FLOAT32 : monolithic_inputs[0]->image_buffer.type;

                // 准备输出缓冲区
                target_node.cached_output = NodeOutput();
                auto& output_buffer = target_node.cached_output->image_buffer;
                output_buffer.width = out_width;
                output_buffer.height = out_height;
                output_buffer.channels = out_channels;
                output_buffer.type = out_type;
                
                size_t pixel_size = sizeof(float); // 假设
                output_buffer.step = out_width * out_channels * pixel_size;
                output_buffer.data.reset(new char[output_buffer.step * output_buffer.height], std::default_delete<char[]>());

                // 单线程分块循环 (与你之前的实现相同)
                const int TILE_SIZE = 256;
                const int HALO_SIZE = 16; // 暂时硬编码

                for (int y = 0; y < output_buffer.height; y += TILE_SIZE) {
                    for (int x = 0; x < output_buffer.width; x += TILE_SIZE) {
                        ps::TileTask task;
                        task.node = &target_node;
                        task.output_tile.buffer = &output_buffer;
                        task.output_tile.roi = cv::Rect(x, y, 
                                                        std::min(TILE_SIZE, output_buffer.width - x),
                                                        std::min(TILE_SIZE, output_buffer.height - y));

                        for (const auto* input_node_output : monolithic_inputs) {
                            const auto& input_buffer = input_node_output->image_buffer;
                            ps::Tile input_tile;
                            input_tile.buffer = const_cast<ps::ImageBuffer*>(&input_buffer);
                            input_tile.roi = calculate_halo(task.output_tile.roi, HALO_SIZE, cv::Size(input_buffer.width, input_buffer.height));
                            task.input_tiles.push_back(input_tile);
                        }
                        
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
        }
        push_compute_event(target_node.id, target_node.name, result_source, elapsed.count());
    } else {
        push_compute_event(target_node.id, target_node.name, result_source, 0.0);
    }
    
    return *target_node.cached_output;
}


/**
 * @brief 计算指定节点的输出结果。
 *
 * 该函数根据传入的节点ID计算出节点的输出，同时支持缓存刷新、计时、以及磁盘缓存控制。
 * 详细功能说明如下：
 *
 * 1. 节点检查：
 *    - 首先检查传入的 node_id 是否在图中存在。
 *    - 如果指定的节点不存在，则抛出 GraphError 异常，并提示未找到该节点。
 *
 * 2. 计时功能：
 *    - 如果 enable_timing 参数为 true，在开始计算之前会调用 clear_timing_results() 清空之前的计时记录。
 *
 * 3. 强制刷新缓存（force_recache）：
 *    - 当 force_recache 为 true 时，将对计算节点及其所有依赖节点进行遍历，执行以下操作：
 *      a. 使用 std::scoped_lock 对图的互斥锁进行加锁，保证线程安全。
 *      b. 通过拓扑排序（后序遍历 topo_postorder_from）获取所有依赖节点。
 *      c. 对于每个依赖节点：
 *         - 如果该节点存在且当前节点不为 preserved（或为当前计算节点本身），则清除其缓存输出（cached_output）。
 *
 * 4. 实际计算：
 *    - 利用 unordered_map 来记录访问状态，防止重复计算或死循环依赖。
 *    - 调用 compute_internal 执行节点的实际计算，其中参数包括节点ID、缓存精度、计时开关和磁盘缓存控制。
 *
 * 5. 计时结果累加：
 *    - 计算完成后，如果启用了计时功能，则遍历所有节点计时信息，并累加计算得到总耗时（total_ms）。
 *
 * @param node_id           要计算的节点ID。
 * @param cache_precision   缓存输出的精度描述，通常为字符串形式。
 * @param force_recache     是否强制刷新缓存（若为 true，则所有相关节点的缓存将被清除）。
 * @param enable_timing     是否启用计时功能，用于计算函数执行耗时。
 * @param disable_disk_cache 是否禁用磁盘缓存功能，若为 true，将不使用磁盘缓存。
 *
 * @return NodeOutput& 返回指定节点计算后的输出结果引用。
 *
 * @throws GraphError 当指定的节点不存在，或在计算过程中发生错误时会抛出该异常。
 */
NodeOutput& NodeGraph::compute(int node_id, const std::string& cache_precision,
                               bool force_recache, bool enable_timing,
                               bool disable_disk_cache) {
    if (!has_node(node_id)) {
        throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
    }

    // 在顶层调用开始前清空计时结果
    if (enable_timing) {
        clear_timing_results();
    }

    if (force_recache) {
        std::scoped_lock lock(graph_mutex_);
        try {
            auto deps = topo_postorder_from(node_id);
            for (int id : deps) {
                if (nodes.count(id)) {
                    // --- 核心逻辑修改 ---
                    auto& node_to_clear = nodes.at(id);
                    // 只有当节点不是 preserved，或者它本身就是计算目标时，才清除缓存
                    if (!node_to_clear.preserved || id == node_id) {
                        node_to_clear.cached_output.reset();
                    }
                    // --- 结束修改 ---
                }
            }
        } catch (const GraphError&) {}
    }

    std::unordered_map<int, bool> visiting;
    auto& result = compute_internal(node_id, cache_precision, visiting, enable_timing, !disable_disk_cache);

    // 在所有计算结束后，累加总时间
    if (enable_timing) {
        double total = 0.0;
        for(const auto& timing : timing_results.node_timings) {
            total += timing.elapsed_ms;
        }
        timing_results.total_ms = total;
    }

    return result;
}

/**
 * @brief 清空节点图的计时结果
 *
 * 该函数用于清除存储在 timing_results 对象中的所有计时数据。
 * 它首先通过 std::lock_guard 获取互斥锁以确保线程安全，然后清除节点计时记录并将总计时毫秒数重置为 0。
 */
void NodeGraph::clear_timing_results() {
    std::lock_guard<std::mutex> lk(timing_mutex_);
    timing_results.node_timings.clear();
    timing_results.total_ms = 0.0;
}

} // namespace ps