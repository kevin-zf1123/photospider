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
#include "benchmark/benchmark_types.hpp"

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
 * @brief 计算节点输出（内部递归调用）
 *
 * 本函数用于计算指定节点的输出结果，具体流程如下：
 *   1. 缓存检测：首先检查内存缓存，如果命中直接返回；
 *      若允许使用磁盘缓存，则尝试从磁盘加载缓存数据。
 *
 *   2. 依赖节点计算与参数解析：若缓存均未命中，
 *      则遍历当前节点所有依赖的输入（包括参数和图像输入）并递归计算其输出，
 *      同时将各依赖节点的结果写入当前节点的运行时参数中。
 *      注意：函数内会标记节点访问状态，防止循环依赖，
 *      若检测到循环依赖则抛出异常。
 *
 *   3. 操作函数获取与执行：
 *      根据当前节点的类型和子类型，获取对应的操作函数，
 *      对操作函数的选择分为两种情况：
 *        - Monolithic 模式：整体计算，直接将依赖节点结果作为输入执行操作函数。
 *        - Tiled 模式：分块计算，对于图像计算需要根据输入图像推导输出图像的尺寸、通道数及数据类型，
 *          同时为输出图像分配缓冲区，并按 TILE_SIZE 划分区域逐块执行计算，
 *          每次计算时还会根据 HALO_SIZE 计算有效区域。
 *
 *   4. 计时与事件记录：
 *      在依赖解析和核心计算前后分别记录时间戳，
 *      并将各阶段计算耗时（依赖解析时长与核心执行时长）封装到 BenchmarkEvent 中，
 *      同时根据 enable_timing 标志更新全局计时结果。
 *
 *   5. 缓存保存与结果返回：
 *      如果节点经过计算获得输出，则保存至内存缓存（及可能的磁盘缓存），
 *      并返回计算后的节点输出。
 *
 * @param node_id           当前需要计算的节点 ID。
 * @param cache_precision   缓存精度配置，用以控制缓存读取与保存的策略。
 * @param visiting          用于检测循环依赖的标志映射，若同一节点在递归过程中重复访问则抛出异常。
 * @param enable_timing     标志是否启用计时，启用后将记录并保存详细的节点计算时长。
 * @param allow_disk_cache  是否允许从磁盘读取节点输出缓存，若允许且内存缓存未命中则尝试从磁盘加载。
 * @param benchmark_events  指向用于记录各阶段详细耗时数据的 BenchmarkEvent 向量的指针（可为 nullptr）。
 *
 * @return NodeOutput& 返回计算或加载到缓存的节点输出数据。
 *
 * @throws GraphError 当检测到循环依赖、缺失依赖输出或找不到对应的操作函数时，会抛出此异常。
 *
 * @note
 *   - 本函数内部采用递归调用，对每个依赖节点均进行相同流程的计算。
 *   - 对于 Tiled 计算模式，若节点非 "image_generator" 且缺少图像输入，将抛出异常。
 *   - 详细的计算耗时（依赖解析和核心执行）仅在 benchmark_events 不为 nullptr 时记录。
 */
NodeOutput& NodeGraph::compute_internal(int node_id,
    const std::string& cache_precision,
    std::unordered_map<int, bool>& visiting,
    bool enable_timing,
    bool allow_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events)
{
    auto& target_node = nodes.at(node_id);
    std::string result_source = "unknown";

    // 将计时器移到更早的位置，以捕获依赖解析时间
    auto start_time_total = std::chrono::high_resolution_clock::now();

    // 创建并初始化 BenchmarkEvent
    BenchmarkEvent current_event;
    current_event.node_id = node_id;
    current_event.op_name = make_key(target_node.type, target_node.subtype);
    current_event.dependency_start_time = start_time_total;

    do {
        // 1. 内存／磁盘缓存检测
        if (target_node.cached_output.has_value()) {
            result_source = "memory_cache";
            break;
        }
        if (allow_disk_cache && try_load_from_disk_cache(target_node)) {
            result_source = "disk_cache";
            break;
        }

        // 2. 循环依赖检测
        if (visiting[node_id]) {
            throw GraphError(GraphErrc::Cycle,
                "Cycle detected: " + std::to_string(node_id));
        }
        visiting[node_id] = true;

        // 3. 参数依赖解析
        // Deep clone parameters to ensure this node's runtime view is independent and
        // avoids yaml-cpp memory_holder merging across threads.
        target_node.runtime_parameters =
            target_node.parameters
            ? YAML::Clone(target_node.parameters)
            : YAML::Node(YAML::NodeType::Map);

        for (auto const& p_input : target_node.parameter_inputs) {
            if (p_input.from_node_id < 0) continue;
            auto const& up_out = compute_internal(
                p_input.from_node_id,
                cache_precision,
                visiting,
                enable_timing,
                allow_disk_cache,
                benchmark_events);
            auto it = up_out.data.find(p_input.from_output_name);
            if (it == up_out.data.end()) {
                throw GraphError(GraphErrc::MissingDependency,
                    "Node " + std::to_string(p_input.from_node_id) +
                    " did not produce output '" +
                    p_input.from_output_name + "'");
            }
            target_node.runtime_parameters[
                p_input.to_parameter_name] = it->second;
        }

        // 4. 图像依赖解析
        std::vector<const NodeOutput*> monolithic_inputs;
        for (auto const& i_input : target_node.image_inputs) {
            if (i_input.from_node_id < 0) continue;
            monolithic_inputs.push_back(
                &compute_internal(
                    i_input.from_node_id,
                    cache_precision,
                    visiting,
                    enable_timing,
                    allow_disk_cache,
                    benchmark_events));
        }

        if (!monolithic_inputs.empty()) {
            const auto& first_buf = monolithic_inputs.front()->image_buffer;
            if (first_buf.width > 0 && first_buf.height > 0) {
                target_node.last_input_size_hp = cv::Size(first_buf.width, first_buf.height);
            } else {
                target_node.last_input_size_hp.reset();
            }
        } else {
            target_node.last_input_size_hp.reset();
        }

        // 5. 查找并派发 Op
        auto op_opt = OpRegistry::instance().resolve_for_intent(
            target_node.type, target_node.subtype, ComputeIntent::GlobalHighPrecision);
        if (!op_opt) {
            throw GraphError(GraphErrc::NoOperation,
                "No op for " + target_node.type +
                ":" + target_node.subtype);
        }

        // 6. 执行计时开始
        current_event.execution_start_time =
            std::chrono::high_resolution_clock::now();

        try {
        std::visit([&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;

            // Monolithic
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                target_node.cached_output =
                    op_func(target_node, monolithic_inputs);
            }

            else if constexpr (std::is_same_v<T, TileOpFunc>) {
                
                // [核心修复] 为 image_mixing 实现 merge_strategy
                std::vector<NodeOutput> normalized_storage;
                std::vector<const NodeOutput*> inputs_for_tiling = monolithic_inputs;
                
                bool is_mixing = (target_node.type == "image_mixing");
                if (is_mixing && monolithic_inputs.size() >= 2) {
                    if (monolithic_inputs[0]->image_buffer.width == 0 || monolithic_inputs[0]->image_buffer.height == 0) {
                        throw GraphError(GraphErrc::InvalidParameter, "Base image for image_mixing node " + std::to_string(target_node.id) + " is empty.");
                    }

                    const auto& base_buffer = monolithic_inputs[0]->image_buffer;
                    const int base_w = base_buffer.width;
                    const int base_h = base_buffer.height;
                    const int base_c = base_buffer.channels;

                    const std::string strategy = as_str(target_node.runtime_parameters, "merge_strategy", "resize");
                    
                    normalized_storage.reserve(monolithic_inputs.size() - 1);

                    for (size_t i = 1; i < monolithic_inputs.size(); ++i) {
                        const auto& current_buffer = monolithic_inputs[i]->image_buffer;
                        
                        if (current_buffer.width == 0 || current_buffer.height == 0) {
                             throw GraphError(GraphErrc::InvalidParameter, "Secondary image for image_mixing node " + std::to_string(target_node.id) + " is empty.");
                        }

                        if (current_buffer.width == base_w && current_buffer.height == base_h && current_buffer.channels == base_c) {
                            continue; // Already matches, no normalization needed
                        }
                        
                        cv::Mat current_mat = toCvMat(current_buffer);
                        
                        // [*** 核心修复 ***] Handle both resize and crop
                        if (current_mat.cols != base_w || current_mat.rows != base_h) {
                            if (strategy == "resize") {
                                cv::resize(current_mat, current_mat, cv::Size(base_w, base_h), 0, 0, cv::INTER_LINEAR);
                            } else if (strategy == "crop") {
                                cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w), std::min(current_mat.rows, base_h));
                                cv::Mat cropped = cv::Mat::zeros(base_h, base_w, current_mat.type());
                                current_mat(crop_roi).copyTo(cropped(crop_roi));
                                current_mat = cropped;
                            } else {
                                throw GraphError(GraphErrc::InvalidParameter, "Unsupported merge_strategy '" + strategy + "' for tiled image_mixing.");
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
                            } else if (current_mat.channels() != base_c) {
                                throw GraphError(GraphErrc::InvalidParameter, "Unsupported channel conversion for image_mixing: " + std::to_string(current_mat.channels()) + " -> " + std::to_string(base_c));
                            }
                        }

                        NodeOutput temp_output;
                        temp_output.image_buffer = fromCvMat(current_mat);
                        normalized_storage.push_back(std::move(temp_output));
                        inputs_for_tiling[i] = &normalized_storage.back();
                    }
                }


                if (inputs_for_tiling.empty() && target_node.type != "image_generator")
                {
                    throw GraphError(GraphErrc::MissingDependency,
                        "Tiled node '" + target_node.name +
                        "' requires at least one image input");
                }

                // 推断输出尺寸
                int out_w = inputs_for_tiling.empty()
                    ? as_int_flexible(target_node.runtime_parameters,
                        "width", 256)
                    : inputs_for_tiling[0]->image_buffer.width;
                int out_h = inputs_for_tiling.empty()
                    ? as_int_flexible(target_node.runtime_parameters,
                        "height", 256)
                    : inputs_for_tiling[0]->image_buffer.height;
                int out_c = inputs_for_tiling.empty()
                    ? 1
                    : inputs_for_tiling[0]->image_buffer.channels;
                auto out_t = inputs_for_tiling.empty()
                    ? ps::DataType::FLOAT32
                    : inputs_for_tiling[0]->image_buffer.type;

                // 分配输出缓冲
                target_node.cached_output = NodeOutput();
                auto& ob = target_node.cached_output->image_buffer;
                ob.width = out_w; ob.height = out_h;
                ob.channels = out_c; ob.type = out_t;
                size_t pix_sz = sizeof(float);
                ob.step = out_w * out_c * pix_sz;
                ob.data.reset(
                    new char[ob.step * ob.height],
                    std::default_delete<char[]>());

                const int TILE_SIZE = 256, HALO_SIZE = 16;
                for (int y = 0; y < ob.height; y += TILE_SIZE) {
                    for (int x = 0; x < ob.width; x += TILE_SIZE) {
                        ps::TileTask task;
                        task.node = &target_node;
                        task.output_tile.buffer = &ob;
                        task.output_tile.roi = cv::Rect(
                            x, y,
                            std::min(TILE_SIZE, ob.width - x),
                            std::min(TILE_SIZE, ob.height - y));

                        // Decide halo usage based on op; only gaussian_blur needs halo
                        const bool needs_halo = (target_node.type == "image_process" && target_node.subtype == "gaussian_blur");
                        for (auto const* in_out : inputs_for_tiling) {
                            ps::Tile in_tile;
                            in_tile.buffer = const_cast<ps::ImageBuffer*>(&in_out->image_buffer);
                            if (needs_halo) {
                                in_tile.roi = calculate_halo(
                                    task.output_tile.roi,
                                    HALO_SIZE,
                                    {in_out->image_buffer.width,
                                     in_out->image_buffer.height});
                            } else {
                                // For non-convolution ops, use exact output ROI to avoid size mismatch/striping
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
        } catch (...) {
            throw GraphError(GraphErrc::ComputeError,
                             "Node " + std::to_string(target_node.id) + " (" + target_node.name + ") failed: unknown exception");
        }

        current_event.execution_end_time = std::chrono::high_resolution_clock::now();
        result_source = "computed";
        // Phase 1: Mirror legacy cache to HP cache and bump version for old path
        try {
            target_node.cached_output_high_precision = *target_node.cached_output;
            target_node.hp_version++;
        } catch (...) {
            // Best-effort; do not fail compute due to mirror issues
        }
        save_cache_if_configured(target_node, cache_precision);
        visiting[node_id] = false;

    } while (false);

    // 7. 结束计时
    auto end_time_total = std::chrono::high_resolution_clock::now();
    if (result_source == "computed") {
        current_event.execution_end_time = end_time_total;
    } else {
        // 缓存命中时，执行时长设为 0
        current_event.execution_start_time = end_time_total;
        current_event.execution_end_time = end_time_total;
    }

    current_event.source = result_source;
    double execution_duration_ms = std::chrono::duration<double, std::milli>(current_event.execution_end_time - current_event.execution_start_time).count();
    if (execution_duration_ms < 0) execution_duration_ms = 0.0; // 保险
    // 8. 全局与本地计时记录
    if (enable_timing) {
        auto tot = std::chrono::duration<double,
            std::milli>(end_time_total - start_time_total);
        {
            std::lock_guard lk(timing_mutex_);
            timing_results.node_timings.push_back({
                target_node.id,
                target_node.name,
                execution_duration_ms, 
                result_source
            });
        }
        push_compute_event(
            target_node.id,
            target_node.name,
            result_source,
            execution_duration_ms);
    } else {
        push_compute_event(
            target_node.id,
            target_node.name,
            result_source,
            0.0);
    }

    // 9. 细节 Benchmark 记录
    if (benchmark_events) {
        current_event.execution_duration_ms =
            std::chrono::duration<double,
                std::milli>(current_event.execution_end_time -
                                 current_event.execution_start_time)
                .count();
        current_event.dependency_duration_ms =
            std::chrono::duration<double,
                std::milli>(current_event.execution_start_time -
                                 current_event.dependency_start_time)
                .count();
        benchmark_events->push_back(current_event);
    }

    return *target_node.cached_output;
}


/**
 * @brief 计算指定节点的输出结果。
 *
 * 该函数实现了节点图的顶层计算逻辑，主要流程如下：
 *
 * 1. 节点存在性验证：
 *    - 检查传入的 node_id 是否存在于节点图中，若不存在则抛出 GraphError 异常。
 *
 * 2. 计时设置：
 *    - 如果启用计时（enable_timing 为 true），在计算前先调用 clear_timing_results() 清除之前的计时数据。
 *
 * 3. 强制刷新缓存（force_recache）：
 *    - 当 force_recache 为 true 时，通过拓扑后序遍历（topo_postorder_from）获取所有依赖节点，
 *      并在持有互斥锁的情况下，对于非 preserved 节点（或当前计算节点自身）清除其缓存输出。
 *
 * 4. 实际计算：
 *    - 使用 unordered_map 记录节点访问状态，以防止循环依赖。
 *    - 调用 compute_internal 函数进行递归计算，并传递缓存精度、计时和磁盘缓存控制等参数。
 *
 * 5. 累计计时结果：
 *    - 若启用了计时功能，则遍历所有节点的计时信息，并将各节点耗时累加得到总执行时长。
 *
 * @param node_id            要计算的节点ID。
 * @param cache_precision    缓存精度说明（通常为字符串形式），用于控制缓存读取与保存策略。
 * @param force_recache      是否强制刷新缓存。如果为 true，则清除所有相关节点的缓存输出（除 preserved 节点外）。
 * @param enable_timing      是否启用计时功能，用以记录并统计函数调用耗时。
 * @param disable_disk_cache 是否禁用磁盘缓存，若为 true，则在计算过程中不尝试读取磁盘缓存。
 * @param benchmark_events   指向用于记录每个节点详细计时数据的 BenchmarkEvent 向量的指针（可为 nullptr）。
 *
 * @return NodeOutput& 返回指定节点计算后的输出数据引用。
 *
 * @throws GraphError 当指定的节点不存在，或者在计算过程中遇到错误（如循环依赖、缺失依赖或无效操作）时抛出异常。
 */
NodeOutput& NodeGraph::compute(int node_id, const std::string& cache_precision,
                               bool force_recache, bool enable_timing,
                               bool disable_disk_cache,
                               std::vector<BenchmarkEvent>* benchmark_events) {
    if (!has_node(node_id)) {
        throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
    }

    // 在顶层调用开始前清空计时结果
    if (enable_timing) {
        clear_timing_results();
        // [修改] 在每次计算开始时重置IO计时器
        total_io_time_ms = 0.0;
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

    std::unordered_map<int, bool> visiting;
    // Do not use disk cache when force_recache is requested to avoid stale/corrupt reads
    bool allow_disk_cache = (!disable_disk_cache) && (!force_recache);
    NodeOutput* result_ptr = nullptr;
    try {
        result_ptr = &compute_internal(node_id, cache_precision, visiting, enable_timing, allow_disk_cache, benchmark_events);
    } catch (const GraphError&) {
        throw;
    } catch (const cv::Exception& e) {
        const auto& n = nodes.at(node_id);
        throw GraphError(GraphErrc::ComputeError, "Node " + std::to_string(n.id) + " (" + n.name + ") failed: " + std::string(e.what()));
    } catch (const std::exception& e) {
        const auto& n = nodes.at(node_id);
        throw GraphError(GraphErrc::ComputeError, "Node " + std::to_string(n.id) + " (" + n.name + ") failed: " + std::string(e.what()));
    } catch (...) {
        const auto& n = nodes.at(node_id);
        throw GraphError(GraphErrc::ComputeError, "Node " + std::to_string(n.id) + " (" + n.name + ") failed: unknown exception");
    }
    
    // 在所有计算结束后，累加总时间
    if (enable_timing) {
        double total = 0.0;
        for(const auto& timing : timing_results.node_timings) {
            total += timing.elapsed_ms;
        }
        timing_results.total_ms = total;
    }

    return *result_ptr;
}

// Phase 1 overload: intent-based entry to sequential compute
NodeOutput& NodeGraph::compute(ComputeIntent intent,
                               int node_id, const std::string& cache_precision,
                               bool force_recache, bool enable_timing,
                               bool disable_disk_cache,
                               std::vector<BenchmarkEvent>* benchmark_events,
                               std::optional<cv::Rect> dirty_roi) {
    switch (intent) {
        case ComputeIntent::GlobalHighPrecision:
            return compute(node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
        case ComputeIntent::RealTimeUpdate:
            if (!dirty_roi.has_value()) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "RealTimeUpdate intent requires a dirty ROI region.");
            }
            return compute_real_time_update(nullptr, node_id, cache_precision,
                                            force_recache, enable_timing,
                                            disable_disk_cache, benchmark_events,
                                            *dirty_roi);
        default:
            return compute(node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
    }
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
