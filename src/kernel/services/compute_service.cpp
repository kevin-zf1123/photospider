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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
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
#include "kernel/scheduler/scheduler_task_runtime.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/compute_task_planner.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/intent_update_coordinator.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/compute-service/node_input_resolver.hpp"
#include "kernel/services/compute-service/parallel_graph_executor.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps {

ComputeService::ComputeService(GraphTraversalService& traversal,
                               GraphCacheService& cache,
                               GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

using compute::align_rect;
using compute::calculate_halo;
using compute::clip_rect;
using compute::expand_rect;
using compute::is_rect_empty;
using compute::kHpAlignment;
using compute::kHpMacroTileSize;
using compute::kHpMicroTileSize;
using compute::kRtDownscaleFactor;
using compute::kRtTileSize;
using compute::merge_rect;
using compute::scale_down_rect;
using compute::scale_down_size;

namespace {

using RtPlanEntry = compute::RtPlanEntry;
using HpPlanEntry = compute::HpPlanEntry;

SchedulerTaskRuntime& scheduler_task_runtime_for(GraphRuntime& runtime,
                                                 ComputeIntent intent) {
  IScheduler* scheduler = runtime.get_scheduler(intent);
  if (!scheduler) {
    throw GraphError(GraphErrc::ComputeError,
                     "No scheduler registered for requested compute intent.");
  }
  auto* task_runtime = dynamic_cast<SchedulerTaskRuntime*>(scheduler);
  if (!task_runtime) {
    throw GraphError(GraphErrc::ComputeError,
                     "Registered scheduler cannot dispatch planned tasks.");
  }
  if (!task_runtime->task_runtime_running()) {
    scheduler->start();
  }
  return *task_runtime;
}

void finalize_output_metadata(NodeOutput& output,
                              const std::vector<const NodeOutput*>& inputs,
                              bool enable_timing, double execution_ms) {
  compute::ComputeMetricsRecorder::finalize_output_metadata(
      output, inputs, enable_timing, execution_ms);
}

void remember_compute_plan(GraphModel& graph,
                           const compute::ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.recent_compute_plans.push_back(compute_plan);
  if (graph.recent_compute_plans.size() > 16) {
    graph.recent_compute_plans.erase(graph.recent_compute_plans.begin());
  }
}

void remember_dirty_snapshot(GraphModel& graph,
                             const compute::DirtyRegionSnapshot& snapshot) {
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
}

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
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;
  auto& target_node = graph.mutable_node(node_id);
  std::string result_source = "unknown";

  // 将计时器移到更早的位置，以捕获依赖解析时间
  auto start_time_total = std::chrono::high_resolution_clock::now();

  // 创建并初始化 BenchmarkEvent
  BenchmarkEvent current_event;
  current_event.node_id = node_id;
  current_event.op_name = make_key(target_node.type, target_node.subtype);
  current_event.dependency_start_time = start_time_total;

  std::vector<const NodeOutput*> monolithic_inputs;
  do {
    // 1. 内存／磁盘缓存检测
    if (compute::ComputeCachePolicy::has_reusable_output(target_node)) {
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

    auto resolved_inputs = compute::NodeInputResolver::resolve(
        target_node,
        [&](int upstream_id) -> const NodeOutput* {
          return &compute_internal(graph, upstream_id, cache_precision,
                                   visiting, enable_timing, allow_disk_cache,
                                   benchmark_events);
        },
        "Sequential compute");
    monolithic_inputs = resolved_inputs.image_inputs;

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

    target_node.cached_output_high_precision = compute::NodeExecutor::execute(
        graph, target_node, *op_opt, monolithic_inputs);

    current_event.execution_end_time =
        std::chrono::high_resolution_clock::now();
    result_source = "computed";
    target_node.hp_version++;
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

  if (result_source == "computed" && target_node.cached_output_high_precision) {
    finalize_output_metadata(*target_node.cached_output_high_precision,
                             monolithic_inputs, enable_timing,
                             execution_duration_ms);
  }

  // 8. 全局与本地计时记录
  if (enable_timing) {
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

  return *compute::ComputeCachePolicy::reusable_output(target_node);
}

NodeOutput& ComputeService::compute_node_no_recurse(
    GraphModel& graph, int node_id, const std::string& cache_precision,
    bool enable_timing, bool allow_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events) {
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;

  auto& target_node = graph.mutable_node(node_id);
  // Fast path: already computed.
  if (compute::ComputeCachePolicy::has_reusable_output(target_node))
    return *compute::ComputeCachePolicy::reusable_output(target_node);

  // Optionally load from disk cache for this node itself
  if (allow_disk_cache) {
    (void)cache_.try_load_from_disk_cache(graph, target_node);
    if (compute::ComputeCachePolicy::has_reusable_output(target_node))
      return *compute::ComputeCachePolicy::reusable_output(target_node);
  }

  // Ensure visibility of upstream writes before reading their cached outputs
  std::atomic_thread_fence(std::memory_order_acquire);

  auto resolved_inputs = compute::NodeInputResolver::resolve(
      target_node,
      [&](int upstream_id) -> const NodeOutput* {
        const Node* upstream = graph.find_node(upstream_id);
        if (!upstream)
          return nullptr;
        return compute::ComputeCachePolicy::reusable_output(*upstream);
      },
      "Parallel scheduler bug");
  const auto& inputs_ready = resolved_inputs.image_inputs;

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

  target_node.cached_output_high_precision =
      compute::NodeExecutor::execute(graph, target_node, *op_opt, inputs_ready);
  target_node.hp_version++;

  // Save disk cache if configured
  cache_.save_cache_if_configured(graph, target_node, cache_precision);

  // Timing & events
  double exec_ms_for_meta = 0.0;
  if (enable_timing) {
    current_event.execution_end_time =
        std::chrono::high_resolution_clock::now();
    current_event.source = "computed";
    current_event.execution_duration_ms =
        std::chrono::duration<double, std::milli>(
            current_event.execution_end_time -
            current_event.execution_start_time)
            .count();
    exec_ms_for_meta = current_event.execution_duration_ms;
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
  if (target_node.cached_output_high_precision) {
    finalize_output_metadata(*target_node.cached_output_high_precision,
                             inputs_ready, enable_timing, exec_ms_for_meta);
  }
  return *target_node.cached_output_high_precision;
}

NodeOutput& ComputeService::compute_high_precision_update(
    GraphModel& graph, GraphRuntime* runtime, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    const cv::Rect& dirty_roi) {
  [[maybe_unused]] std::unique_lock<std::mutex> graph_lock(graph.graph_mutex_);

  (void)runtime;  // Unused for now, can be used for micro-task scheduling later
  (void)cache_precision;
  (void)disable_disk_cache;
  (void)benchmark_events;
  (void)enable_timing;

  RoiPropagationService roi_propagation;
  compute::DirtyRegionPlanner dirty_planner(traversal_, roi_propagation);
  auto dirty_plan =
      dirty_planner.plan_high_precision(graph, node_id, dirty_roi);
  graph.last_dirty_region_snapshot_debug =
      compute::DirtyRegionPlanner::describe_snapshot(dirty_plan.snapshot);
  remember_dirty_snapshot(graph, dirty_plan.snapshot);
  auto plan = dirty_plan.entries;
  auto execution_order = dirty_plan.execution_order;
  compute::ComputeTaskPlanner task_planner;
  const compute::ComputePlan compute_plan = task_planner.plan(
      {ComputeIntent::GlobalHighPrecision, node_id, false, dirty_roi},
      execution_order, &dirty_plan.snapshot, &graph);
  remember_compute_plan(graph, compute_plan);
  for (const auto& work : compute_plan.planned_work) {
    auto entry_it = plan.find(work.node_id);
    if (entry_it == plan.end())
      continue;
    if (!is_rect_empty(work.represented_hp_roi)) {
      entry_it->second.roi_hp =
          clip_rect(work.represented_hp_roi, entry_it->second.hp_size);
    }
  }

  struct DownsampleRequest {
    int node_id = -1;
    cv::Rect roi_hp;
    int hp_version = 0;
  };
  std::vector<DownsampleRequest> downsample_requests;
  GraphRuntime* runtime_ptr = runtime;
  if (force_recache) {
    for (const auto& [nid, _] : plan) {
      Node& node = graph.mutable_node(nid);
      node.cached_output_high_precision.reset();
      node.hp_roi.reset();
      node.hp_version = 0;
    }
  }

  // --- 2. Execution: Iterate forward and compute dirty tiles for each node in
  // plan ---
  auto compute_node_hp = [&](int nid, HpPlanEntry& entry) {
    if (is_rect_empty(entry.roi_hp))
      return;
    Node& node = graph.mutable_node(nid);

    if (runtime_ptr) {
      runtime_ptr->log_event(GraphRuntime::SchedulerEvent::EXECUTE, nid);
    }

    auto resolved_inputs = compute::NodeInputResolver::resolve(
        node,
        [&](int upstream_id) -> const NodeOutput* {
          const Node* upstream = graph.find_node(upstream_id);
          if (!upstream)
            return nullptr;
          return compute::ComputeCachePolicy::reusable_output(*upstream);
        },
        "HP update");
    const auto& image_inputs_ready = resolved_inputs.image_inputs;

    const auto* impls =
        OpRegistry::instance().get_implementations(node.type, node.subtype);
    const TileOpFunc* hp_tile_fn =
        (impls && impls->tiled_hp) ? &*impls->tiled_hp : nullptr;
    const MonolithicOpFunc* hp_mono_fn =
        (impls && impls->monolithic_hp) ? &*impls->monolithic_hp : nullptr;

    if (hp_tile_fn) {
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
        hp_buffer = make_aligned_cpu_image_buffer(
            entry.hp_size.width, entry.hp_size.height, channels, dtype);
      }

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
              std::min(kHpMacroTileSize,
                       macro_cover.y + macro_cover.height - y));
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
            if (runtime_ptr) {
              runtime_ptr->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE,
                                     nid);
            }
            compute::NodeExecutor::execute_tile_task(task, *hp_tile_fn);
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
                in_tile.buffer =
                    const_cast<ImageBuffer*>(&in_out->image_buffer);
                in_tile.roi = clip_rect(expand_rect(micro_tile, entry.halo_hp),
                                        cv::Size(in_out->image_buffer.width,
                                                 in_out->image_buffer.height));
                task.input_tiles.push_back(in_tile);
              }
              if (runtime_ptr) {
                runtime_ptr->log_event(
                    GraphRuntime::SchedulerEvent::EXECUTE_TILE, nid);
              }
              compute::NodeExecutor::execute_tile_task(task, *hp_tile_fn);
            }
          }
        }
      }
    } else if (hp_mono_fn) {
      node.cached_output_high_precision =
          (*hp_mono_fn)(node, image_inputs_ready);

      if (!node.cached_output_high_precision) {
        throw GraphError(GraphErrc::ComputeError,
                         "Monolithic HP operator produced no output for " +
                             node.type + ":" + node.subtype);
      }
    } else {
      throw GraphError(GraphErrc::NoOperation,
                       "No suitable HP operator (tiled or monolithic) for " +
                           node.type + ":" + node.subtype);
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

  for (const auto& work : compute_plan.planned_work) {
    auto entry_it = plan.find(work.node_id);
    if (entry_it != plan.end()) {
      compute_node_hp(work.node_id, entry_it->second);
    }
  }

  if (!downsample_requests.empty()) {
    auto make_downsample_task =
        [runtime_ptr, &graph,
         event_service = std::ref(events_)](DownsampleRequest request) -> Task {
      return [runtime_ptr, &graph, event_service, request]() {
        Node* node_ptr = graph.find_node_mutable(request.node_id);
        if (!node_ptr) {
          return;
        }
        Node& node = *node_ptr;
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
          event_service.get().push(node.id, node.name, "downsample_passthrough",
                                   0.0);
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
          event_service.get().push(node.id, node.name, "downsample_passthrough",
                                   0.0);
          return;
        }

        ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
        bool needs_alloc = (rt_buffer.width != rt_size.width) ||
                           (rt_buffer.height != rt_size.height) ||
                           (rt_buffer.channels != hp_buffer.channels) ||
                           (rt_buffer.type != hp_buffer.type) ||
                           (!rt_buffer.data);
        if (needs_alloc) {
          rt_buffer =
              make_aligned_cpu_image_buffer(rt_size.width, rt_size.height,
                                            hp_buffer.channels, hp_buffer.type);
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
        event_service.get().push(node.id, node.name, "downsample", 0.0);

        if (runtime_ptr) {
          runtime_ptr->log_event(GraphRuntime::SchedulerEvent::EXECUTE_TILE,
                                 node.id);
        }
      };
    };

    for (const auto& request : downsample_requests) {
      Task task = make_downsample_task(request);
      task();
    }
  }

  Node& target = graph.mutable_node(node_id);
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

  (void)runtime;
  (void)cache_precision;
  (void)disable_disk_cache;
  (void)benchmark_events;
  (void)enable_timing;

  RoiPropagationService roi_propagation;
  compute::DirtyRegionPlanner dirty_planner(traversal_, roi_propagation);
  auto dirty_plan = dirty_planner.plan_real_time(graph, node_id, dirty_roi);
  graph.last_dirty_region_snapshot_debug =
      compute::DirtyRegionPlanner::describe_snapshot(dirty_plan.snapshot);
  remember_dirty_snapshot(graph, dirty_plan.snapshot);
  auto plan = dirty_plan.entries;
  auto execution_order = dirty_plan.execution_order;
  compute::ComputeTaskPlanner task_planner;
  const compute::ComputePlan compute_plan = task_planner.plan(
      {ComputeIntent::RealTimeUpdate, node_id, false, dirty_roi},
      execution_order, &dirty_plan.snapshot, &graph);
  remember_compute_plan(graph, compute_plan);
  for (const auto& work : compute_plan.planned_work) {
    auto entry_it = plan.find(work.node_id);
    if (entry_it == plan.end())
      continue;
    if (!is_rect_empty(work.represented_hp_roi)) {
      entry_it->second.roi_hp =
          clip_rect(work.represented_hp_roi, entry_it->second.hp_size);
    }
    if (!is_rect_empty(work.execution_roi)) {
      entry_it->second.roi_rt =
          clip_rect(work.execution_roi, entry_it->second.rt_size);
    }
  }

  if (force_recache) {
    for (const auto& kv : plan) {
      Node& node = graph.mutable_node(kv.first);
      node.cached_output_real_time.reset();
      node.rt_roi.reset();
      node.rt_version = 0;
    }
  }

  auto compute_node_rt = [&](int nid, RtPlanEntry& entry) {
    Node& node = graph.mutable_node(nid);
    if (is_rect_empty(entry.roi_rt))
      return;

    auto resolved_inputs = compute::NodeInputResolver::resolve(
        node,
        [&](int upstream_id) -> const NodeOutput* {
          const Node* upstream = graph.find_node(upstream_id);
          if (!upstream)
            return nullptr;
          return compute::ComputeCachePolicy::interactive_output(*upstream);
        },
        "RT update");
    const auto& image_inputs_ready = resolved_inputs.image_inputs;

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
      rt_buffer = make_aligned_cpu_image_buffer(
          entry.rt_size.width, entry.rt_size.height, channels, dtype);
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
                if (rt_buffer.width != entry.rt_size.width ||
                    rt_buffer.height != entry.rt_size.height ||
                    rt_buffer.channels != result.image_buffer.channels ||
                    rt_buffer.type != result.image_buffer.type ||
                    !rt_buffer.data) {
                  rt_buffer = make_aligned_cpu_image_buffer(
                      entry.rt_size.width, entry.rt_size.height,
                      result.image_buffer.channels, result.image_buffer.type);
                }
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
                  compute::NodeExecutor::execute_tile_task(task, fn);
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

  for (const auto& work : compute_plan.planned_work) {
    auto it = plan.find(work.node_id);
    if (it == plan.end())
      continue;
    compute_node_rt(work.node_id, it->second);
  }

  Node& target = graph.mutable_node(node_id);
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
  compute::ParallelGraphExecutor executor(traversal_, cache_, events_);
  SchedulerTaskRuntime& task_runtime =
      scheduler_task_runtime_for(runtime, ComputeIntent::GlobalHighPrecision);
  return executor.execute(
      graph, task_runtime, node_id, cache_precision, force_recache,
      enable_timing, disable_disk_cache, benchmark_events,
      [this, &cache_precision, enable_timing, benchmark_events](
          GraphModel& fallback_graph, int fallback_node_id,
          bool allow_disk_cache) -> NodeOutput& {
        std::unordered_map<int, bool> visiting;
        return compute_internal(fallback_graph, fallback_node_id,
                                cache_precision, visiting, enable_timing,
                                allow_disk_cache, benchmark_events);
      });
}

NodeOutput& ComputeService::compute_parallel(
    GraphModel& graph, GraphRuntime& runtime, ComputeIntent intent, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi) {
  return compute_intent_update_impl(
      graph, &runtime, true, intent, node_id, cache_precision, force_recache,
      enable_timing, disable_disk_cache, benchmark_events, dirty_roi);
}

NodeOutput& ComputeService::compute_intent_update_impl(
    GraphModel& graph, GraphRuntime* runtime, bool use_parallel_executor,
    ComputeIntent intent, int node_id, const std::string& cache_precision,
    bool force_recache, bool enable_timing, bool disable_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi) {
  compute::IntentUpdateCallbacks callbacks;
  const Node* coordinator_node = graph.find_node(node_id);
  const std::string coordinator_node_name =
      coordinator_node ? coordinator_node->name : std::string();
  callbacks.run_global_high_precision = [&]() -> NodeOutput& {
    if (use_parallel_executor) {
      if (!runtime) {
        throw GraphError(GraphErrc::ComputeError,
                         "Parallel HP compute requires a runtime.");
      }
      return compute_parallel(graph, *runtime, node_id, cache_precision,
                              force_recache, enable_timing, disable_disk_cache,
                              benchmark_events);
    }
    return compute(graph, node_id, cache_precision, force_recache,
                   enable_timing, disable_disk_cache, benchmark_events);
  };
  callbacks.run_global_high_precision_dirty_recompute = [&]() -> NodeOutput& {
    // TODO: For now, a dirty ROI on a global compute triggers a full recompute.
    if (use_parallel_executor) {
      if (!runtime) {
        throw GraphError(GraphErrc::ComputeError,
                         "Parallel HP compute requires a runtime.");
      }
      return compute_parallel(graph, *runtime, node_id, cache_precision, true,
                              enable_timing, disable_disk_cache,
                              benchmark_events);
    }
    return compute(graph, node_id, cache_precision, true, enable_timing,
                   disable_disk_cache, benchmark_events);
  };
  callbacks.run_high_precision_update = [&]() {
    compute_high_precision_update(
        graph, runtime, node_id, cache_precision, force_recache, enable_timing,
        disable_disk_cache, nullptr /* no events */, *dirty_roi);
  };
  callbacks.run_real_time_update = [&]() -> NodeOutput& {
    return compute_real_time_update(
        graph, runtime, node_id, cache_precision, force_recache, enable_timing,
        disable_disk_cache, benchmark_events, *dirty_roi);
  };
  callbacks.real_time_output = [&]() -> NodeOutput& {
    Node& target = graph.mutable_node(node_id);
    if (!target.cached_output_real_time) {
      throw GraphError(GraphErrc::ComputeError,
                       "RT compute finished without target output.");
    }
    return *target.cached_output_real_time;
  };
  callbacks.record_stage = [&,
                            coordinator_node_name](const std::string& stage) {
    events_.push(node_id, coordinator_node_name, stage, 0.0);
  };

  SchedulerTaskRuntime* hp_task_runtime = nullptr;
  SchedulerTaskRuntime* rt_task_runtime = nullptr;
  if (use_parallel_executor && runtime) {
    hp_task_runtime = &scheduler_task_runtime_for(
        *runtime, ComputeIntent::GlobalHighPrecision);
    if (intent == ComputeIntent::RealTimeUpdate) {
      rt_task_runtime =
          &scheduler_task_runtime_for(*runtime, ComputeIntent::RealTimeUpdate);
    }
  }

  return compute::IntentUpdateCoordinator::coordinate_intent_update(
      intent, hp_task_runtime, rt_task_runtime, dirty_roi, callbacks);
}

// Phase 1 overload: intent-based entry to sequential compute
NodeOutput& ComputeService::compute_with_intent_impl(
    GraphModel& graph, ComputeIntent intent, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi) {
  return compute_intent_update_impl(
      graph, nullptr, false, intent, node_id, cache_precision, force_recache,
      enable_timing, disable_disk_cache, benchmark_events, dirty_roi);
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
  compute::ComputeTaskPlanner task_planner;
  const compute::ComputePlan compute_plan = task_planner.plan(
      {ComputeIntent::GlobalHighPrecision, node_id, false, std::nullopt},
      execution_order, nullptr, &graph);
  remember_compute_plan(graph, compute_plan);
  execution_order = compute_plan.planned_nodes;

  if (force_recache) {
    std::lock_guard<std::mutex> lk(graph.graph_mutex_);
    for (int nid : execution_order) {
      auto& node = graph.mutable_node(nid);
      node.cached_output_high_precision.reset();
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
