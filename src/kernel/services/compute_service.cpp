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
 *         b.
 * 依赖解析：递归计算所有上游节点，解析输入参数及图像数据。若存在循环依赖或缺失依赖则抛出异常。
 *         c. 操作分派：根据节点类型和子类型，使用 std::visit
 * 调用对应的操作函数:
 *              - 整体（Monolithic）操作：直接计算并返回完整的图像输出。
 *              -
 * 分块（Tiled）操作：将图像按分块进行处理，每个分块可能扩展光环区域以满足边界需求。
 *         d.
 * 时间计量与事件记录：统计节点计算所耗费的时间，并更新内部性能指标与事件记录。
 *
 *    - compute:
 *         对外接口，用于计算并返回指定节点的输出。该函数在开始计算前可能清除部分缓存，并在计算完成后更新总的执行时间。
 *
 * 3. 定时统计:
 *    - clear_timing_results:
 *         清除所有节点计时记录，并重置总计时，用于重新开始性能统计。
 *
 * @注意事项：
 * - 程序中对于分块计算设有固定的 TILE_SIZE 和 HALO_SIZE，目前 HALO_SIZE
 * 被固定为 16 像素。
 * - 异常处理机制保证了循环依赖、缺失依赖和无效操作类型都能被及时捕捉和处理。
 * -
 * 依赖于全局缓存与磁盘缓存机制，以提升图计算的性能，但需要根据具体应用进行配置。
 */
#include "kernel/services/compute_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "benchmark/benchmark_types.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/param_utils.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps {

ComputeService::ComputeService(GraphTraversalService& traversal,
                               GraphCacheService& cache,
                               GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

// --- 阶段四新增：辅助函数 ---

/**
 * @brief 执行一个分块计算任务。
 *
 * 本函数接受一个包含节点、输出分块和输入分块的任务描述（TileTask
 * 结构体）以及一个
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
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size,
                        const cv::Size& bounds) {
  if (halo_size <= 0) {
    return roi;
  }
  int x = std::max(0, roi.x - halo_size);
  int y = std::max(0, roi.y - halo_size);
  int right = std::min(bounds.width, roi.x + roi.width + halo_size);
  int bottom = std::min(bounds.height, roi.y + roi.height + halo_size);
  return cv::Rect(x, y, right - x, bottom - y);
}

namespace {

constexpr int kRtDownscaleFactor = 4;
constexpr int kRtTileSize = 16;
constexpr int kHpAlignment = kRtDownscaleFactor * kRtTileSize;
// 64px alignment on full-res
constexpr int kHpMacroTileSize = 256;
constexpr int kHpMicroTileSize = 64;

inline bool is_rect_empty(const cv::Rect& rect) {
  return rect.width <= 0 || rect.height <= 0;
}

inline cv::Rect clip_rect(const cv::Rect& rect, const cv::Size& bounds) {
  if (bounds.width <= 0 || bounds.height <= 0)
    return cv::Rect();
  int x0 = std::clamp(rect.x, 0, bounds.width);
  int y0 = std::clamp(rect.y, 0, bounds.height);
  int x1 = std::clamp(rect.x + rect.width, 0, bounds.width);
  int y1 = std::clamp(rect.y + rect.height, 0, bounds.height);
  if (x1 <= x0 || y1 <= y0)
    return cv::Rect();
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Rect expand_rect(const cv::Rect& rect, int padding) {
  if (padding <= 0 || is_rect_empty(rect))
    return rect;
  int x0 = rect.x - padding;
  int y0 = rect.y - padding;
  int x1 = rect.x + rect.width + padding;
  int y1 = rect.y + rect.height + padding;
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Rect align_rect(const cv::Rect& rect, int alignment) {
  if (is_rect_empty(rect) || alignment <= 1)
    return rect;
  int x0 = (rect.x / alignment) * alignment;
  int y0 = (rect.y / alignment) * alignment;
  int x1 = ((rect.x + rect.width + alignment - 1) / alignment) * alignment;
  int y1 = ((rect.y + rect.height + alignment - 1) / alignment) * alignment;
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) {
  if (is_rect_empty(a))
    return b;
  if (is_rect_empty(b))
    return a;
  int x0 = std::min(a.x, b.x);
  int y0 = std::min(a.y, b.y);
  int x1 = std::max(a.x + a.width, b.x + b.width);
  int y1 = std::max(a.y + a.height, b.y + b.height);
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Size scale_down_size(const cv::Size& size, int factor) {
  if (size.width <= 0 || size.height <= 0)
    return cv::Size();
  return cv::Size((size.width + factor - 1) / factor,
                  (size.height + factor - 1) / factor);
}

inline cv::Rect scale_down_rect(const cv::Rect& rect, int factor) {
  if (is_rect_empty(rect))
    return cv::Rect();
  int x0 = rect.x / factor;
  int y0 = rect.y / factor;
  int x1 = (rect.x + rect.width + factor - 1) / factor;
  int y1 = (rect.y + rect.height + factor - 1) / factor;
  return cv::Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

inline cv::Rect scale_up_rect(const cv::Rect& rect, int factor) {
  if (is_rect_empty(rect))
    return cv::Rect();
  int x0 = rect.x * factor;
  int y0 = rect.y * factor;
  int x1 = (rect.x + rect.width) * factor;
  int y1 = (rect.y + rect.height) * factor;
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

struct RtPlanEntry {
  cv::Rect roi_hp;
  cv::Rect roi_rt;
  cv::Size hp_size;
  cv::Size rt_size;
  int halo_hp = 0;
  int halo_rt = 0;
};

struct HpPlanEntry {
  cv::Rect roi_hp;
  cv::Size hp_size;
  int halo_hp = 0;
};

}  // anonymous namespace
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
 *        - Tiled
 * 模式：分块计算，对于图像计算需要根据输入图像推导输出图像的尺寸、通道数及数据类型，
 *          同时为输出图像分配缓冲区，并按 TILE_SIZE 划分区域逐块执行计算，
 *          每次计算时还会根据 HALO_SIZE 计算有效区域。
 *
 *   4. 计时与事件记录：
 *      在依赖解析和核心计算前后分别记录时间戳，
 *      并将各阶段计算耗时（依赖解析时长与核心执行时长）封装到 BenchmarkEvent
 * 中， 同时根据 enable_timing 标志更新全局计时结果。
 *
 *   5. 缓存保存与结果返回：
 *      如果节点经过计算获得输出，则保存至内存缓存（及可能的磁盘缓存），
 *      并返回计算后的节点输出。
 *
 * @param node_id           当前需要计算的节点 ID。
 * @param cache_precision   缓存精度配置，用以控制缓存读取与保存的策略。
 * @param visiting
 * 用于检测循环依赖的标志映射，若同一节点在递归过程中重复访问则抛出异常。
 * @param enable_timing 标志是否启用计时，启用后将记录并保存详细的节点计算时长。
 * @param allow_disk_cache
 * 是否允许从磁盘读取节点输出缓存，若允许且内存缓存未命中则尝试从磁盘加载。
 * @param benchmark_events  指向用于记录各阶段详细耗时数据的 BenchmarkEvent
 * 向量的指针（可为 nullptr）。
 *
 * @return NodeOutput& 返回计算或加载到缓存的节点输出数据。
 *
 * @throws GraphError
 * 当检测到循环依赖、缺失依赖输出或找不到对应的操作函数时，会抛出此异常。
 *
 * @note
 *   - 本函数内部采用递归调用，对每个依赖节点均进行相同流程的计算。
 *   - 对于 Tiled 计算模式，若节点非 "image_generator"
 * 且缺少图像输入，将抛出异常。
 *   - 详细的计算耗时（依赖解析和核心执行）仅在 benchmark_events 不为 nullptr
 * 时记录。
 */
NodeOutput& ComputeService::compute_internal(
    GraphModel& graph, int node_id, const std::string& cache_precision,
    std::unordered_map<int, bool>& visiting, bool enable_timing,
    bool allow_disk_cache, std::vector<BenchmarkEvent>* benchmark_events) {
  auto& nodes = graph.nodes;
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;
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
    if (allow_disk_cache &&
        cache_.try_load_from_disk_cache(graph, target_node)) {
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
    // Deep clone parameters to ensure this node's runtime view is independent
    // and avoids yaml-cpp memory_holder merging across threads.
    target_node.runtime_parameters = target_node.parameters
                                         ? YAML::Clone(target_node.parameters)
                                         : YAML::Node(YAML::NodeType::Map);

    for (auto const& p_input : target_node.parameter_inputs) {
      if (p_input.from_node_id < 0)
        continue;
      auto const& up_out = compute_internal(
          graph, p_input.from_node_id, cache_precision, visiting, enable_timing,
          allow_disk_cache, benchmark_events);
      auto it = up_out.data.find(p_input.from_output_name);
      if (it == up_out.data.end()) {
        throw GraphError(GraphErrc::MissingDependency,
                         "Node " + std::to_string(p_input.from_node_id) +
                             " did not produce output '" +
                             p_input.from_output_name + "'");
      }
      target_node.runtime_parameters[p_input.to_parameter_name] = it->second;
    }

    // 4. 图像依赖解析
    std::vector<const NodeOutput*> monolithic_inputs;
    for (auto const& i_input : target_node.image_inputs) {
      if (i_input.from_node_id < 0)
        continue;
      monolithic_inputs.push_back(&compute_internal(
          graph, i_input.from_node_id, cache_precision, visiting, enable_timing,
          allow_disk_cache, benchmark_events));
    }

    if (!monolithic_inputs.empty()) {
      const auto& first_buf = monolithic_inputs.front()->image_buffer;
      if (first_buf.width > 0 && first_buf.height > 0) {
        target_node.last_input_size_hp =
            cv::Size(first_buf.width, first_buf.height);
      } else {
        target_node.last_input_size_hp.reset();
      }
    } else {
      target_node.last_input_size_hp.reset();
    }

    // 5. 查找并派发 Op
    auto op_opt = OpRegistry::instance().resolve_for_intent(
        target_node.type, target_node.subtype,
        ComputeIntent::GlobalHighPrecision);
    if (!op_opt) {
      throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type +
                                                   ":" + target_node.subtype);
    }

    // 6. 执行计时开始
    current_event.execution_start_time =
        std::chrono::high_resolution_clock::now();

    try {
      std::visit(
          [&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;

            // Monolithic
              if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                target_node.cached_output =
                    op_func(target_node, monolithic_inputs);
              } else if constexpr (std::is_same_v<T, TileOpFunc>) {
              // [核心修复] 为 image_mixing 实现 merge_strategy
              std::vector<NodeOutput> normalized_storage;
              std::vector<const NodeOutput*> inputs_for_tiling =
                  monolithic_inputs;

              bool is_mixing = (target_node.type == "image_mixing");
              if (is_mixing && monolithic_inputs.size() >= 2) {
                if (monolithic_inputs[0]->image_buffer.width == 0 ||
                    monolithic_inputs[0]->image_buffer.height == 0) {
                  throw GraphError(GraphErrc::InvalidParameter,
                                   "Base image for image_mixing node " +
                                       std::to_string(target_node.id) +
                                       " is empty.");
                }

                const auto& base_buffer = monolithic_inputs[0]->image_buffer;
                const int base_w = base_buffer.width;
                const int base_h = base_buffer.height;
                const int base_c = base_buffer.channels;

                const std::string strategy = as_str(
                    target_node.runtime_parameters, "merge_strategy", "resize");

                normalized_storage.reserve(monolithic_inputs.size() - 1);

                for (size_t i = 1; i < monolithic_inputs.size(); ++i) {
                  const auto& current_buffer =
                      monolithic_inputs[i]->image_buffer;

                  if (current_buffer.width == 0 || current_buffer.height == 0) {
                    throw GraphError(GraphErrc::InvalidParameter,
                                     "Secondary image for image_mixing node " +
                                         std::to_string(target_node.id) +
                                         " is empty.");
                  }

                  if (current_buffer.width == base_w &&
                      current_buffer.height == base_h &&
                      current_buffer.channels == base_c) {
                    continue;  // Already matches, no normalization needed
                  }

                  cv::Mat current_mat = toCvMat(current_buffer);

                  // [*** 核心修复 ***] Handle both resize and crop
                  if (current_mat.cols != base_w ||
                      current_mat.rows != base_h) {
                    if (strategy == "resize") {
                      cv::resize(current_mat, current_mat,
                                 cv::Size(base_w, base_h), 0, 0,
                                 cv::INTER_LINEAR);
                    } else if (strategy == "crop") {
                      cv::Rect crop_roi(0, 0,
                                        std::min(current_mat.cols, base_w),
                                        std::min(current_mat.rows, base_h));
                      cv::Mat cropped =
                          cv::Mat::zeros(base_h, base_w, current_mat.type());
                      current_mat(crop_roi).copyTo(cropped(crop_roi));
                      current_mat = cropped;
                    } else {
                      throw GraphError(GraphErrc::InvalidParameter,
                                       "Unsupported merge_strategy '" +
                                           strategy +
                                           "' for tiled image_mixing.");
                    }
                  }

                  if (current_mat.channels() != base_c) {
                    if (current_mat.channels() == 1 &&
                        (base_c == 3 || base_c == 4)) {
                      std::vector<cv::Mat> planes(base_c, current_mat);
                      cv::merge(planes, current_mat);
                    } else if ((current_mat.channels() == 3 ||
                                current_mat.channels() == 4) &&
                               base_c == 1) {
                      cv::cvtColor(current_mat, current_mat,
                                   cv::COLOR_BGR2GRAY);
                    } else if (current_mat.channels() == 4 && base_c == 3) {
                      cv::cvtColor(current_mat, current_mat,
                                   cv::COLOR_BGRA2BGR);
                    } else if (current_mat.channels() == 3 && base_c == 4) {
                      cv::cvtColor(current_mat, current_mat,
                                   cv::COLOR_BGR2BGRA);
                    } else if (current_mat.channels() != base_c) {
                      throw GraphError(
                          GraphErrc::InvalidParameter,
                          "Unsupported channel conversion for image_mixing: " +
                              std::to_string(current_mat.channels()) + " -> " +
                              std::to_string(base_c));
                    }
                  }

                  NodeOutput temp_output;
                  temp_output.image_buffer = fromCvMat(current_mat);
                  normalized_storage.push_back(std::move(temp_output));
                  inputs_for_tiling[i] = &normalized_storage.back();
                }
              }

              if (inputs_for_tiling.empty() &&
                  target_node.type != "image_generator") {
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
              ob.width = out_w;
              ob.height = out_h;
              ob.channels = out_c;
              ob.type = out_t;
              size_t pix_sz = sizeof(float);
              ob.step = out_w * out_c * pix_sz;
              ob.data.reset(new char[ob.step * ob.height],
                            std::default_delete<char[]>());

              const int TILE_SIZE = 256, HALO_SIZE = 16;
              for (int y = 0; y < ob.height; y += TILE_SIZE) {
                for (int x = 0; x < ob.width; x += TILE_SIZE) {
                  ps::TileTask task;
                  task.node = &target_node;
                  task.output_tile.buffer = &ob;
                  task.output_tile.roi =
                      cv::Rect(x, y, std::min(TILE_SIZE, ob.width - x),
                               std::min(TILE_SIZE, ob.height - y));

                  // Decide halo usage based on op; only gaussian_blur needs
                  // halo
                  const bool needs_halo =
                      (target_node.type == "image_process" &&
                       target_node.subtype == "gaussian_blur");
                  for (auto const* in_out : inputs_for_tiling) {
                    ps::Tile in_tile;
                    in_tile.buffer =
                        const_cast<ps::ImageBuffer*>(&in_out->image_buffer);
                    if (needs_halo) {
                      in_tile.roi =
                          calculate_halo(task.output_tile.roi, HALO_SIZE,
                                         {in_out->image_buffer.width,
                                          in_out->image_buffer.height});
                    } else {
                      // For non-convolution ops, use exact output ROI to avoid
                      // size mismatch/striping
                      in_tile.roi = task.output_tile.roi;
                    }
                    task.input_tiles.push_back(in_tile);
                  }
                  execute_tile_task(task, op_func);
                }
              }
            }
          },
          *op_opt);
    } catch (const cv::Exception& e) {
      throw GraphError(GraphErrc::ComputeError,
                       "Node " + std::to_string(target_node.id) + " (" +
                           target_node.name +
                           ") failed: " + std::string(e.what()));
    } catch (const std::exception& e) {
      throw GraphError(GraphErrc::ComputeError,
                       "Node " + std::to_string(target_node.id) + " (" +
                           target_node.name +
                           ") failed: " + std::string(e.what()));
    } catch (...) {
      throw GraphError(GraphErrc::ComputeError,
                       "Node " + std::to_string(target_node.id) + " (" +
                           target_node.name + ") failed: unknown exception");
    }

    current_event.execution_end_time =
        std::chrono::high_resolution_clock::now();
    result_source = "computed";
    // Phase 1: Mirror legacy cache to HP cache and bump version for old path
    try {
      target_node.cached_output_high_precision = *target_node.cached_output;
      target_node.hp_version++;
    } catch (...) {
      // Best-effort; do not fail compute due to mirror issues
    }
    cache_.save_cache_if_configured(graph, target_node, cache_precision);
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
  double execution_duration_ms =
      std::chrono::duration<double, std::milli>(
          current_event.execution_end_time - current_event.execution_start_time)
          .count();
  if (execution_duration_ms < 0)
    execution_duration_ms = 0.0;  // 保险
  // 8. 全局与本地计时记录
  if (enable_timing) {
    auto tot = std::chrono::duration<double, std::milli>(end_time_total -
                                                         start_time_total);
    {
      std::lock_guard lk(timing_mutex);
      timing_results.node_timings.push_back({target_node.id, target_node.name,
                                             execution_duration_ms,
                                             result_source});
    }
    events_.push(target_node.id, target_node.name, result_source,
                 execution_duration_ms);
  } else {
    events_.push(target_node.id, target_node.name, result_source, 0.0);
  }

  // 9. 细节 Benchmark 记录
  if (benchmark_events) {
    current_event.execution_duration_ms =
        std::chrono::duration<double, std::milli>(
            current_event.execution_end_time -
            current_event.execution_start_time)
            .count();
    current_event.dependency_duration_ms =
        std::chrono::duration<double, std::milli>(
            current_event.execution_start_time -
            current_event.dependency_start_time)
            .count();
    benchmark_events->push_back(current_event);
  }

  return *target_node.cached_output;
}

NodeOutput& ComputeService::compute_node_no_recurse(
    GraphModel& graph, int node_id, const std::string& cache_precision,
    bool enable_timing, bool allow_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events) {
  auto& nodes = graph.nodes;
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;

  auto& target_node = nodes.at(node_id);
  // Fast path: already computed
  if (target_node.cached_output.has_value())
    return *target_node.cached_output;

  // Optionally load from disk cache for this node itself
  if (allow_disk_cache) {
    (void)cache_.try_load_from_disk_cache(graph, target_node);
    if (target_node.cached_output.has_value())
      return *target_node.cached_output;
  }

  // Ensure visibility of upstream writes before reading their cached outputs
  std::atomic_thread_fence(std::memory_order_acquire);

  // Build runtime parameters starting from static parameters.
  // Use deep clone to avoid yaml-cpp memory_holder merges across threads.
  target_node.runtime_parameters = target_node.parameters
                                       ? YAML::Clone(target_node.parameters)
                                       : YAML::Node(YAML::NodeType::Map);

  // Read parameter inputs from already computed parents
  for (const auto& p_input : target_node.parameter_inputs) {
    if (p_input.from_node_id < 0)
      continue;
    const auto itn = nodes.find(p_input.from_node_id);
    if (itn == nodes.end() || !itn->second.cached_output.has_value()) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Parallel scheduler bug: parameter input not ready for node " +
              std::to_string(node_id));
    }
    const auto& up_out = *itn->second.cached_output;
    auto it = up_out.data.find(p_input.from_output_name);
    if (it == up_out.data.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(p_input.from_node_id) +
                           " did not produce output '" +
                           p_input.from_output_name + "'");
    }
    target_node.runtime_parameters[p_input.to_parameter_name] = it->second;
  }

  // Gather image inputs from parents (must be ready)
  std::vector<const NodeOutput*> inputs_ready;
  inputs_ready.reserve(target_node.image_inputs.size());
  for (const auto& i_input : target_node.image_inputs) {
    if (i_input.from_node_id < 0)
      continue;
    const auto itn = nodes.find(i_input.from_node_id);
    if (itn == nodes.end() || !itn->second.cached_output.has_value()) {
      throw GraphError(
          GraphErrc::MissingDependency,
          "Parallel scheduler bug: image input not ready for node " +
              std::to_string(node_id));
    }
    inputs_ready.push_back(&*itn->second.cached_output);
  }

  if (!inputs_ready.empty()) {
    const auto& first_buf = inputs_ready.front()->image_buffer;
    if (first_buf.width > 0 && first_buf.height > 0) {
      target_node.last_input_size_hp =
          cv::Size(first_buf.width, first_buf.height);
    } else {
      target_node.last_input_size_hp.reset();
    }
  } else {
    target_node.last_input_size_hp.reset();
  }

  auto op_opt = OpRegistry::instance().resolve_for_intent(
      target_node.type, target_node.subtype,
      ComputeIntent::GlobalHighPrecision);
  if (!op_opt) {
    throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type +
                                                 ":" + target_node.subtype);
  }

  // Timing event for benchmarking
  BenchmarkEvent current_event;
  current_event.node_id = node_id;
  current_event.op_name = make_key(target_node.type, target_node.subtype);
  current_event.dependency_start_time =
      std::chrono::high_resolution_clock::now();
  current_event.execution_start_time = current_event.dependency_start_time;

  try {
    std::visit(
        [&](auto&& op_func) {
          using T = std::decay_t<decltype(op_func)>;
          if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
            target_node.cached_output = op_func(target_node, inputs_ready);
            target_node.cached_output_high_precision =
                *target_node.cached_output;
            target_node.hp_version++;
          } else if constexpr (std::is_same_v<T, TileOpFunc>) {
            // Prepare normalized inputs similar to compute_internal
            std::vector<NodeOutput> normalized_storage;
            std::vector<const NodeOutput*> inputs_for_tiling = inputs_ready;

            bool is_mixing = (target_node.type == "image_mixing");
            if (is_mixing && inputs_ready.size() >= 2) {
              const auto& base_buffer = inputs_ready[0]->image_buffer;
              if (base_buffer.width == 0 || base_buffer.height == 0) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "Base image for image_mixing is empty.");
              }
              const int base_w = base_buffer.width;
              const int base_h = base_buffer.height;
              const int base_c = base_buffer.channels;
              const std::string strategy = as_str(
                  target_node.runtime_parameters, "merge_strategy", "resize");
              normalized_storage.reserve(inputs_ready.size() - 1);
              for (size_t i = 1; i < inputs_ready.size(); ++i) {
                const auto& current_buffer = inputs_ready[i]->image_buffer;
                if (current_buffer.width == 0 || current_buffer.height == 0) {
                  throw GraphError(
                      GraphErrc::InvalidParameter,
                      "Secondary image for image_mixing is empty.");
                }
                cv::Mat current_mat = toCvMat(current_buffer);
                if (current_mat.cols != base_w || current_mat.rows != base_h) {
                  if (strategy == "resize") {
                    cv::resize(current_mat, current_mat,
                               cv::Size(base_w, base_h), 0, 0,
                               cv::INTER_LINEAR);
                  } else if (strategy == "crop") {
                    cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w),
                                      std::min(current_mat.rows, base_h));
                    cv::Mat cropped =
                        cv::Mat::zeros(base_h, base_w, current_mat.type());
                    current_mat(crop_roi).copyTo(cropped(crop_roi));
                    current_mat = cropped;
                  } else {
                    throw GraphError(
                        GraphErrc::InvalidParameter,
                        "Unsupported merge_strategy for tiled mixing.");
                  }
                }
                if (current_mat.channels() != base_c) {
                  if (current_mat.channels() == 1 &&
                      (base_c == 3 || base_c == 4)) {
                    std::vector<cv::Mat> planes(base_c, current_mat);
                    cv::merge(planes, current_mat);
                  } else if ((current_mat.channels() == 3 ||
                              current_mat.channels() == 4) &&
                             base_c == 1) {
                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
                  } else if (current_mat.channels() == 4 && base_c == 3) {
                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
                  } else if (current_mat.channels() == 3 && base_c == 4) {
                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
                  } else {
                    throw GraphError(
                        GraphErrc::InvalidParameter,
                        "Unsupported channel conversion in tiled mixing.");
                  }
                }
                NodeOutput tmp;
                tmp.image_buffer = fromCvMat(current_mat);
                normalized_storage.push_back(std::move(tmp));
                inputs_for_tiling[i] = &normalized_storage.back();
              }
            }

            // Infer output shape
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

            target_node.cached_output = NodeOutput();
            auto& ob = target_node.cached_output->image_buffer;
            ob.width = out_w;
            ob.height = out_h;
            ob.channels = out_c;
            ob.type = out_t;
            size_t pix_sz = sizeof(float);
            ob.step = out_w * out_c * pix_sz;
            ob.data.reset(new char[ob.step * ob.height],
                          std::default_delete<char[]>());

            const int TILE_SIZE = 256, HALO_SIZE = 16;
            for (int y = 0; y < ob.height; y += TILE_SIZE) {
              for (int x = 0; x < ob.width; x += TILE_SIZE) {
                ps::TileTask task;
                task.node = &target_node;
                task.output_tile.buffer = &ob;
                task.output_tile.roi =
                    cv::Rect(x, y, std::min(TILE_SIZE, ob.width - x),
                             std::min(TILE_SIZE, ob.height - y));
                const bool needs_halo =
                    (target_node.type == "image_process" &&
                     target_node.subtype == "gaussian_blur");
                for (auto const* in_out : inputs_for_tiling) {
                  ps::Tile in_tile;
                  in_tile.buffer =
                      const_cast<ps::ImageBuffer*>(&in_out->image_buffer);
                  if (needs_halo) {
                    in_tile.roi =
                        calculate_halo(task.output_tile.roi, HALO_SIZE,
                                       {in_out->image_buffer.width,
                                        in_out->image_buffer.height});
                  } else {
                    in_tile.roi = task.output_tile.roi;
                  }
                  task.input_tiles.push_back(in_tile);
                }
                execute_tile_task(task, op_func);
              }
            }
            // Mirror to HP cache and bump version after tiling finishes
            target_node.cached_output_high_precision =
                *target_node.cached_output;
            target_node.hp_version++;
          }
        },
        *op_opt);
  } catch (const cv::Exception& e) {
    throw GraphError(GraphErrc::ComputeError,
                     "Node " + std::to_string(target_node.id) + " (" +
                         target_node.name +
                         ") failed: " + std::string(e.what()));
  } catch (const std::exception& e) {
    throw GraphError(GraphErrc::ComputeError,
                     "Node " + std::to_string(target_node.id) + " (" +
                         target_node.name +
                         ") failed: " + std::string(e.what()));
  }

  // Save disk cache if configured
  cache_.save_cache_if_configured(graph, target_node, cache_precision);

  // Timing & events
  if (enable_timing) {
    current_event.execution_end_time =
        std::chrono::high_resolution_clock::now();
    current_event.source = "computed";
    current_event.execution_duration_ms =
        std::chrono::duration<double, std::milli>(
            current_event.execution_end_time -
            current_event.execution_start_time)
            .count();
    if (benchmark_events)
      benchmark_events->push_back(current_event);
    {
      std::lock_guard lk(timing_mutex);
      timing_results.node_timings.push_back(
          {target_node.id, target_node.name,
           current_event.execution_duration_ms, "computed"});
    }
    events_.push(target_node.id, target_node.name, "computed",
                 current_event.execution_duration_ms);
  } else {
    events_.push(target_node.id, target_node.name, "computed", 0.0);
  }
  return *target_node.cached_output;
}

NodeOutput& ComputeService::compute_high_precision_update(
    GraphModel& graph, GraphRuntime* runtime, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    const cv::Rect& dirty_roi) {
  auto& nodes = graph.nodes;

  (void)runtime;  // Unused for now, can be used for micro-task scheduling later
  (void)cache_precision;
  (void)disable_disk_cache;
  (void)benchmark_events;
  (void)enable_timing;

  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound, "Cannot compute HP update: node " +
                                              std::to_string(node_id) +
                                              " not found.");
  }
  if (is_rect_empty(dirty_roi)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute HP update: dirty ROI is empty.");
  }

  // --- 1. Planning: Propagate dirty ROI backwards to build an execution plan
  // ---
  auto execution_order = traversal_.topo_postorder_from(graph, node_id);
  if (execution_order.empty()) {
    execution_order.push_back(node_id);  // Handle single-node graph
  }

  // Helper to infer full-resolution (HP) size for any node
  std::unordered_map<int, cv::Size> hp_size_cache;
  std::function<cv::Size(int)> infer_hp_size = [&](int nid) -> cv::Size {
    if (hp_size_cache.count(nid))
      return hp_size_cache.at(nid);

    cv::Size size{0, 0};
    const Node& node = nodes.at(nid);

    auto take_from_output = [&](const std::optional<NodeOutput>& opt) -> bool {
      if (!opt.has_value())
        return false;
      const auto& img = opt->image_buffer;
      if (img.width <= 0 || img.height <= 0)
        return false;
      size = cv::Size(img.width, img.height);
      return true;
    };
    if (take_from_output(node.cached_output_high_precision)) {
      hp_size_cache[nid] = size;
      return size;
    }
    if (take_from_output(node.cached_output)) {
      hp_size_cache[nid] = size;
      return size;
    }
    if (node.cached_output_real_time) {
      const auto& buf = node.cached_output_real_time->image_buffer;
      if (buf.width > 0 && buf.height > 0) {
        size = cv::Size(buf.width * kRtDownscaleFactor,
                        buf.height * kRtDownscaleFactor);
        hp_size_cache[nid] = size;
        return size;
      }
    }
    for (const auto& input : node.image_inputs) {
      if (input.from_node_id < 0)
        continue;
      cv::Size parent_size = infer_hp_size(input.from_node_id);
      if (parent_size.width > 0 && parent_size.height > 0) {
        size = parent_size;
        break;
      }
    }
    if (size.width <= 0 || size.height <= 0) {
      int width = as_int_flexible(node.parameters, "width", 0);
      int height = as_int_flexible(node.parameters, "height", 0);
      if (width > 0 && height > 0) {
        size = cv::Size(width, height);
      }
    }
    hp_size_cache[nid] = size;
    return size;
  };

  auto infer_halo_hp = [&](const Node& node) -> int {
    // TODO: This should query operator metadata in the future
    if (node.type != "image_process")
      return 0;
    if (node.subtype == "gaussian_blur" ||
        node.subtype == "gaussian_blur_tiled") {
      int k = as_int_flexible(node.runtime_parameters, "ksize",
                              as_int_flexible(node.parameters, "ksize", 0));
      if (k <= 0)
        k = 3;
      if (k % 2 == 0)
        ++k;
      return std::max(0, k / 2);
    }
    if (node.subtype == "convolve") {
      int radius =
          as_int_flexible(node.runtime_parameters, "kernel_radius",
                          as_int_flexible(node.parameters, "kernel_radius", 0));
      radius = std::max(
          radius, as_int_flexible(node.runtime_parameters, "radius", radius));
      radius =
          std::max(radius, as_int_flexible(node.parameters, "radius", radius));
      int ksize =
          as_int_flexible(node.runtime_parameters, "kernel_size",
                          as_int_flexible(node.parameters, "kernel_size", 0));
      if (ksize <= 0)
        ksize = as_int_flexible(node.runtime_parameters, "ksize",
                                as_int_flexible(node.parameters, "ksize", 0));
      if (ksize > 0)
        radius = std::max(radius, std::max(0, (ksize - 1) / 2));
      if (radius <= 0)
        radius = 1;
      return radius;
    }
    return 0;
  };

  std::unordered_map<int, HpPlanEntry> plan;
  struct DownsampleRequest {
    int node_id = -1;
    cv::Rect roi_hp;
    int hp_version = 0;
  };
  std::vector<DownsampleRequest> downsample_requests;
  GraphRuntime* runtime_ptr = runtime;
  auto ensure_entry = [&](int nid) -> HpPlanEntry& {
    auto [it, inserted] = plan.emplace(nid, HpPlanEntry{});
    if (inserted) {
      it->second.hp_size = infer_hp_size(nid);
      it->second.halo_hp = infer_halo_hp(nodes.at(nid));
    } else {
      if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0)
        it->second.hp_size = infer_hp_size(nid);
      if (it->second.halo_hp == 0)
        it->second.halo_hp = infer_halo_hp(nodes.at(nid));
    }
    return it->second;
  };

  HpPlanEntry& target_entry = ensure_entry(node_id);
  target_entry.roi_hp =
      clip_rect(align_rect(dirty_roi, kHpMicroTileSize), target_entry.hp_size);
  if (is_rect_empty(target_entry.roi_hp)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI does not intersect node output.");
  }

  for (auto it = execution_order.rbegin(); it != execution_order.rend(); ++it) {
    int current_id = *it;
    if (!plan.count(current_id))
      continue;
    HpPlanEntry& current_entry = plan.at(current_id);
    if (is_rect_empty(current_entry.roi_hp))
      continue;

    const Node& current_node = nodes.at(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));

    auto propagate_fn = OpRegistry::instance().get_dirty_propagator(
        current_node.type, current_node.subtype);
    cv::Rect upstream_roi_hp = propagate_fn(current_node, current_entry.roi_hp);
    upstream_roi_hp = align_rect(upstream_roi_hp, kHpMicroTileSize);

    for (const auto& img_input : current_node.image_inputs) {
      if (img_input.from_node_id < 0)
        continue;
      int parent_id = img_input.from_node_id;
      HpPlanEntry& parent_entry = ensure_entry(parent_id);
      cv::Rect parent_roi = clip_rect(upstream_roi_hp, parent_entry.hp_size);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
    }
  }

  // Filter and finalize plan entries
  std::vector<int> erase_ids;
  for (auto& [nid, entry] : plan) {
    if (entry.hp_size.width <= 0 || entry.hp_size.height <= 0) {
      erase_ids.push_back(nid);
      continue;
    }
    entry.roi_hp =
        clip_rect(align_rect(entry.roi_hp, kHpMicroTileSize), entry.hp_size);
    if (is_rect_empty(entry.roi_hp)) {
      erase_ids.push_back(nid);
      continue;
    }
    if (entry.halo_hp == 0)
      entry.halo_hp = infer_halo_hp(nodes.at(nid));
  }
  for (int nid : erase_ids)
    plan.erase(nid);

  if (plan.empty())
    throw GraphError(GraphErrc::InvalidParameter,
                     "HP planner produced empty execution set.");
  if (force_recache) {
    for (const auto& [nid, _] : plan) {
      nodes.at(nid).cached_output_high_precision.reset();
      nodes.at(nid).hp_roi.reset();
      nodes.at(nid).hp_version = 0;
    }
  }

  // --- 2. Execution: Iterate forward and compute dirty tiles for each node in
  // plan ---
  auto compute_node_hp = [&](int nid, HpPlanEntry& entry) {
    if (is_rect_empty(entry.roi_hp))
      return;
    Node& node = nodes.at(nid);

    // Resolve runtime params
    node.runtime_parameters = node.parameters ? YAML::Clone(node.parameters)
                                              : YAML::Node(YAML::NodeType::Map);
    for (const auto& p_input : node.parameter_inputs) {
      if (p_input.from_node_id < 0)
        continue;
      const auto* parent_out =
          nodes.at(p_input.from_node_id)
                  .cached_output_high_precision.has_value()
              ? &*nodes.at(p_input.from_node_id).cached_output_high_precision
              : (nodes.at(p_input.from_node_id).cached_output.has_value()
                     ? &*nodes.at(p_input.from_node_id).cached_output
                     : nullptr);
      if (!parent_out)
        throw GraphError(
            GraphErrc::MissingDependency,
            "HP parameter input not ready for node " + std::to_string(nid));
      auto it_val = parent_out->data.find(p_input.from_output_name);
      if (it_val == parent_out->data.end())
        throw GraphError(GraphErrc::MissingDependency,
                         "HP parameter '" + p_input.from_output_name +
                             "' missing for node " + std::to_string(nid));
      node.runtime_parameters[p_input.to_parameter_name] = it_val->second;
    }

    // Gather input image pointers
    std::vector<const NodeOutput*> image_inputs_ready;
    for (const auto& img_input : node.image_inputs) {
      if (img_input.from_node_id < 0)
        continue;
      const auto* parent_out =
          nodes.at(img_input.from_node_id)
                  .cached_output_high_precision.has_value()
              ? &*nodes.at(img_input.from_node_id).cached_output_high_precision
              : (nodes.at(img_input.from_node_id).cached_output.has_value()
                     ? &*nodes.at(img_input.from_node_id).cached_output
                     : (nodes.at(img_input.from_node_id)
                                .cached_output_real_time.has_value()
                            ? &*nodes.at(img_input.from_node_id)
                                    .cached_output_real_time
                            : nullptr));
      if (!parent_out)
        throw GraphError(
            GraphErrc::MissingDependency,
            "HP image input not ready for node " + std::to_string(nid));
      image_inputs_ready.push_back(parent_out);
    }

    // Ensure output HP buffer exists and is correctly sized
    auto [channels, dtype] = [&]() -> std::pair<int, DataType> {
      if (node.cached_output_high_precision) {
        const auto& b = node.cached_output_high_precision->image_buffer;
        if (b.width > 0)
          return {b.channels, b.type};
      }
      for (const auto* in : image_inputs_ready) {
        const auto& b = in->image_buffer;
        if (b.width > 0)
          return {b.channels, b.type};
      }
      return {1, DataType::FLOAT32};
    }();
    if (!node.cached_output_high_precision)
      node.cached_output_high_precision = NodeOutput{};
    ImageBuffer& hp_buffer = node.cached_output_high_precision->image_buffer;
    if (hp_buffer.width != entry.hp_size.width ||
        hp_buffer.height != entry.hp_size.height ||
        hp_buffer.channels != channels || hp_buffer.type != dtype ||
        !hp_buffer.data) {
      hp_buffer.width = entry.hp_size.width;
      hp_buffer.height = entry.hp_size.height;
      hp_buffer.channels = channels;
      hp_buffer.type = dtype;
      hp_buffer.device = Device::CPU;
      size_t row_bytes =
          static_cast<size_t>(hp_buffer.width) * channels * sizeof(float);
      hp_buffer.step = row_bytes;
      hp_buffer.data.reset(new char[row_bytes * hp_buffer.height],
                           std::default_delete<char[]>());
      std::memset(hp_buffer.data.get(), 0, row_bytes * hp_buffer.height);
    }

    // Get tiled operator
    const auto* impls =
        OpRegistry::instance().get_implementations(node.type, node.subtype);
    const TileOpFunc* hp_tile_fn =
        (impls && impls->tiled_hp) ? &*impls->tiled_hp : nullptr;
    if (!hp_tile_fn)
      throw GraphError(
          GraphErrc::NoOperation,
          "No tiled HP operator for " + node.type + ":" + node.subtype);

    // Execute tiling logic
    TileTask task;
    task.node = &node;
    task.output_tile.buffer = &hp_buffer;
    const cv::Size out_bounds(hp_buffer.width, hp_buffer.height);

    // Prioritize Macro tiles
    cv::Rect macro_cover = align_rect(entry.roi_hp, kHpMacroTileSize);
    macro_cover = clip_rect(macro_cover, out_bounds);
    for (int y = macro_cover.y; y < macro_cover.y + macro_cover.height;
         y += kHpMacroTileSize) {
      for (int x = macro_cover.x; x < macro_cover.x + macro_cover.width;
           x += kHpMacroTileSize) {
        cv::Rect macro_tile(
            x, y,
            std::min(kHpMacroTileSize, macro_cover.x + macro_cover.width - x),
            std::min(kHpMacroTileSize, macro_cover.y + macro_cover.height - y));
        macro_tile = clip_rect(macro_tile, out_bounds);
        if (is_rect_empty(macro_tile))
          continue;
        cv::Rect touched = macro_tile & entry.roi_hp;
        if (is_rect_empty(touched))
          continue;

        // If ROI covers the entire macro tile, process it as one big tile
        if (touched == macro_tile && macro_tile.width >= kHpMacroTileSize &&
            macro_tile.height >= kHpMacroTileSize) {
          task.output_tile.roi = macro_tile;
          task.input_tiles.clear();
          for (const auto* in_out : image_inputs_ready) {
            Tile in_tile;
            in_tile.buffer = const_cast<ImageBuffer*>(&in_out->image_buffer);
            in_tile.roi = clip_rect(expand_rect(macro_tile, entry.halo_hp),
                                    cv::Size(in_out->image_buffer.width,
                                             in_out->image_buffer.height));
            task.input_tiles.push_back(in_tile);
          }
          execute_tile_task(task, *hp_tile_fn);
          continue;
        }

        // Otherwise, fall back to micro tiles for the intersected region
        cv::Rect micro_cover = align_rect(touched, kHpMicroTileSize);
        micro_cover = clip_rect(micro_cover, out_bounds) & macro_tile;
        for (int my = micro_cover.y; my < micro_cover.y + micro_cover.height;
             my += kHpMicroTileSize) {
          for (int mx = micro_cover.x; mx < micro_cover.x + micro_cover.width;
               mx += kHpMicroTileSize) {
            cv::Rect micro_tile(
                mx, my,
                std::min(kHpMicroTileSize,
                         micro_cover.x + micro_cover.width - mx),
                std::min(kHpMicroTileSize,
                         micro_cover.y + micro_cover.height - my));
            micro_tile = clip_rect(micro_tile, out_bounds);
            if (is_rect_empty(micro_tile))
              continue;
            task.output_tile.roi = micro_tile;
            task.input_tiles.clear();
            for (const auto* in_out : image_inputs_ready) {
              Tile in_tile;
              in_tile.buffer = const_cast<ImageBuffer*>(&in_out->image_buffer);
              in_tile.roi = clip_rect(expand_rect(micro_tile, entry.halo_hp),
                                      cv::Size(in_out->image_buffer.width,
                                               in_out->image_buffer.height));
              task.input_tiles.push_back(in_tile);
            }
            execute_tile_task(task, *hp_tile_fn);
          }
        }
      }
    }

    // Update node state
    node.hp_roi =
        node.hp_roi.has_value()
            ? clip_rect(merge_rect(*node.hp_roi, entry.roi_hp), entry.hp_size)
            : entry.roi_hp;
    node.hp_version++;
    events_.push(node.id, node.name, "hp_update", 0.0);

    if (runtime_ptr) {
      downsample_requests.push_back({node.id, entry.roi_hp, node.hp_version});
    }
  };

  for (int nid : execution_order) {
    if (plan.count(nid)) {
      compute_node_hp(nid, plan.at(nid));
    }
  }

  if (!downsample_requests.empty()) {
    auto make_downsample_task = [this,
                                 &graph](DownsampleRequest request) -> Task {
      return [this, &graph, request]() {
        auto dtype_bytes = [](DataType dt) -> size_t {
          switch (dt) {
            case DataType::UINT8:
              return sizeof(uint8_t);
            case DataType::INT8:
              return sizeof(int8_t);
            case DataType::UINT16:
              return sizeof(uint16_t);
            case DataType::INT16:
              return sizeof(int16_t);
            case DataType::FLOAT64:
              return sizeof(double);
            case DataType::FLOAT32:
            default:
              return sizeof(float);
          }
        };

        std::unique_lock<std::mutex> lock(graph.graph_mutex_);
        auto node_it = graph.nodes.find(request.node_id);
        if (node_it == graph.nodes.end()) {
          return;
        }
        Node& node = node_it->second;
        if (!node.cached_output_high_precision) {
          return;
        }
        if (node.hp_version < request.hp_version) {
          return;
        }
        if (node.rt_version > request.hp_version) {
          return;
        }

        const NodeOutput& hp_output = *node.cached_output_high_precision;
        const ImageBuffer& hp_buffer = hp_output.image_buffer;
        cv::Size hp_size(std::max(hp_buffer.width, 0),
                         std::max(hp_buffer.height, 0));
        cv::Rect roi_hp = clip_rect(request.roi_hp, hp_size);
        if (is_rect_empty(roi_hp) && hp_size.width > 0 && hp_size.height > 0) {
          roi_hp = cv::Rect(0, 0, hp_size.width, hp_size.height);
        }

        if (!node.cached_output_real_time) {
          node.cached_output_real_time = NodeOutput{};
        }
        node.cached_output_real_time->data = hp_output.data;

        if (hp_buffer.width <= 0 || hp_buffer.height <= 0 || !hp_buffer.data) {
          node.cached_output_real_time = node.cached_output_high_precision;
          if (!is_rect_empty(roi_hp)) {
            node.rt_roi =
                node.rt_roi.has_value()
                    ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                    : roi_hp;
          }
          node.rt_version = request.hp_version;
          events_.push(node.id, node.name, "downsample_passthrough", 0.0);
          return;
        }

        cv::Size rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
        if (rt_size.width <= 0 || rt_size.height <= 0) {
          node.cached_output_real_time = node.cached_output_high_precision;
          if (!is_rect_empty(roi_hp)) {
            node.rt_roi =
                node.rt_roi.has_value()
                    ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                    : roi_hp;
          }
          node.rt_version = request.hp_version;
          events_.push(node.id, node.name, "downsample_passthrough", 0.0);
          return;
        }

        ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
        bool needs_alloc = (rt_buffer.width != rt_size.width) ||
                           (rt_buffer.height != rt_size.height) ||
                           (rt_buffer.channels != hp_buffer.channels) ||
                           (rt_buffer.type != hp_buffer.type) ||
                           (!rt_buffer.data);
        if (needs_alloc) {
          size_t pixel_size = dtype_bytes(hp_buffer.type);
          if (pixel_size == 0) {
            pixel_size = sizeof(float);
          }
          rt_buffer.width = rt_size.width;
          rt_buffer.height = rt_size.height;
          rt_buffer.channels = hp_buffer.channels;
          rt_buffer.type = hp_buffer.type;
          rt_buffer.device = Device::CPU;
          rt_buffer.context.reset();
          rt_buffer.step = static_cast<size_t>(rt_buffer.width) *
                           rt_buffer.channels * pixel_size;
          rt_buffer.data.reset(new char[rt_buffer.step * rt_buffer.height],
                               std::default_delete<char[]>());
          std::memset(rt_buffer.data.get(), 0,
                      rt_buffer.step * rt_buffer.height);
        }

        cv::Rect roi_rt =
            clip_rect(scale_down_rect(roi_hp, kRtDownscaleFactor), rt_size);
        if (is_rect_empty(roi_rt)) {
          roi_rt = cv::Rect(0, 0, rt_size.width, rt_size.height);
        }

        cv::Mat hp_mat = toCvMat(hp_buffer);
        cv::Mat rt_mat = toCvMat(rt_buffer);
        cv::Mat hp_patch = hp_mat(roi_hp);
        cv::Mat downsampled;
        cv::resize(hp_patch, downsampled, cv::Size(roi_rt.width, roi_rt.height),
                   0, 0, cv::INTER_LINEAR);
        downsampled.copyTo(rt_mat(roi_rt));

        node.rt_roi = node.rt_roi.has_value()
                          ? clip_rect(merge_rect(*node.rt_roi, roi_hp), hp_size)
                          : roi_hp;
        node.rt_version = request.hp_version;
        events_.push(node.id, node.name, "downsample", 0.0);
      };
    };

    for (const auto& request : downsample_requests) {
      Task task = make_downsample_task(request);
      if (runtime_ptr) {
        runtime_ptr->submit_ready_task_any_thread(std::move(task),
                                                  TaskPriority::High);
      } else {
        task();
      }
    }
  }

  Node& target = nodes.at(node_id);
  if (!target.cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP compute finished without target output.");
  }
  return *target.cached_output_high_precision;
}

NodeOutput& ComputeService::compute_real_time_update(
    GraphModel& graph, GraphRuntime* runtime, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    const cv::Rect& dirty_roi) {
  [[maybe_unused]] std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);
  auto& nodes = graph.nodes;

  (void)runtime;
  (void)cache_precision;
  (void)disable_disk_cache;
  (void)benchmark_events;
  (void)enable_timing;

  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound, "Cannot compute RT update: node " +
                                              std::to_string(node_id) +
                                              " not found.");
  }

  if (dirty_roi.width <= 0 || dirty_roi.height <= 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Cannot compute RT update: dirty ROI is empty.");
  }

  auto execution_order = traversal_.topo_postorder_from(graph, node_id);
  if (execution_order.empty()) {
    execution_order.push_back(node_id);
  }

  std::unordered_map<int, cv::Size> hp_size_cache;
  std::function<cv::Size(int)> infer_hp_size = [&](int nid) -> cv::Size {
    auto cached = hp_size_cache.find(nid);
    if (cached != hp_size_cache.end())
      return cached->second;

    cv::Size size{0, 0};
    const Node& node = nodes.at(nid);

    auto take_from_output = [&](const std::optional<NodeOutput>& opt) -> bool {
      if (!opt.has_value())
        return false;
      const auto& img = opt->image_buffer;
      if (img.width <= 0 || img.height <= 0)
        return false;
      size = cv::Size(img.width, img.height);
      return true;
    };

    if (take_from_output(node.cached_output_high_precision)) {
      hp_size_cache[nid] = size;
      return size;
    }
    if (take_from_output(node.cached_output)) {
      hp_size_cache[nid] = size;
      return size;
    }
    if (node.cached_output_real_time) {
      const auto& img = node.cached_output_real_time->image_buffer;
      if (img.width > 0 && img.height > 0) {
        size = cv::Size(img.width * kRtDownscaleFactor,
                        img.height * kRtDownscaleFactor);
        hp_size_cache[nid] = size;
        return size;
      }
    }

    for (const auto& input : node.image_inputs) {
      if (input.from_node_id < 0)
        continue;
      cv::Size parent_size = infer_hp_size(input.from_node_id);
      if (parent_size.width > 0 && parent_size.height > 0) {
        size = parent_size;
        break;
      }
    }

    if (size.width <= 0 || size.height <= 0) {
      int width = as_int_flexible(node.parameters, "width", 0);
      int height = as_int_flexible(node.parameters, "height", 0);
      if (width > 0 && height > 0) {
        size = cv::Size(width, height);
      }
    }

    hp_size_cache[nid] = size;
    return size;
  };

  auto infer_halo_hp = [&](const Node& node) -> int {
    if (node.type != "image_process")
      return 0;
    if (node.subtype == "gaussian_blur" ||
        node.subtype == "gaussian_blur_tiled") {
      int k = as_int_flexible(node.runtime_parameters, "ksize",
                              as_int_flexible(node.parameters, "ksize", 0));
      if (k <= 0)
        k = 3;
      if (k % 2 == 0)
        ++k;
      return std::max(0, k / 2);
    }
    if (node.subtype == "convolve") {
      int radius =
          as_int_flexible(node.runtime_parameters, "kernel_radius",
                          as_int_flexible(node.parameters, "kernel_radius", 0));
      radius = std::max(
          radius, as_int_flexible(node.runtime_parameters, "radius", radius));
      radius =
          std::max(radius, as_int_flexible(node.parameters, "radius", radius));
      int ksize =
          as_int_flexible(node.runtime_parameters, "kernel_size",
                          as_int_flexible(node.parameters, "kernel_size", 0));
      if (ksize <= 0) {
        ksize = as_int_flexible(node.runtime_parameters, "ksize",
                                as_int_flexible(node.parameters, "ksize", 0));
      }
      if (ksize > 0) {
        radius = std::max(radius, std::max(0, (ksize - 1) / 2));
      }
      if (radius <= 0)
        radius = 1;
      return radius;
    }
    return 0;
  };

  std::unordered_map<int, RtPlanEntry> plan;
  auto ensure_entry = [&](int nid) -> RtPlanEntry& {
    auto [it, inserted] = plan.emplace(nid, RtPlanEntry{});
    if (inserted) {
      it->second.hp_size = infer_hp_size(nid);
      it->second.rt_size =
          scale_down_size(it->second.hp_size, kRtDownscaleFactor);
      it->second.halo_hp = infer_halo_hp(nodes.at(nid));
      it->second.halo_rt =
          (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
    } else {
      if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0) {
        it->second.hp_size = infer_hp_size(nid);
        it->second.rt_size =
            scale_down_size(it->second.hp_size, kRtDownscaleFactor);
      }
      if (it->second.halo_hp == 0) {
        it->second.halo_hp = infer_halo_hp(nodes.at(nid));
        it->second.halo_rt =
            (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
      }
    }
    return it->second;
  };

  RtPlanEntry& target_entry = ensure_entry(node_id);
  target_entry.roi_hp =
      clip_rect(align_rect(dirty_roi, kHpAlignment), target_entry.hp_size);
  if (is_rect_empty(target_entry.roi_hp)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI does not intersect node output.");
  }
  target_entry.roi_rt = clip_rect(
      align_rect(scale_down_rect(target_entry.roi_hp, kRtDownscaleFactor),
                 kRtTileSize),
      target_entry.rt_size);
  if (is_rect_empty(target_entry.roi_rt)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dirty ROI collapses after RT scaling.");
  }

  for (auto it = execution_order.rbegin(); it != execution_order.rend(); ++it) {
    int current_id = *it;
    auto plan_it = plan.find(current_id);
    if (plan_it == plan.end())
      continue;

    RtPlanEntry& current_entry = plan_it->second;
    if (is_rect_empty(current_entry.roi_hp))
      continue;

    const Node& current_node = nodes.at(current_id);
    current_entry.halo_hp =
        std::max(current_entry.halo_hp, infer_halo_hp(current_node));
    current_entry.halo_rt =
        (current_entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;

    auto propagate_fn = OpRegistry::instance().get_dirty_propagator(
        current_node.type, current_node.subtype);
    cv::Rect upstream_roi_hp = propagate_fn(current_node, current_entry.roi_hp);
    upstream_roi_hp = clip_rect(upstream_roi_hp, current_entry.hp_size);
    if (is_rect_empty(upstream_roi_hp)) {
      continue;
    }

    for (const auto& img_input : current_node.image_inputs) {
      if (img_input.from_node_id < 0)
        continue;
      int parent_id = img_input.from_node_id;
      RtPlanEntry& parent_entry = ensure_entry(parent_id);
      cv::Rect parent_roi = clip_rect(align_rect(upstream_roi_hp, kHpAlignment),
                                      parent_entry.hp_size);
      if (is_rect_empty(parent_roi))
        continue;
      parent_entry.roi_hp =
          is_rect_empty(parent_entry.roi_hp)
              ? parent_roi
              : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi),
                          parent_entry.hp_size);
    }
  }

  std::vector<int> erase_ids;
  erase_ids.reserve(plan.size());
  for (auto& kv : plan) {
    auto& entry = kv.second;
    if (entry.hp_size.width <= 0 || entry.hp_size.height <= 0) {
      erase_ids.push_back(kv.first);
      continue;
    }
    entry.roi_hp =
        clip_rect(align_rect(entry.roi_hp, kHpAlignment), entry.hp_size);
    if (is_rect_empty(entry.roi_hp)) {
      erase_ids.push_back(kv.first);
      continue;
    }
    entry.rt_size = scale_down_size(entry.hp_size, kRtDownscaleFactor);
    entry.roi_rt =
        clip_rect(align_rect(scale_down_rect(entry.roi_hp, kRtDownscaleFactor),
                             kRtTileSize),
                  entry.rt_size);
    if (is_rect_empty(entry.roi_rt)) {
      erase_ids.push_back(kv.first);
      continue;
    }
    if (entry.halo_hp == 0) {
      entry.halo_hp = infer_halo_hp(nodes.at(kv.first));
      entry.halo_rt =
          (entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
    }
  }
  for (int nid : erase_ids) {
    plan.erase(nid);
  }

  if (plan.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "RT planner produced empty execution set.");
  }

  if (force_recache) {
    for (const auto& kv : plan) {
      Node& node = nodes.at(kv.first);
      node.cached_output_real_time.reset();
      node.rt_roi.reset();
      node.rt_version = 0;
    }
  }

  auto compute_node_rt = [&](int nid, RtPlanEntry& entry) {
    Node& node = nodes.at(nid);
    if (is_rect_empty(entry.roi_rt))
      return;

    YAML::Node runtime_params = node.parameters
                                    ? YAML::Clone(node.parameters)
                                    : YAML::Node(YAML::NodeType::Map);
    for (const auto& p_input : node.parameter_inputs) {
      if (p_input.from_node_id < 0)
        continue;
      const Node& parent_node = nodes.at(p_input.from_node_id);
      const NodeOutput* parent_out = nullptr;
      if (parent_node.cached_output_real_time) {
        parent_out = &*parent_node.cached_output_real_time;
      } else if (parent_node.cached_output_high_precision) {
        parent_out = &*parent_node.cached_output_high_precision;
      } else if (parent_node.cached_output) {
        parent_out = &*parent_node.cached_output;
      }
      if (!parent_out) {
        throw GraphError(
            GraphErrc::MissingDependency,
            "RT parameter input not ready for node " + std::to_string(nid));
      }
      auto it_val = parent_out->data.find(p_input.from_output_name);
      if (it_val == parent_out->data.end()) {
        throw GraphError(GraphErrc::MissingDependency,
                         "RT parameter '" + p_input.from_output_name +
                             "' missing for node " + std::to_string(nid));
      }
      runtime_params[p_input.to_parameter_name] = it_val->second;
    }
    node.runtime_parameters = runtime_params;

    std::vector<const NodeOutput*> image_inputs_ready;
    image_inputs_ready.reserve(node.image_inputs.size());
    for (const auto& img_input : node.image_inputs) {
      if (img_input.from_node_id < 0)
        continue;
      Node& parent_node = nodes.at(img_input.from_node_id);
      const NodeOutput* parent_out = nullptr;
      if (parent_node.cached_output_real_time) {
        parent_out = &*parent_node.cached_output_real_time;
      } else if (parent_node.cached_output_high_precision) {
        parent_out = &*parent_node.cached_output_high_precision;
      } else if (parent_node.cached_output) {
        parent_out = &*parent_node.cached_output;
      }
      if (!parent_out) {
        throw GraphError(
            GraphErrc::MissingDependency,
            "RT image input not ready for node " + std::to_string(nid));
      }
      image_inputs_ready.push_back(parent_out);
    }

    auto op_variant = OpRegistry::instance().resolve_for_intent(
        node.type, node.subtype, ComputeIntent::RealTimeUpdate);
    if (!op_variant) {
      op_variant = OpRegistry::instance().resolve_for_intent(
          node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
    }
    if (!op_variant) {
      throw GraphError(
          GraphErrc::NoOperation,
          "No operator registered for node " + node.type + ":" + node.subtype);
    }

    auto infer_output_spec = [&]() -> std::pair<int, DataType> {
      if (node.cached_output_real_time) {
        const auto& buf = node.cached_output_real_time->image_buffer;
        if (buf.width > 0 && buf.height > 0 && buf.channels > 0) {
          return {buf.channels, buf.type};
        }
      }
      for (const auto* input_out : image_inputs_ready) {
        const auto& buf = input_out->image_buffer;
        if (buf.width > 0 && buf.height > 0 && buf.channels > 0) {
          return {buf.channels, buf.type};
        }
      }
      if (node.cached_output_high_precision) {
        const auto& buf = node.cached_output_high_precision->image_buffer;
        if (buf.width > 0 && buf.height > 0 && buf.channels > 0) {
          return {buf.channels, buf.type};
        }
      }
      return {1, DataType::FLOAT32};
    };

    auto [channels, dtype] = infer_output_spec();
    if (!node.cached_output_real_time) {
      node.cached_output_real_time = NodeOutput{};
    }
    ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
    bool needs_alloc = (rt_buffer.width != entry.rt_size.width) ||
                       (rt_buffer.height != entry.rt_size.height) ||
                       (rt_buffer.channels != channels) ||
                       (rt_buffer.type != dtype) || (!rt_buffer.data);
    if (needs_alloc) {
      rt_buffer.width = entry.rt_size.width;
      rt_buffer.height = entry.rt_size.height;
      rt_buffer.channels = channels;
      rt_buffer.type = dtype;
      rt_buffer.device = Device::CPU;
      size_t pixel_size = sizeof(float);
      size_t row_bytes =
          static_cast<size_t>(rt_buffer.width) * channels * pixel_size;
      rt_buffer.step = row_bytes;
      rt_buffer.data.reset(new char[row_bytes * rt_buffer.height],
                           std::default_delete<char[]>());
      std::memset(rt_buffer.data.get(), 0, row_bytes * rt_buffer.height);
    }

    try {
      std::visit(
          [&](auto&& fn) {
            using T = std::decay_t<decltype(fn)>;
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
              NodeOutput result = fn(node, image_inputs_ready);
              if (result.image_buffer.width > 0 &&
                  result.image_buffer.height > 0) {
                cv::Mat result_mat = toCvMat(result.image_buffer);
                if (result_mat.cols != entry.rt_size.width ||
                    result_mat.rows != entry.rt_size.height) {
                  cv::resize(
                      result_mat, result_mat,
                      cv::Size(entry.rt_size.width, entry.rt_size.height), 0, 0,
                      cv::INTER_LINEAR);
                }
                cv::Mat dest = toCvMat(rt_buffer);
                result_mat(entry.roi_rt).copyTo(dest(entry.roi_rt));
              }
              node.cached_output_real_time->data = result.data;
            } else if constexpr (std::is_same_v<T, TileOpFunc>) {
              if (image_inputs_ready.empty()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "RT tiled op requires image inputs for node " +
                                     std::to_string(nid));
              }
              TileTask task;
              task.node = &node;
              task.output_tile.buffer = &rt_buffer;
              const cv::Size out_bounds(rt_buffer.width, rt_buffer.height);
              const int halo_rt = entry.halo_rt;
              for (int y = entry.roi_rt.y;
                   y < entry.roi_rt.y + entry.roi_rt.height; y += kRtTileSize) {
                for (int x = entry.roi_rt.x;
                     x < entry.roi_rt.x + entry.roi_rt.width;
                     x += kRtTileSize) {
                  int tile_w = std::min(
                      kRtTileSize, entry.roi_rt.x + entry.roi_rt.width - x);
                  int tile_h = std::min(
                      kRtTileSize, entry.roi_rt.y + entry.roi_rt.height - y);
                  cv::Rect tile_roi(x, y, tile_w, tile_h);
                  tile_roi = clip_rect(tile_roi, out_bounds);
                  if (is_rect_empty(tile_roi))
                    continue;
                  task.output_tile.roi = tile_roi;
                  task.input_tiles.clear();
                  for (const NodeOutput* input_out : image_inputs_ready) {
                    Tile input_tile;
                    input_tile.buffer =
                        const_cast<ImageBuffer*>(&input_out->image_buffer);
                    cv::Rect input_roi = tile_roi;
                    if (halo_rt > 0) {
                      input_roi = expand_rect(input_roi, halo_rt);
                    }
                    input_roi = clip_rect(
                        input_roi, cv::Size(input_out->image_buffer.width,
                                            input_out->image_buffer.height));
                    if (is_rect_empty(input_roi)) {
                      input_roi = clip_rect(
                          tile_roi, cv::Size(input_out->image_buffer.width,
                                             input_out->image_buffer.height));
                    }
                    input_tile.roi = input_roi;
                    task.input_tiles.push_back(input_tile);
                  }
                  execute_tile_task(task, fn);
                }
              }
            }
          },
          *op_variant);
    } catch (const cv::Exception& e) {
      throw GraphError(GraphErrc::ComputeError, "RT compute failed at node " +
                                                    std::to_string(nid) + ": " +
                                                    std::string(e.what()));
    } catch (const GraphError&) {
      throw;
    } catch (const std::exception& e) {
      throw GraphError(GraphErrc::ComputeError, "RT compute failed at node " +
                                                    std::to_string(nid) + ": " +
                                                    std::string(e.what()));
    }

    if (node.rt_roi.has_value()) {
      node.rt_roi =
          clip_rect(merge_rect(*node.rt_roi, entry.roi_hp), entry.hp_size);
    } else {
      node.rt_roi = entry.roi_hp;
    }
    node.rt_version++;
    events_.push(node.id, node.name, "rt_update", 0.0);
  };

  for (int nid : execution_order) {
    auto it = plan.find(nid);
    if (it == plan.end())
      continue;
    compute_node_rt(nid, it->second);
  }

  Node& target = nodes.at(node_id);
  if (!target.cached_output_real_time) {
    throw GraphError(GraphErrc::ComputeError,
                     "RT compute finished without target output.");
  }
  return *target.cached_output_real_time;
}

NodeOutput& ComputeService::compute_parallel(
    GraphModel& graph, GraphRuntime& runtime, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events) {
  auto& nodes = graph.nodes;
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;
  auto& graph_mutex = graph.graph_mutex_;
  auto& total_io_time_ms = graph.total_io_time_ms;
  // [健壮性修复] 将整个设置过程包裹在 try...catch 中
  try {
    if (!graph.has_node(node_id)) {
      throw GraphError(
          GraphErrc::NotFound,
          "Cannot compute: node " + std::to_string(node_id) + " not found.");
    }

    if (enable_timing) {
      clear_timing_results(graph);
      total_io_time_ms = 0.0;
    }

    auto execution_order = traversal_.topo_postorder_from(graph, node_id);
    std::unordered_set<int> execution_set(execution_order.begin(),
                                          execution_order.end());

    if (force_recache) {
      std::scoped_lock lock(graph_mutex);
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
      resolved_ops[i] = OpRegistry::instance().resolve_for_intent(
          n.type, n.subtype, ComputeIntent::GlobalHighPrecision);
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
          dependents_map[dep_idx].push_back(i);  // i 是当前节点的稠密索引
          num_deps++;
        }
      };

      for (const auto& input : node.image_inputs)
        count_dep(input.from_node_id);
      for (const auto& input : node.parameter_inputs)
        count_dep(input.from_node_id);

      dependency_counters[i] = num_deps;
    }

    // --- 为每个节点（按稠密索引）创建任务 ---
    for (size_t i = 0; i < num_nodes; ++i) {
      int current_node_id = execution_order[i];
      int current_node_idx = i;

      auto inner_task = [this, &graph, &nodes, &timing_mutex, &timing_results,
                         &runtime, &dependency_counters, &dependents_map,
                         &execution_order, &all_tasks, &id_to_idx,
                         &temp_results, &resolved_ops, &resolved_meta,
                         &graph_mutex, &total_io_time_ms, current_node_id,
                         current_node_idx, cache_precision, enable_timing,
                         disable_disk_cache, benchmark_events,
                         force_recache]() {
        // 1) Pure compute into temp_results without mutating GraphModel
        try {
          const Node& target_node = nodes.at(current_node_id);
          bool allow_disk_cache = (!disable_disk_cache) && (!force_recache);

          // Ensure upstream writes visible
          std::atomic_thread_fence(std::memory_order_acquire);

          auto get_upstream_output = [&](int up_id) -> const NodeOutput* {
            if (up_id < 0)
              return nullptr;
            auto itn = nodes.find(up_id);
            if (itn == nodes.end())
              return nullptr;
            auto it_idx = id_to_idx.find(up_id);
            if (it_idx != id_to_idx.end()) {
              int up_idx = it_idx->second;
              if (temp_results[up_idx].has_value())
                return &*temp_results[up_idx];
            }
            if (itn->second.cached_output.has_value())
              return &*itn->second.cached_output;
            return nullptr;
          };

          // Memory or disk fast path
          if (!target_node.cached_output.has_value() && allow_disk_cache &&
              !temp_results[current_node_idx].has_value()) {
            NodeOutput from_disk;
            if (cache_.try_load_from_disk_cache_into(graph, target_node,
                                                     from_disk)) {
              temp_results[current_node_idx] = std::move(from_disk);
              if (enable_timing) {
                // Log a zero-cost event indicating disk cache hit (IO time
                // tracked separately)
                BenchmarkEvent ev;
                ev.node_id = current_node_id;
                ev.op_name = make_key(target_node.type, target_node.subtype);
                ev.dependency_start_time =
                    std::chrono::high_resolution_clock::now();
                ev.execution_start_time = ev.dependency_start_time;
                ev.execution_end_time = ev.execution_start_time;
                ev.execution_duration_ms = 0.0;
                ev.source = "disk_cache";
                if (benchmark_events) {
                  benchmark_events->push_back(ev);
                }
                {
                  std::lock_guard lk(timing_mutex);
                  timing_results.node_timings.push_back(
                      {target_node.id, target_node.name, 0.0, "disk_cache"});
                }
                events_.push(target_node.id, target_node.name, "disk_cache",
                             0.0);
              } else {
                events_.push(target_node.id, target_node.name, "disk_cache",
                             0.0);
              }
            }
          }

          if (!target_node.cached_output.has_value() &&
              !temp_results[current_node_idx].has_value()) {
            YAML::Node runtime_params =
                target_node.parameters ? YAML::Clone(target_node.parameters)
                                       : YAML::Node(YAML::NodeType::Map);
            for (const auto& p_input : target_node.parameter_inputs) {
              if (p_input.from_node_id < 0)
                continue;
              auto const* up_out = get_upstream_output(p_input.from_node_id);
              if (!up_out) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "Parameter input not ready for node " +
                                     std::to_string(current_node_id));
              }
              auto it = up_out->data.find(p_input.from_output_name);
              if (it == up_out->data.end()) {
                throw GraphError(
                    GraphErrc::MissingDependency,
                    "Node " + std::to_string(p_input.from_node_id) +
                        " missing output '" + p_input.from_output_name + "'");
              }
              runtime_params[p_input.to_parameter_name] = it->second;
            }

            std::vector<const NodeOutput*> inputs_ready;
            inputs_ready.reserve(target_node.image_inputs.size());
            for (const auto& i_input : target_node.image_inputs) {
              if (i_input.from_node_id < 0)
                continue;
              auto const* up_out = get_upstream_output(i_input.from_node_id);
              if (!up_out) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "Image input not ready for node " +
                                     std::to_string(current_node_id));
              }
              inputs_ready.push_back(up_out);
            }

            const auto& op_opt = resolved_ops[current_node_idx];
            if (!op_opt.has_value()) {
              throw GraphError(
                  GraphErrc::NoOperation,
                  "No op for " + target_node.type + ":" + target_node.subtype);
            }

            BenchmarkEvent current_event;
            current_event.node_id = current_node_id;
            current_event.op_name =
                make_key(target_node.type, target_node.subtype);
            current_event.dependency_start_time =
                std::chrono::high_resolution_clock::now();
            current_event.execution_start_time =
                current_event.dependency_start_time;

            // Build execution node with resolved runtime parameters
            Node node_for_exec = target_node;
            node_for_exec.runtime_parameters = runtime_params;

            NodeOutput result;
            bool tiled_dispatched = false;
            try {
              std::visit(
                  [&](auto&& op_func) {
                    using T = std::decay_t<decltype(op_func)>;
                    if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                      result = op_func(node_for_exec, inputs_ready);
                    } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                      // Build normalized inputs that must outlive tile
                      // micro-tasks
                      std::vector<NodeOutput> normalized_storage_local;
                      std::vector<const NodeOutput*> inputs_for_tiling =
                          inputs_ready;
                      bool is_mixing = (target_node.type == "image_mixing");
                      if (is_mixing && inputs_ready.size() >= 2) {
                        const auto& base_buffer = inputs_ready[0]->image_buffer;
                        if (base_buffer.width == 0 || base_buffer.height == 0) {
                          throw GraphError(
                              GraphErrc::InvalidParameter,
                              "Base image for image_mixing is empty.");
                        }
                        const int base_w = base_buffer.width;
                        const int base_h = base_buffer.height;
                        const int base_c = base_buffer.channels;
                        const std::string strategy =
                            as_str(node_for_exec.runtime_parameters,
                                   "merge_strategy", "resize");
                        normalized_storage_local.reserve(inputs_ready.size() -
                                                         1);
                        for (size_t i = 1; i < inputs_ready.size(); ++i) {
                          const auto& current_buffer =
                              inputs_ready[i]->image_buffer;
                          if (current_buffer.width == 0 ||
                              current_buffer.height == 0) {
                            throw GraphError(
                                GraphErrc::InvalidParameter,
                                "Secondary image for image_mixing is empty.");
                          }
                          cv::Mat current_mat = toCvMat(current_buffer);
                          if (current_mat.cols != base_w ||
                              current_mat.rows != base_h) {
                            if (strategy == "resize") {
                              cv::resize(current_mat, current_mat,
                                         cv::Size(base_w, base_h), 0, 0,
                                         cv::INTER_LINEAR);
                            } else if (strategy == "crop") {
                              cv::Rect crop_roi(
                                  0, 0, std::min(current_mat.cols, base_w),
                                  std::min(current_mat.rows, base_h));
                              cv::Mat cropped = cv::Mat::zeros(
                                  base_h, base_w, current_mat.type());
                              current_mat(crop_roi).copyTo(cropped(crop_roi));
                              current_mat = cropped;
                            } else {
                              throw GraphError(GraphErrc::InvalidParameter,
                                               "Unsupported merge_strategy for "
                                               "tiled mixing.");
                            }
                          }
                          if (current_mat.channels() != base_c) {
                            if (current_mat.channels() == 1 &&
                                (base_c == 3 || base_c == 4)) {
                              std::vector<cv::Mat> planes(base_c, current_mat);
                              cv::merge(planes, current_mat);
                            } else if ((current_mat.channels() == 3 ||
                                        current_mat.channels() == 4) &&
                                       base_c == 1) {
                              cv::cvtColor(current_mat, current_mat,
                                           cv::COLOR_BGR2GRAY);
                            } else if (current_mat.channels() == 4 &&
                                       base_c == 3) {
                              cv::cvtColor(current_mat, current_mat,
                                           cv::COLOR_BGRA2BGR);
                            } else if (current_mat.channels() == 3 &&
                                       base_c == 4) {
                              cv::cvtColor(current_mat, current_mat,
                                           cv::COLOR_BGR2BGRA);
                            } else {
                              throw GraphError(GraphErrc::InvalidParameter,
                                               "Unsupported channel conversion "
                                               "in tiled mixing.");
                            }
                          }
                          NodeOutput tmp;
                          tmp.image_buffer = fromCvMat(current_mat);
                          normalized_storage_local.push_back(std::move(tmp));
                          inputs_for_tiling[i] =
                              &normalized_storage_local.back();
                        }
                      }

                      // Prepare shared store for normalized inputs to extend
                      // lifetime
                      auto norm_store_sp =
                          std::make_shared<std::vector<NodeOutput>>(
                              std::move(normalized_storage_local));
                      // Build input_ptrs that reference shared store for
                      // secondary images
                      std::vector<const NodeOutput*> input_ptrs = inputs_ready;
                      if (is_mixing && inputs_ready.size() >= 2) {
                        for (size_t i = 1, k = 0; i < inputs_ready.size();
                             ++i, ++k) {
                          if (k < norm_store_sp->size())
                            input_ptrs[i] = &(*norm_store_sp)[k];
                        }
                      }

                      // Infer output shape
                      int out_w = input_ptrs.empty()
                                      ? as_int_flexible(
                                            node_for_exec.runtime_parameters,
                                            "width", 256)
                                      : input_ptrs[0]->image_buffer.width;
                      int out_h = input_ptrs.empty()
                                      ? as_int_flexible(
                                            node_for_exec.runtime_parameters,
                                            "height", 256)
                                      : input_ptrs[0]->image_buffer.height;
                      int out_c = input_ptrs.empty()
                                      ? 1
                                      : input_ptrs[0]->image_buffer.channels;
                      auto out_t = input_ptrs.empty()
                                       ? ps::DataType::FLOAT32
                                       : input_ptrs[0]->image_buffer.type;

                      // Allocate output buffer in temp_results (visible for
                      // tiles)
                      temp_results[current_node_idx] = NodeOutput{};
                      auto& ob = temp_results[current_node_idx]->image_buffer;
                      ob.width = out_w;
                      ob.height = out_h;
                      ob.channels = out_c;
                      ob.type = out_t;
                      ob.step =
                          static_cast<size_t>(out_w) * out_c * sizeof(float);
                      ob.data.reset(new char[ob.step * ob.height],
                                    std::default_delete<char[]>());

                      // Tile size from metadata preference
                      int tile_size = 128;
                      if (resolved_meta[current_node_idx].has_value()) {
                        auto pref =
                            resolved_meta[current_node_idx]->tile_preference;
                        if (pref == TileSizePreference::MICRO)
                          tile_size = 16;
                        else if (pref == TileSizePreference::MACRO)
                          tile_size = 256;
                      }
                      const bool needs_halo =
                          (node_for_exec.type == "image_process" &&
                           node_for_exec.subtype.find("gaussian_blur") !=
                               std::string::npos);
                      const int HALO_SIZE = 16;

                      // Plan tiles and spawn micro tasks
                      int tiles_x = (out_w + tile_size - 1) / tile_size;
                      int tiles_y = (out_h + tile_size - 1) / tile_size;
                      int total_tiles = tiles_x * tiles_y;
                      runtime.inc_graph_tasks_to_complete(total_tiles);
                      auto remaining =
                          std::make_shared<std::atomic<int>>(total_tiles);
                      auto start_tp = std::make_shared<
                          std::chrono::high_resolution_clock::time_point>(
                          std::chrono::high_resolution_clock::now());

                      for (int ty = 0; ty < tiles_y; ++ty) {
                        for (int tx = 0; tx < tiles_x; ++tx) {
                          int x = tx * tile_size;
                          int y = ty * tile_size;
                          int w = std::min(tile_size, out_w - x);
                          int h = std::min(tile_size, out_h - y);
                          Task tile_task = [this, &runtime, &dependents_map,
                                            &dependency_counters, &all_tasks,
                                            &temp_results, &nodes,
                                            &timing_mutex, &timing_results, x,
                                            y, w, h, needs_halo, HALO_SIZE,
                                            input_ptrs, norm_store_sp,
                                            remaining, start_tp,
                                            current_node_idx, current_node_id,
                                            node_for_exec, op_func,
                                            benchmark_events, enable_timing]() {
                            try {
                              TileTask tt;
                              tt.node = &node_for_exec;
                              tt.output_tile.buffer =
                                  &temp_results[current_node_idx]->image_buffer;
                              tt.output_tile.roi = cv::Rect(x, y, w, h);
                              for (auto const* in_out : input_ptrs) {
                                Tile in_tile;
                                in_tile.buffer = const_cast<ImageBuffer*>(
                                    &in_out->image_buffer);
                                in_tile.roi =
                                    needs_halo
                                        ? calculate_halo(
                                              tt.output_tile.roi, HALO_SIZE,
                                              {in_out->image_buffer.width,
                                               in_out->image_buffer.height})
                                        : tt.output_tile.roi;
                                tt.input_tiles.push_back(std::move(in_tile));
                              }
                              execute_tile_task(tt, op_func);
                              if (remaining->fetch_sub(
                                      1, std::memory_order_acq_rel) == 1) {
                                std::atomic_thread_fence(
                                    std::memory_order_release);
                                if (enable_timing) {
                                  auto end_tp =
                                      std::chrono::high_resolution_clock::now();
                                  double exec_ms =
                                      std::chrono::duration<double, std::milli>(
                                          end_tp - *start_tp)
                                          .count();
                                  BenchmarkEvent ev;
                                  ev.node_id = current_node_id;
                                  ev.op_name = make_key(node_for_exec.type,
                                                        node_for_exec.subtype);
                                  ev.execution_start_time = *start_tp;
                                  ev.dependency_start_time = *start_tp;
                                  ev.execution_end_time = end_tp;
                                  ev.execution_duration_ms = exec_ms;
                                  ev.source = "computed";
                                  if (benchmark_events) {
                                    std::lock_guard lk(timing_mutex);
                                    benchmark_events->push_back(ev);
                                  }
                                  {
                                    std::lock_guard lk(timing_mutex);
                                    timing_results.node_timings.push_back(
                                        {current_node_id,
                                         nodes.at(current_node_id).name,
                                         exec_ms, std::string("computed")});
                                  }
                                  events_.push(current_node_id,
                                               nodes.at(current_node_id).name,
                                               "computed", exec_ms);
                                } else {
                                  events_.push(current_node_id,
                                               nodes.at(current_node_id).name,
                                               "computed", 0.0);
                                }
                                for (int dependent_idx :
                                     dependents_map[current_node_idx]) {
                                  if (--dependency_counters[dependent_idx] ==
                                      0) {
                                    runtime.submit_ready_task_from_worker(
                                        std::move(all_tasks[dependent_idx]));
                                  }
                                }
                              }
                            } catch (const std::exception& e) {
                              runtime.set_exception(
                                  std::make_exception_ptr(GraphError(
                                      GraphErrc::ComputeError,
                                      std::string("Tile stage at node ") +
                                          std::to_string(current_node_id) +
                                          " (" +
                                          nodes.at(current_node_id).name +
                                          ") failed: " + e.what())));
                            } catch (...) {
                              runtime.set_exception(
                                  std::make_exception_ptr(GraphError(
                                      GraphErrc::ComputeError,
                                      std::string("Tile stage at node ") +
                                          std::to_string(current_node_id) +
                                          " (" +
                                          nodes.at(current_node_id).name +
                                          ") failed: unknown exception")));
                            }
                            runtime.dec_graph_tasks_to_complete();
                          };
                          runtime.submit_ready_task_from_worker(
                              std::move(tile_task));
                        }
                      }
                      tiled_dispatched = true;
                    }
                  },
                  *op_opt);
            } catch (const std::exception& e) {
              throw;
            }

            if (tiled_dispatched) {
              // Tiled micro-tasks handle timing and dependent scheduling; skip
              // monolithic finalization.
              return;
            }
            temp_results[current_node_idx] = std::move(result);

            if (enable_timing) {
              current_event.execution_end_time =
                  std::chrono::high_resolution_clock::now();
              double ms = std::chrono::duration<double, std::milli>(
                              current_event.execution_end_time -
                              current_event.execution_start_time)
                              .count();
              current_event.source = "computed";
              current_event.execution_duration_ms = ms;
              if (benchmark_events) {
                std::lock_guard lk(timing_mutex);
                benchmark_events->push_back(current_event);
              }
              {
                std::lock_guard lk(timing_mutex);
                timing_results.node_timings.push_back(
                    {target_node.id, target_node.name, ms, "computed"});
              }
              events_.push(target_node.id, target_node.name, "computed", ms);
            } else {
              events_.push(target_node.id, target_node.name, "computed", 0.0);
            }
          }
        } catch (const cv::Exception& e) {
          runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Compute stage at node " + std::to_string(current_node_id) +
                  " (" + nodes.at(current_node_id).name +
                  ") failed: " + std::string(e.what()))));
          return;
        } catch (const std::exception& e) {
          runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Compute stage at node " + std::to_string(current_node_id) +
                  " (" + nodes.at(current_node_id).name +
                  ") failed: " + e.what())));
          return;
        } catch (...) {
          runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Compute stage at node " + std::to_string(current_node_id) +
                  " (" + nodes.at(current_node_id).name +
                  ") failed: unknown exception")));
          return;
        }

        // 2) 触发后续依赖任务（捕获并精确标注阶段）
        try {
          // Ensure all writes to temp_results are visible before scheduling
          // dependents
          std::atomic_thread_fence(std::memory_order_release);
          for (int dependent_idx : dependents_map[current_node_idx]) {
            if (--dependency_counters[dependent_idx] == 0) {
              runtime.submit_ready_task_from_worker(
                  std::move(all_tasks[dependent_idx]));
            }
          }
        } catch (const std::out_of_range& e) {
          runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Scheduling stage after node " + std::to_string(current_node_id) +
                  " (" + nodes.at(current_node_id).name +
                  ") failed: out_of_range: " + std::string(e.what()))));
        } catch (const std::exception& e) {
          runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Scheduling stage after node " + std::to_string(current_node_id) +
                  " (" + nodes.at(current_node_id).name +
                  ") failed: " + e.what())));
        } catch (...) {
          runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Scheduling stage after node " + std::to_string(current_node_id) +
                  " (" + nodes.at(current_node_id).name +
                  ") failed: unknown exception")));
        }
      };

      // --- 包装任务以处理异常和完成计数 ---
      all_tasks[i] = [inner_task = std::move(inner_task), &runtime,
                      current_node_id]() {
        runtime.log_event(GraphRuntime::SchedulerEvent::EXECUTE,
                          current_node_id);
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

    if (execution_order.empty() && graph.has_node(node_id)) {
      // 处理图中只有一个节点的情况
      if (!nodes.at(node_id).cached_output.has_value()) {
        std::unordered_map<int, bool> visiting;
        compute_internal(graph, node_id, cache_precision, visiting,
                         enable_timing, !disable_disk_cache, benchmark_events);
      }
    } else {
      runtime.submit_initial_tasks(std::move(initial_tasks),
                                   execution_order.size());
      for (size_t i = 0; i < num_nodes; ++i) {
        if (dependency_counters[i] == 0) {
          runtime.log_event(GraphRuntime::SchedulerEvent::ASSIGN_INITIAL,
                            execution_order[i]);
        }
      }
      runtime.wait_for_completion();
    }

    // --- 后续处理（计时、结果返回） ---
    if (enable_timing) {
      double total = 0.0;
      {
        std::lock_guard lk(timing_mutex);
        for (const auto& timing : timing_results.node_timings) {
          total += timing.elapsed_ms;
        }
        timing_results.total_ms = total;
      }
    }

    // Commit results: write back to nodes and save caches
    {
      std::scoped_lock lock(graph_mutex);
      for (size_t i = 0; i < num_nodes; ++i) {
        if (temp_results[i].has_value()) {
          int nid = execution_order[i];
          nodes.at(nid).cached_output = std::move(*temp_results[i]);
          try {
            nodes.at(nid).cached_output_high_precision =
                *nodes.at(nid).cached_output;
            nodes.at(nid).hp_version++;
          } catch (...) {
            // Non-fatal; maintain backward compatibility
          }
          cache_.save_cache_if_configured(graph, nodes.at(nid),
                                          cache_precision);
        }
      }
    }
    if (!nodes.at(node_id).cached_output) {
      throw GraphError(GraphErrc::ComputeError,
                       "Parallel computation finished but target node has no "
                       "output. An upstream error likely occurred.");
    }
    return *nodes.at(node_id).cached_output;
  } catch (...) {
    // 捕获在本函数内（任务提交前）抛出的异常，并传递给运行时
    runtime.set_exception(std::current_exception());
    // 等待运行时处理异常并唤醒
    runtime.wait_for_completion();
    // wait_for_completion 内部会重新抛出异常，这里我们只需确保函数有返回值
    // 在实际情况下，由于 rethrow，代码不会执行到这里
    throw GraphError(
        GraphErrc::Unknown,
        "Caught pre-flight exception during parallel compute setup.");
  }
}

NodeOutput& ComputeService::compute_parallel(
    GraphModel& graph, GraphRuntime& runtime, ComputeIntent intent, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi) {
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      if (dirty_roi.has_value()) {
        // TODO: For now, a dirty ROI on a global compute triggers a full
        // recompute. In the future, this could be an optimized partial update.
        return compute_parallel(graph, runtime, node_id, cache_precision, true,
                                enable_timing, disable_disk_cache,
                                benchmark_events);
      }
      return compute_parallel(graph, runtime, node_id, cache_precision,
                              force_recache, enable_timing, disable_disk_cache,
                              benchmark_events);
    case ComputeIntent::RealTimeUpdate:
      if (!dirty_roi.has_value()) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "RealTimeUpdate intent requires a dirty ROI region.");
      }
      // 启动后台 HP 更新（目前是串行执行，但逻辑上是独立的）
      compute_high_precision_update(
          graph, &runtime, node_id, cache_precision, force_recache,
          enable_timing, disable_disk_cache, benchmark_events, *dirty_roi);

      // 接着执行并返回 RT 更新的结果
      return compute_real_time_update(
          graph, &runtime, node_id, cache_precision, force_recache,
          enable_timing, disable_disk_cache, benchmark_events, *dirty_roi);
    default:
      return compute_parallel(graph, runtime, node_id, cache_precision,
                              force_recache, enable_timing, disable_disk_cache,
                              benchmark_events);
  }
}

// Phase 1 overload: intent-based entry to sequential compute
NodeOutput& ComputeService::compute_with_intent_impl(
    GraphModel& graph, ComputeIntent intent, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi) {
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      return compute(graph, node_id, cache_precision, force_recache,
                     enable_timing, disable_disk_cache, benchmark_events);
    case ComputeIntent::RealTimeUpdate:
      if (!dirty_roi.has_value()) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "RealTimeUpdate intent requires a dirty ROI region.");
      }
      return compute_real_time_update(
          graph, nullptr, node_id, cache_precision, force_recache,
          enable_timing, disable_disk_cache, benchmark_events, *dirty_roi);
    default:
      return compute(graph, node_id, cache_precision, force_recache,
                     enable_timing, disable_disk_cache, benchmark_events);
  }
}

NodeOutput& ComputeService::compute(
    GraphModel& graph, int node_id, const std::string& cache_precision,
    bool force_recache, bool enable_timing, bool disable_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events) {
  return compute_sequential_impl(graph, node_id, cache_precision, force_recache,
                                 enable_timing, disable_disk_cache,
                                 benchmark_events);
}

NodeOutput& ComputeService::compute_sequential_impl(
    GraphModel& graph, int node_id, const std::string& cache_precision,
    bool force_recache, bool enable_timing, bool disable_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events) {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound, "Node " + std::to_string(node_id) +
                                              " not found in graph.");
  }

  if (benchmark_events) {
    benchmark_events->clear();
  }

  if (enable_timing) {
    clear_timing_results(graph);
  }

  std::vector<int> execution_order;
  try {
    execution_order = traversal_.topo_postorder_from(graph, node_id);
  } catch (const GraphError&) {
    throw;
  }

  if (force_recache) {
    std::lock_guard<std::mutex> lk(graph.graph_mutex_);
    for (int nid : execution_order) {
      auto& node = graph.nodes.at(nid);
      node.cached_output.reset();
      node.cached_output_high_precision.reset();
      node.cached_output_real_time.reset();
    }
  }

  std::unordered_map<int, bool> visiting;
  bool allow_disk_cache = !disable_disk_cache && !force_recache;
  NodeOutput& result =
      compute_internal(graph, node_id, cache_precision, visiting, enable_timing,
                       allow_disk_cache, benchmark_events);

  if (enable_timing) {
    double total = 0.0;
    {
      std::lock_guard<std::mutex> lk(graph.timing_mutex_);
      for (const auto& timing : graph.timing_results.node_timings) {
        total += timing.elapsed_ms;
      }
      graph.timing_results.total_ms = total;
    }
  }

  return result;
}

NodeOutput& ComputeService::compute(
    GraphModel& graph, ComputeIntent intent, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi) {
  return compute_with_intent_impl(
      graph, intent, node_id, cache_precision, force_recache, enable_timing,
      disable_disk_cache, benchmark_events, dirty_roi);
}

/**
 * @brief 清空节点图的计时结果
 *
 * 该函数用于清除存储在 timing_results 对象中的所有计时数据。
 * 它首先通过 std::lock_guard
 * 获取互斥锁以确保线程安全，然后清除节点计时记录并将总计时毫秒数重置为 0。
 */
void ComputeService::clear_timing_results(GraphModel& graph) {
  std::lock_guard<std::mutex> lk(graph.timing_mutex_);
  graph.timing_results.node_timings.clear();
  graph.timing_results.total_ms = 0.0;
}

}  // namespace ps
