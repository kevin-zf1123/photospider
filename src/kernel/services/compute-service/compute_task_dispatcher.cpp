#include "kernel/services/compute-service/compute_task_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/task_graph_planning.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

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

ComputeTaskDispatcher::ComputeTaskDispatcher(GraphTraversalService& traversal,
                                             GraphCacheService& cache,
                                             GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

void ComputeTaskDispatcher::clear_timing_results(GraphModel& graph) {
  std::lock_guard<std::mutex> lk(graph.timing_mutex_);
  graph.timing_results.node_timings.clear();
  graph.timing_results.total_ms = 0.0;
}

void ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
    std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
    std::optional<uint64_t> epoch, std::function<void()> before_downstream) {
  auto submit_one = [&](SchedulerTaskRuntime::Task&& task,
                        SchedulerTaskPriority priority) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> future = done->get_future();
    task_runtime.submit_ready_task_any_thread(
        [task = std::move(task), done, &task_runtime]() mutable {
          try {
            if (task) {
              task();
            }
            done->set_value();
          } catch (...) {
            auto error = std::current_exception();
            task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
            task_runtime.set_exception(error);
            done->set_exception(error);
          }
        },
        priority, epoch);
    future.get();
  };

  auto submit_and_wait = [&](std::vector<SchedulerTaskRuntime::Task>&& tasks,
                             SchedulerTaskPriority priority,
                             bool preserve_order) {
    if (preserve_order) {
      for (auto& task : tasks) {
        submit_one(std::move(task), priority);
      }
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(tasks.size());
    for (auto& task : tasks) {
      auto done = std::make_shared<std::promise<void>>();
      futures.push_back(done->get_future());
      task_runtime.submit_ready_task_any_thread(
          [task = std::move(task), done, &task_runtime]() mutable {
            try {
              if (task) {
                task();
              }
              done->set_value();
            } catch (...) {
              auto error = std::current_exception();
              task_runtime.log_event(SchedulerTraceAction::RethrowException,
                                     -1);
              task_runtime.set_exception(error);
              done->set_exception(error);
            }
          },
          priority, epoch);
    }
    for (auto& future : futures) {
      future.get();
    }
  };

  submit_and_wait(std::move(source_tasks), SchedulerTaskPriority::High,
                  false);
  if (before_downstream) {
    try {
      before_downstream();
    } catch (...) {
      auto error = std::current_exception();
      task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
      task_runtime.set_exception(error);
      std::rethrow_exception(error);
    }
  }
  submit_and_wait(std::move(downstream_tasks), SchedulerTaskPriority::Normal,
                  true);
}

NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime, int node_id,
    const std::string& cache_precision, bool force_recache, bool enable_timing,
    bool disable_disk_cache, std::vector<BenchmarkEvent>* benchmark_events,
    SequentialFallback sequential_fallback) {
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
    FullTaskGraphExpander full_expander;
    NodeCacheTaskGraphPruner node_cache_pruner;
    const ComputeRequest request{ComputeIntent::GlobalHighPrecision, node_id,
                                 true, std::nullopt};
    const FullTaskGraph full_graph =
        full_expander.expand(graph, request.intent);
    const ComputePlan compute_plan =
        node_cache_pruner.prune(full_graph, request, execution_order, graph);
    remember_compute_plan(graph, compute_plan);
    execution_order = compute_plan.planned_nodes;
    std::unordered_set<int> execution_set(execution_order.begin(),
                                          execution_order.end());

    if (force_recache) {
      std::scoped_lock lock(graph_mutex);
      for (int id : execution_order) {
        if (graph.has_node(id)) {
          graph.mutable_node(id).cached_output_high_precision.reset();
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
    std::vector<SchedulerTaskRuntime::Task> all_tasks;
    all_tasks.resize(num_nodes);
    // Scheme B: temporary results storage per node index
    std::vector<std::optional<NodeOutput>> temp_results(num_nodes);
    // Pre-resolve ops on main thread to avoid concurrent registry access
    std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops(num_nodes);
    for (size_t i = 0; i < num_nodes; ++i) {
      const auto& n = graph.node(execution_order[i]);
      resolved_ops[i] = OpRegistry::instance().resolve_for_intent(
          n.type, n.subtype, ComputeIntent::GlobalHighPrecision);
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

      auto inner_task = [this, &graph, &timing_mutex, &timing_results,
                         &task_runtime, &dependency_counters, &dependents_map,
                         &all_tasks, &id_to_idx, &temp_results, &resolved_ops,
                         current_node_id, current_node_idx,
                         cache_precision, enable_timing, disable_disk_cache,
                         benchmark_events, force_recache]() {
        // 1) Pure compute into temp_results without mutating GraphModel
        try {
          const Node& target_node = graph.node(current_node_id);
          bool allow_disk_cache = (!disable_disk_cache) && (!force_recache);

          // Ensure upstream writes visible
          std::atomic_thread_fence(std::memory_order_acquire);

          auto get_upstream_output = [&](int up_id) -> const NodeOutput* {
            if (up_id < 0)
              return nullptr;
            const Node* upstream = graph.find_node(up_id);
            if (!upstream)
              return nullptr;
            auto it_idx = id_to_idx.find(up_id);
            if (it_idx != id_to_idx.end()) {
              int up_idx = it_idx->second;
              if (temp_results[up_idx].has_value())
                return &*temp_results[up_idx];
            }
            if (upstream->cached_output_high_precision.has_value())
              return &*upstream->cached_output_high_precision;
            return nullptr;
          };

          // Memory or disk fast path
          if (!target_node.cached_output_high_precision.has_value() &&
              allow_disk_cache && !temp_results[current_node_idx].has_value()) {
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

            TiledExecutionConfig tiled_config;
            if (std::holds_alternative<TileOpFunc>(*op_opt)) {
              if (auto metadata = OpRegistry::instance().get_metadata(
                      target_node.type, target_node.subtype)) {
                tiled_config.metadata = *metadata;
                if (metadata->tile_preference == TileSizePreference::MICRO) {
                  tiled_config.tile_size = 16;
                } else if (metadata->tile_preference ==
                           TileSizePreference::MACRO) {
                  tiled_config.tile_size = 256;
                }
              }
              tiled_config.on_tile = [&task_runtime,
                                      current_node_id](const cv::Rect&) {
                task_runtime.log_event(SchedulerTraceAction::ExecuteTile,
                                       current_node_id);
              };
            }
            NodeOutput result =
                NodeExecutor::execute(graph, node_for_exec, *op_opt,
                                      inputs_ready, tiled_config);
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
          task_runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Compute stage at node " + std::to_string(current_node_id) +
                  " (" + graph.node(current_node_id).name +
                  ") failed: " + std::string(e.what()))));
          return;
        } catch (const std::exception& e) {
          task_runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Compute stage at node " + std::to_string(current_node_id) +
                  " (" + graph.node(current_node_id).name +
                  ") failed: " + e.what())));
          return;
        } catch (...) {
          task_runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Compute stage at node " + std::to_string(current_node_id) +
                  " (" + graph.node(current_node_id).name +
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
              task_runtime.submit_ready_task_from_worker(
                  std::move(all_tasks[dependent_idx]));
            }
          }
        } catch (const std::out_of_range& e) {
          task_runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Scheduling stage after node " + std::to_string(current_node_id) +
                  " (" + graph.node(current_node_id).name +
                  ") failed: out_of_range: " + std::string(e.what()))));
        } catch (const std::exception& e) {
          task_runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Scheduling stage after node " + std::to_string(current_node_id) +
                  " (" + graph.node(current_node_id).name +
                  ") failed: " + e.what())));
        } catch (...) {
          task_runtime.set_exception(std::make_exception_ptr(GraphError(
              GraphErrc::ComputeError,
              "Scheduling stage after node " + std::to_string(current_node_id) +
                  " (" + graph.node(current_node_id).name +
                  ") failed: unknown exception")));
        }
      };

      // --- 包装任务以处理异常和完成计数 ---
      all_tasks[i] = [inner_task = std::move(inner_task), &task_runtime,
                      current_node_id]() {
        task_runtime.log_event(SchedulerTraceAction::Execute, current_node_id);
        inner_task();
        task_runtime.dec_tasks_to_complete();
      };
    }

    // --- 提交初始就绪任务 ---
    std::vector<SchedulerTaskRuntime::Task> initial_tasks;
    std::unordered_set<int> submitted_initial_indices;
    TaskGraphReadyChecker ready_checker;
    const std::vector<int> initial_ready_task_ids =
        ready_checker.initial_ready_task_ids(compute_plan.task_graph);
    for (int task_id : initial_ready_task_ids) {
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
      if (!graph.node(node_id).cached_output_high_precision.has_value()) {
        sequential_fallback(graph, node_id, !disable_disk_cache);
      }
    } else {
      task_runtime.submit_initial_tasks(std::move(initial_tasks),
                                        execution_order.size());
      for (size_t i = 0; i < num_nodes; ++i) {
        if (submitted_initial_indices.count(static_cast<int>(i))) {
          task_runtime.log_event(SchedulerTraceAction::AssignInitial,
                                 execution_order[i]);
        }
      }
      task_runtime.wait_for_completion();
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
          Node& node = graph.mutable_node(nid);
          node.cached_output_high_precision = std::move(*temp_results[i]);
          node.hp_version++;
          cache_.save_cache_if_configured(graph, node, cache_precision);
        }
      }
    }
    if (!graph.node(node_id).cached_output_high_precision) {
      throw GraphError(GraphErrc::ComputeError,
                       "Parallel computation finished but target node has no "
                       "output. An upstream error likely occurred.");
    }
    return *graph.mutable_node(node_id).cached_output_high_precision;
  } catch (...) {
    // 捕获在本函数内（任务提交前）抛出的异常，并传递给运行时
    task_runtime.log_event(SchedulerTraceAction::RethrowException, node_id);
    task_runtime.set_exception(std::current_exception());
    // 等待运行时处理异常并唤醒
    task_runtime.wait_for_completion();
    // wait_for_completion 内部会重新抛出异常，这里我们只需确保函数有返回值
    // 在实际情况下，由于 rethrow，代码不会执行到这里
    throw GraphError(
        GraphErrc::Unknown,
        "Caught pre-flight exception during parallel compute setup.");
  }
}

}  // namespace ps::compute
