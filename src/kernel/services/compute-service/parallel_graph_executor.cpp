#include "kernel/services/compute-service/parallel_graph_executor.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "benchmark/benchmark_types.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/param_utils.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/compute_task_planner.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

using compute::calculate_halo;

void finalize_output_metadata(NodeOutput& output,
                              const std::vector<const NodeOutput*>& inputs,
                              bool enable_timing, double execution_ms) {
  ComputeMetricsRecorder::finalize_output_metadata(output, inputs,
                                                   enable_timing, execution_ms);
}

void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.recent_compute_plans.push_back(compute_plan);
  if (graph.recent_compute_plans.size() > 16) {
    graph.recent_compute_plans.erase(graph.recent_compute_plans.begin());
  }
}

}  // namespace

ParallelGraphExecutor::ParallelGraphExecutor(GraphTraversalService& traversal,
                                             GraphCacheService& cache,
                                             GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

void ParallelGraphExecutor::clear_timing_results(GraphModel& graph) {
  std::lock_guard<std::mutex> lk(graph.timing_mutex_);
  graph.timing_results.node_timings.clear();
  graph.timing_results.total_ms = 0.0;
}

NodeOutput& ParallelGraphExecutor::execute(
    GraphModel& graph, GraphRuntime& runtime, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    SequentialFallback sequential_fallback) {
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
    compute::ComputeTaskPlanner task_planner;
    const ComputePlan compute_plan = task_planner.plan(
        {ComputeIntent::GlobalHighPrecision, node_id, true, std::nullopt},
        execution_order, nullptr, &graph);
    remember_compute_plan(graph, compute_plan);
    execution_order = compute_plan.planned_nodes;
    std::unordered_set<int> execution_set(execution_order.begin(),
                                          execution_order.end());

    if (force_recache) {
      std::scoped_lock lock(graph_mutex);
      for (int id : execution_order) {
        if (nodes.count(id)) {
          nodes.at(id).cached_output.reset();
          nodes.at(id).cached_output_high_precision.reset();
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
      dependency_counters[i] = 0;
    }
    for (const auto& dependency : compute_plan.task_graph.dependencies) {
      if (!execution_set.count(dependency.from_node_id) ||
          !execution_set.count(dependency.to_node_id)) {
        continue;
      }
      const int dep_idx = id_to_idx.at(dependency.from_node_id);
      const int dependent_idx = id_to_idx.at(dependency.to_node_id);
      dependents_map[dep_idx].push_back(dependent_idx);
      ++dependency_counters[dependent_idx];
    }

    // --- 为每个节点（按稠密索引）创建任务 ---
    for (size_t i = 0; i < num_nodes; ++i) {
      int current_node_id = execution_order[i];
      int current_node_idx = i;

      auto inner_task = [this, &graph, &nodes, &timing_mutex, &timing_results,
                         &runtime, &dependency_counters, &dependents_map,
                         &all_tasks, &id_to_idx, &temp_results, &resolved_ops,
                         &resolved_meta, current_node_id, current_node_idx,
                         cache_precision, enable_timing, disable_disk_cache,
                         benchmark_events, force_recache]() {
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
            if (itn->second.cached_output_high_precision.has_value())
              return &*itn->second.cached_output_high_precision;
            if (itn->second.cached_output.has_value())
              return &*itn->second.cached_output;
            return nullptr;
          };

          // Memory or disk fast path
          if (!target_node.cached_output_high_precision.has_value() &&
              !target_node.cached_output.has_value() && allow_disk_cache &&
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

          if (!target_node.cached_output_high_precision.has_value() &&
              !target_node.cached_output.has_value() &&
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
                      ob = make_aligned_cpu_image_buffer(out_w, out_h, out_c,
                                                         out_t);

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
                      auto access_pattern =
                          OpMetadata::InputAccessPattern::SpatialAligned;
                      if (resolved_meta[current_node_idx].has_value()) {
                        access_pattern =
                            resolved_meta[current_node_idx]->access_pattern;
                      }
                      auto prop_fn =
                          OpRegistry::instance().get_dirty_propagator(
                              target_node.type, target_node.subtype);

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
                                            y, w, h, needs_halo, input_ptrs,
                                            norm_store_sp, remaining, start_tp,
                                            current_node_idx, current_node_id,
                                            node_for_exec, op_func,
                                            benchmark_events, enable_timing,
                                            access_pattern, prop_fn, &graph]() {
                            try {
                              runtime.log_event(
                                  GraphRuntime::SchedulerEvent::EXECUTE_TILE,
                                  current_node_id);
                              TileTask tt;
                              tt.node = &node_for_exec;
                              tt.output_tile.buffer =
                                  &temp_results[current_node_idx]->image_buffer;
                              tt.output_tile.roi = cv::Rect(x, y, w, h);
                              for (auto const* in_out : input_ptrs) {
                                Tile in_tile;
                                in_tile.buffer = const_cast<ImageBuffer*>(
                                    &in_out->image_buffer);
                                cv::Rect input_roi;
                                if (access_pattern ==
                                    OpMetadata::InputAccessPattern::
                                        RandomAccess) {
                                  input_roi = prop_fn(
                                      node_for_exec, tt.output_tile.roi, graph);
                                  input_roi =
                                      input_roi &
                                      cv::Rect(0, 0, in_tile.buffer->width,
                                               in_tile.buffer->height);
                                } else if (needs_halo) {
                                  input_roi = calculate_halo(
                                      tt.output_tile.roi, HALO_SIZE,
                                      {in_out->image_buffer.width,
                                       in_out->image_buffer.height});
                                } else {
                                  input_roi = tt.output_tile.roi;
                                }
                                in_tile.roi = input_roi;
                                tt.input_tiles.push_back(std::move(in_tile));
                              }
                              compute::NodeExecutor::execute_tile_task(tt,
                                                                       op_func);
                              if (remaining->fetch_sub(
                                      1, std::memory_order_acq_rel) == 1) {
                                std::atomic_thread_fence(
                                    std::memory_order_release);
                                double exec_ms_for_meta = 0.0;
                                if (enable_timing) {
                                  auto end_tp =
                                      std::chrono::high_resolution_clock::now();
                                  double exec_ms =
                                      std::chrono::duration<double, std::milli>(
                                          end_tp - *start_tp)
                                          .count();
                                  exec_ms_for_meta = exec_ms;
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
                                finalize_output_metadata(
                                    *temp_results[current_node_idx], input_ptrs,
                                    enable_timing, exec_ms_for_meta);
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
            double exec_ms_for_meta = 0.0;
            if (enable_timing) {
              current_event.execution_end_time =
                  std::chrono::high_resolution_clock::now();
              double ms = std::chrono::duration<double, std::milli>(
                              current_event.execution_end_time -
                              current_event.execution_start_time)
                              .count();
              exec_ms_for_meta = ms;
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
            finalize_output_metadata(result, inputs_ready, enable_timing,
                                     exec_ms_for_meta);
            temp_results[current_node_idx] = std::move(result);
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
    std::unordered_set<int> submitted_initial_indices;
    for (int task_id : compute_plan.task_graph.initial_task_ids) {
      if (task_id < 0 ||
          task_id >= static_cast<int>(compute_plan.task_graph.tasks.size())) {
        continue;
      }
      const int planned_node_id =
          compute_plan.task_graph.tasks[task_id].node_id;
      auto idx_it = id_to_idx.find(planned_node_id);
      if (idx_it == id_to_idx.end())
        continue;
      const int node_idx = idx_it->second;
      if (dependency_counters[node_idx] != 0)
        continue;
      if (submitted_initial_indices.insert(node_idx).second) {
        initial_tasks.push_back(std::move(all_tasks[node_idx]));
      }
    }
    if (initial_tasks.empty()) {
      for (size_t i = 0; i < num_nodes; ++i) {
        if (dependency_counters[i] == 0) {
          submitted_initial_indices.insert(static_cast<int>(i));
          initial_tasks.push_back(std::move(all_tasks[i]));
        }
      }
    }

    if (execution_order.empty() && graph.has_node(node_id)) {
      // 处理图中只有一个节点的情况
      if (!nodes.at(node_id).cached_output_high_precision.has_value() &&
          !nodes.at(node_id).cached_output.has_value()) {
        sequential_fallback(graph, node_id, !disable_disk_cache);
      }
    } else {
      runtime.submit_initial_tasks(std::move(initial_tasks),
                                   execution_order.size());
      for (size_t i = 0; i < num_nodes; ++i) {
        if (submitted_initial_indices.count(static_cast<int>(i))) {
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
          nodes.at(nid).cached_output_high_precision =
              std::move(*temp_results[i]);
          nodes.at(nid).hp_version++;
          cache_.save_cache_if_configured(graph, nodes.at(nid),
                                          cache_precision);
        }
      }
    }
    if (!nodes.at(node_id).cached_output_high_precision &&
        !nodes.at(node_id).cached_output) {
      throw GraphError(GraphErrc::ComputeError,
                       "Parallel computation finished but target node has no "
                       "output. An upstream error likely occurred.");
    }
    return nodes.at(node_id).cached_output_high_precision.has_value()
               ? *nodes.at(node_id).cached_output_high_precision
               : *nodes.at(node_id).cached_output;
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

}  // namespace ps::compute
