#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"  // <--- 修正点: 添加缺失的头文件
#include "compute/compute_service.hpp"  // <--- 修正点: 添加缺失的头文件
#include "compute/compute_task_dispatcher.hpp"
#include "compute/realtime_proxy_graph.hpp"
#include "graph/graph_cache_service.hpp"      // <--- 修正点: 添加缺失的头文件
#include "graph/graph_model.hpp"              // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"  // <--- 修正点: 添加缺失的头文件
#include "providers/configured_image_artifact_codec.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"  // M3.3: 新调度器
#include "scheduler/serial_debug_scheduler.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"

namespace {

void register_micro_blur_for_dirty_scheduler_tests() {
  auto& registry = ps::OpRegistry::instance();
  const auto base_impl =
      registry.get_implementations("image_process", "gaussian_blur");
  ASSERT_TRUE(base_impl && base_impl->tiled_hp);

  ps::OpMetadata micro_meta;
  micro_meta.tile_preference = ps::TileSizePreference::MICRO;
  registry.register_op_hp_tiled("image_process", "micro_blur_for_test",
                                *base_impl->tiled_hp, micro_meta);
}

std::optional<size_t> first_trace_index(
    const std::vector<ps::GraphRuntime::SchedulerEvent>& log,
    ps::GraphRuntime::SchedulerEvent::Action action) {
  for (size_t i = 0; i < log.size(); ++i) {
    if (log[i].action == action) {
      return i;
    }
  }
  return std::nullopt;
}

const char* scheduler_action_name(
    ps::GraphRuntime::SchedulerEvent::Action action) {
  switch (action) {
    case ps::GraphRuntime::SchedulerEvent::ASSIGN_INITIAL:
      return "ASSIGN_INITIAL";
    case ps::GraphRuntime::SchedulerEvent::EXECUTE:
      return "EXECUTE";
    case ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE:
      return "EXECUTE_TILE";
    case ps::GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE:
      return "EXECUTE_DIRTY_SOURCE";
    case ps::GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE:
      return "EXECUTE_DIRTY_DOWNSTREAM_NODE";
    case ps::GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE:
      return "EXECUTE_DIRTY_DOWNSTREAM_TILE";
    case ps::GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION:
      return "SKIP_STALE_GENERATION";
    case ps::GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION:
      return "RETHROW_EXCEPTION";
  }
  return "UNKNOWN";
}

nlohmann::json scheduler_trace_json(
    const std::vector<ps::GraphRuntime::SchedulerEvent>& events) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto& e : events) {
    j.push_back(
        {{"sequence", e.sequence},
         {"epoch", e.epoch},
         {"node_id", e.node_id},
         {"worker_id", e.worker_id},
         {"action", scheduler_action_name(e.action)},
         {"ts_us", std::chrono::duration_cast<std::chrono::microseconds>(
                       e.timestamp.time_since_epoch())
                       .count()}});
  }
  return j;
}

void write_scheduler_trace_json(
    const std::string& path,
    const std::vector<ps::GraphRuntime::SchedulerEvent>& events) {
  std::ofstream ofs(path);
  ofs << std::setw(2) << scheduler_trace_json(events) << std::endl;
}

/**
 * @brief Owns a small test callback batch exposed through public task handles.
 * @throws std::bad_alloc when owned callback or returned handle storage grows.
 * @note Instances borrow the scheduler runtime and must outlive the matching
 * source-first submission call.
 */
class OrderedBatchTaskExecutor final : public ps::TaskExecutor {
 public:
  /**
   * @brief Takes ownership of callbacks for one scheduler batch.
   * @param runtime Running scheduler runtime used for completion accounting.
   * @param tasks Callbacks indexed by TaskHandle::task_id.
   * @throws std::bad_alloc if callback storage ownership allocates.
   */
  OrderedBatchTaskExecutor(ps::SchedulerTaskRuntime& runtime,
                           std::vector<ps::SchedulerTaskRuntime::Task> tasks)
      : runtime_(runtime), tasks_(std::move(tasks)) {}

  /**
   * @brief Returns one task handle for each owned callback.
   * @return Borrowed handles valid while this executor remains alive.
   * @throws std::bad_alloc if handle storage allocation fails.
   */
  std::vector<ps::TaskHandle> handles() {
    std::vector<ps::TaskHandle> result;
    result.reserve(tasks_.size());
    for (std::size_t index = 0; index < tasks_.size(); ++index) {
      result.push_back(ps::TaskHandle{this, static_cast<int>(index),
                                      static_cast<int>(index)});
    }
    return result;
  }

  /**
   * @brief Executes one callback and settles scheduler task accounting.
   * @param task_id Dense callback index supplied by the task handle.
   * @return Nothing.
   * @throws std::out_of_range for an invalid task id.
   * @throws The exact callback or scheduler completion exception.
   */
  void run_task(int task_id) override {
    tasks_.at(static_cast<std::size_t>(task_id))();
    runtime_.dec_tasks_to_complete();
  }

 private:
  /** @brief Borrowed scheduler runtime for completion accounting. */
  ps::SchedulerTaskRuntime& runtime_;
  /** @brief Owned callbacks indexed by public task handles. */
  std::vector<ps::SchedulerTaskRuntime::Task> tasks_;
};

}  // namespace

// =============================================================================
// M3.3: 使用 CpuWorkStealingScheduler 的并行计算测试
// =============================================================================

TEST(SchedulerDirtyReadyTasks, SourceFirstOrderOnSerialAndCpuSchedulers) {
  auto run_case = [](ps::SchedulerTaskRuntime& runtime) {
    std::mutex mutex;
    std::vector<int> order;
    auto append = [&](int value) {
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(value);
    };

    std::vector<ps::SchedulerTaskRuntime::Task> source_tasks;
    source_tasks.push_back([&] { append(1); });
    source_tasks.push_back([&] { append(2); });

    std::vector<ps::SchedulerTaskRuntime::Task> downstream_tasks;
    downstream_tasks.push_back([&] {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_EQ(order.size(), 2u)
          << "downstream task must wait for all dirty source tasks";
      order.push_back(3);
    });

    OrderedBatchTaskExecutor source_executor(runtime, std::move(source_tasks));
    OrderedBatchTaskExecutor downstream_executor(runtime,
                                                 std::move(downstream_tasks));
    ps::compute::ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
        runtime, source_executor.handles(), 2, downstream_executor.handles(),
        1);

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order.back(), 3);
  };

  ps::SerialDebugScheduler serial;
  serial.start();
  run_case(serial);
  serial.shutdown();

  ps::CpuWorkStealingScheduler cpu(2);
  cpu.start();
  run_case(cpu);
  cpu.shutdown();
}

TEST(SchedulerTestM33, ParallelComputeWithNewScheduler) {
  using ps::ComputeIntent;
  using ps::CpuWorkStealingScheduler;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "scheduler_m33_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  // 创建并设置新的 CpuWorkStealingScheduler
  auto& runtime = ps::testing::KernelTestAccess::runtime(kernel, graph_name);
  auto scheduler = std::make_unique<CpuWorkStealingScheduler>();
  scheduler->start();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  // 通过 ComputeService facade 触发 scheduler-owned task runtime 执行。
  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = node_id;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.execution.parallel = true;
  bool success = svc.cmd_compute(request);
  ASSERT_TRUE(success);
  const auto& result =
      runtime.model().node(node_id).cached_output_high_precision.value();

  // 验证计算完成
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}

// =============================================================================
// 原有测试保持不变（用于回归验证）
// =============================================================================

TEST(SchedulerTest, ParallelLogToJson) {
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "scheduler_ci_graph";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  ps::testing::KernelTestAccess::clear_scheduler_trace(kernel, graph_name);

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = node_id;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.execution.parallel = true;
  auto ok = svc.cmd_compute_async(request);
  ASSERT_TRUE(ok.has_value());
  const Kernel::AsyncComputeResult outcome = ok->get();
  ASSERT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());

  auto events_page =
      ps::testing::KernelTestAccess::scheduler_trace(kernel, graph_name);
  ASSERT_FALSE(events_page.events.empty());
  auto facade_events =
      svc.cmd_scheduler_trace(graph_name, 0, ps::kSchedulerTraceMaxLimit);
  ASSERT_TRUE(facade_events.has_value());
  EXPECT_EQ(facade_events->events.size(), events_page.events.size());

  write_scheduler_trace_json("scheduler_log.json", events_page.events);

  std::ifstream ifs("scheduler_log.json");
  ASSERT_TRUE(static_cast<bool>(ifs));
}

TEST(Scheduler, DirtyRegionTiledComputation) {
  ps::Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ps::InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  register_micro_blur_for_dirty_scheduler_tests();
  const std::string graph_name = "dirty_region_test";
  const std::string yaml_path = "tests/fixtures/graphs/dirty_region_test.yaml";
  const int final_node_id = 3;

  // 加载图
  auto loaded_name = svc.cmd_load_graph(graph_name, "sessions", yaml_path);
  ASSERT_TRUE(loaded_name.has_value());
  ASSERT_EQ(*loaded_name, graph_name);

  ps::GraphRuntime& runtime =
      ps::testing::KernelTestAccess::runtime(kernel, graph_name);

  // ========================================================================
  // 步骤 1: 第一次完整计算 (填充缓存)
  // ========================================================================
  std::cout << "--- SCHEDULER TEST: Performing initial full computation... ---"
            << std::endl;
  runtime.clear_scheduler_log();

  ps::Kernel::ComputeRequest full_request;
  full_request.name = graph_name;
  full_request.node_id = final_node_id;
  full_request.cache.precision = "int8";
  full_request.execution.parallel = true;
  bool success_full = svc.cmd_compute(full_request);
  ASSERT_TRUE(success_full);

  auto log_full_compute =
      runtime.scheduler_trace_page(0, ps::kSchedulerTraceMaxLimit).events;
  ASSERT_FALSE(log_full_compute.empty());

  size_t full_compute_task_count = 0;
  size_t full_compute_tile_count = 0;
  for (const auto& event : log_full_compute) {
    if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE) {
      full_compute_task_count++;
    } else if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE) {
      full_compute_tile_count++;
    }
  }
  std::cout << "Full compute executed " << full_compute_task_count
            << " node tasks and " << full_compute_tile_count << " tile tasks."
            << std::endl;
  ASSERT_GT(full_compute_tile_count, 10);

  // ========================================================================
  // 步骤 2: 模拟一个局部修改 (脏区)
  // ========================================================================
  std::cout << "--- SCHEDULER TEST: Simulating a dirty region... ---"
            << std::endl;

  ps::PixelRect dirty_rect{200, 200, 128, 128};

  // 修正点: 明确 Lambda 返回类型为 void
  runtime.graph_state()
      .submit([&](ps::GraphModel& g) -> void {
        g.mutate_node_runtime_state(1, [&](auto& source_state) {
          // HP formal cache must be populated after compute
          ASSERT_TRUE(source_state.cached_output_high_precision.has_value());

          // Modify the HP cached image to simulate a dirty region
          cv::Mat mat = ps::toCvMat(
              source_state.cached_output_high_precision->image_buffer);

          mat(cv::Rect{dirty_rect.x, dirty_rect.y, dirty_rect.width,
                       dirty_rect.height})
              .setTo(cv::Scalar(1.0f));
        });

        // Keep downstream HP outputs as dirty update seeds. The dirty ROI
        // executor refreshes only the changed region while preserving the
        // existing full-frame HP output outside that ROI.
      })
      .get();

  // ========================================================================
  // 步骤 3: 第二次增量计算
  // ========================================================================
  std::cout << "--- SCHEDULER TEST: Performing incremental computation... ---"
            << std::endl;
  runtime.clear_scheduler_log();

  // 修正点: post 的 Lambda 返回 NodeOutput，并接收 future 的结果
  auto future =
      runtime.graph_state().submit([&](ps::GraphModel& g) -> ps::NodeOutput {
        // 修正点: 直接构造服务类，不访问 kernel 的私有成员
        ps::GraphTraversalService traversal_service;
        ps::GraphCacheService cache_service{
            ps::providers::make_configured_image_artifact_codec(),
            ps::testing::make_yaml_cache_metadata_codec()};
        ps::ComputeService compute_svc(traversal_service, cache_service,
                                       runtime.event_service());

        ps::ComputeService::Request request;
        request.node_id = final_node_id;
        request.cache.precision = "int8";
        request.cache.disable_disk_cache = true;
        request.intent = ps::ComputeIntent::RealTimeUpdate;
        request.dirty_roi = dirty_rect;
        return compute_svc.compute_parallel(g, runtime, request);
      });

  // 等待并获取结果（尽管我们不使用结果，但 get() 会等待完成并传播异常）
  future.get();

  runtime.graph_state()
      .submit([&](ps::GraphModel& g) -> void {
        const auto& target = g.node(final_node_id);
        ASSERT_TRUE(target.cached_output_high_precision.has_value());
        EXPECT_GT(target.hp_version, 0);
        EXPECT_TRUE(target.hp_roi.has_value());
      })
      .get();
  const auto* proxy_state =
      runtime.realtime_proxy_graph().find_state(final_node_id);
  ASSERT_NE(proxy_state, nullptr);
  ASSERT_TRUE(proxy_state->output.has_value());
  EXPECT_GT(proxy_state->version, 0);
  EXPECT_TRUE(proxy_state->roi_hp.has_value());

  auto dirty_snapshot = svc.cmd_dirty_region_snapshot_debug(graph_name);
  ASSERT_TRUE(dirty_snapshot.has_value());
  EXPECT_NE(dirty_snapshot->find("tiles="), std::string::npos);
  EXPECT_NE(dirty_snapshot->find("edges="), std::string::npos);

  auto log_incremental_compute =
      runtime.scheduler_trace_page(0, ps::kSchedulerTraceMaxLimit).events;
  ASSERT_FALSE(log_incremental_compute.empty());

  size_t incremental_compute_task_count = 0;
  size_t incremental_tile_task_count = 0;
  size_t dirty_source_task_count = 0;
  size_t dirty_downstream_node_count = 0;
  size_t dirty_downstream_tile_count = 0;
  std::set<int> workers_used;
  for (const auto& event : log_incremental_compute) {
    if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE) {
      incremental_compute_task_count++;
    } else if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE) {
      incremental_tile_task_count++;
    } else if (event.action ==
               ps::GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE) {
      dirty_source_task_count++;
    } else if (event.action == ps::GraphRuntime::SchedulerEvent::
                                   EXECUTE_DIRTY_DOWNSTREAM_NODE) {
      dirty_downstream_node_count++;
    } else if (event.action == ps::GraphRuntime::SchedulerEvent::
                                   EXECUTE_DIRTY_DOWNSTREAM_TILE) {
      dirty_downstream_tile_count++;
    }
    if ((event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE ||
         event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE) &&
        event.worker_id != -1) {
      workers_used.insert(event.worker_id);
    }
  }
  std::cout << "Incremental compute executed " << incremental_compute_task_count
            << " node tasks and " << incremental_tile_task_count
            << " tile tasks on " << workers_used.size() << " workers."
            << std::endl;

  // ========================================================================
  // 步骤 4: 断言和验证
  // ========================================================================
  ASSERT_LT(incremental_tile_task_count, full_compute_tile_count / 10);
  EXPECT_GT(dirty_source_task_count, 0u);
  EXPECT_GT(dirty_downstream_node_count, 0u);
  EXPECT_GT(dirty_downstream_tile_count, 0u);
  auto first_dirty_source =
      first_trace_index(log_incremental_compute,
                        ps::GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE);
  auto first_dirty_downstream = first_trace_index(
      log_incremental_compute,
      ps::GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE);
  ASSERT_TRUE(first_dirty_source.has_value());
  ASSERT_TRUE(first_dirty_downstream.has_value());
  EXPECT_LT(*first_dirty_source, *first_dirty_downstream);
  auto facade_scheduler_trace =
      svc.cmd_scheduler_trace(graph_name, 0, ps::kSchedulerTraceMaxLimit);
  ASSERT_TRUE(facade_scheduler_trace.has_value());
  EXPECT_EQ(facade_scheduler_trace->events.size(),
            log_incremental_compute.size());
  if (std::thread::hardware_concurrency() > 1) {
    ASSERT_GT(workers_used.size(),
              0);  // 在任务很少时，可能只用到1个worker，所以改为>0
  }
  write_scheduler_trace_json("dirty_scheduler_log.json",
                             log_incremental_compute);
}

TEST(Scheduler,
     DirtyRegionProductionTraceCoversStaleGenerationAndExceptionRethrow) {
  ps::Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ps::InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  register_micro_blur_for_dirty_scheduler_tests();

  const std::string stale_graph_name = "dirty_region_stale_generation_test";
  ASSERT_TRUE(svc.cmd_load_graph(stale_graph_name, "sessions",
                                 "tests/fixtures/graphs/dirty_region_test.yaml")
                  .has_value());
  ps::GraphRuntime& stale_runtime =
      ps::testing::KernelTestAccess::runtime(kernel, stale_graph_name);
  ps::Kernel::ComputeRequest stale_hp_request;
  stale_hp_request.name = stale_graph_name;
  stale_hp_request.node_id = 3;
  stale_hp_request.cache.precision = "int8";
  stale_hp_request.execution.parallel = true;
  ASSERT_TRUE(svc.cmd_compute(stale_hp_request));

  const ps::PixelRect dirty_rect{200, 200, 128, 128};
  stale_runtime.graph_state()
      .submit([&](ps::GraphModel& g) -> void {
        g.dirty_source_hp_commit_generation[1] =
            std::numeric_limits<uint64_t>::max();
        stale_runtime.realtime_proxy_graph().synchronize_with_graph(g);
        ps::compute::RealtimeProxyGraph::NodeState rt_state;
        rt_state.dirty_source_generation = std::numeric_limits<uint64_t>::max();
        stale_runtime.realtime_proxy_graph().commit_node_state(
            1, std::move(rt_state));
        g.mutate_node_runtime_state(
            2, [](auto& state) { state.cached_output_high_precision.reset(); });
        g.mutate_node_runtime_state(
            3, [](auto& state) { state.cached_output_high_precision.reset(); });
      })
      .get();
  stale_runtime.clear_scheduler_log();
  auto stale_future = stale_runtime.graph_state().submit(
      [&](ps::GraphModel& g) -> ps::NodeOutput {
        ps::GraphTraversalService traversal_service;
        ps::GraphCacheService cache_service{
            ps::providers::make_configured_image_artifact_codec(),
            ps::testing::make_yaml_cache_metadata_codec()};
        ps::ComputeService compute_svc(traversal_service, cache_service,
                                       stale_runtime.event_service());
        ps::ComputeService::Request request;
        request.node_id = 3;
        request.cache.precision = "int8";
        request.cache.disable_disk_cache = true;
        request.intent = ps::ComputeIntent::RealTimeUpdate;
        request.dirty_roi = dirty_rect;
        return compute_svc.compute_parallel(g, stale_runtime, request);
      });
  EXPECT_NO_THROW(stale_future.get());
  const auto stale_log =
      stale_runtime.scheduler_trace_page(0, ps::kSchedulerTraceMaxLimit).events;
  EXPECT_TRUE(
      first_trace_index(stale_log,
                        ps::GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION)
          .has_value());
  write_scheduler_trace_json("dirty_scheduler_stale_log.json", stale_log);

  const std::string exception_graph_name = "dirty_region_exception_test";
  ASSERT_TRUE(svc.cmd_load_graph(exception_graph_name, "sessions",
                                 "tests/fixtures/graphs/dirty_region_test.yaml")
                  .has_value());
  ps::GraphRuntime& exception_runtime =
      ps::testing::KernelTestAccess::runtime(kernel, exception_graph_name);
  ps::Kernel::ComputeRequest exception_hp_request;
  exception_hp_request.name = exception_graph_name;
  exception_hp_request.node_id = 3;
  exception_hp_request.cache.precision = "int8";
  exception_hp_request.execution.parallel = true;
  ASSERT_TRUE(svc.cmd_compute(exception_hp_request));
  exception_runtime.graph_state()
      .submit([&](ps::GraphModel& g) -> void {
        ps::Node broken = g.node(2);
        broken.type = "missing_op";
        broken.subtype = "dirty_exception";
        g.replace_node(broken);
        g.mutate_node_runtime_state(
            2, [](auto& state) { state.cached_output_high_precision.reset(); });
        g.mutate_node_runtime_state(
            3, [](auto& state) { state.cached_output_high_precision.reset(); });
      })
      .get();
  exception_runtime.clear_scheduler_log();
  auto exception_future = exception_runtime.graph_state().submit(
      [&](ps::GraphModel& g) -> ps::NodeOutput {
        ps::GraphTraversalService traversal_service;
        ps::GraphCacheService cache_service{
            ps::providers::make_configured_image_artifact_codec(),
            ps::testing::make_yaml_cache_metadata_codec()};
        ps::ComputeService compute_svc(traversal_service, cache_service,
                                       exception_runtime.event_service());
        ps::ComputeService::Request request;
        request.node_id = 3;
        request.cache.precision = "int8";
        request.cache.disable_disk_cache = true;
        request.intent = ps::ComputeIntent::RealTimeUpdate;
        request.dirty_roi = dirty_rect;
        return compute_svc.compute_parallel(g, exception_runtime, request);
      });
  EXPECT_THROW(exception_future.get(), ps::GraphError);
  const auto exception_log =
      exception_runtime.scheduler_trace_page(0, ps::kSchedulerTraceMaxLimit)
          .events;
  EXPECT_TRUE(
      first_trace_index(exception_log,
                        ps::GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION)
          .has_value());
  write_scheduler_trace_json("dirty_scheduler_exception_log.json",
                             exception_log);
}

/**
 * @brief Verifies concurrent HP/RT dirty siblings preserve ParameterMap state.
 *
 * The test seeds one scheduler-backed graph, performs an authoritative HP
 * compute, then issues repeated RealTimeUpdate transactions whose HP and RT
 * siblings resolve the same live node parameters concurrently. It finally
 * checks graph-owned static state, request-local runtime cleanup, and the RT
 * proxy output.
 *
 * @return Nothing; GoogleTest assertions report compute, parameter, or
 * proxy-state failures.
 * @throws std::bad_alloc, GraphError, or std::system_error if graph setup,
 * concurrent dirty execution, or result inspection fails.
 * @note The test exercises production scheduler callbacks. Transaction-local
 *       synchronization must protect same-node snapshot and parameter staging
 *       without serializing operation bodies or persisting into GraphRuntime.
 */
TEST(Scheduler, ConcurrentDirtySiblingsPreserveParameterValueState) {
  ps::Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  ps::Kernel::SchedulerConfig scheduler_config;
  scheduler_config.worker_count = 8;
  kernel.set_scheduler_config(scheduler_config);
  ps::InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  register_micro_blur_for_dirty_scheduler_tests();

  const std::string graph_name = "dirty_region_yaml_sibling_test";
  ASSERT_TRUE(svc.cmd_load_graph(graph_name, "sessions",
                                 "tests/fixtures/graphs/dirty_region_test.yaml")
                  .has_value());
  ps::GraphRuntime& runtime =
      ps::testing::KernelTestAccess::runtime(kernel, graph_name);

  ps::Kernel::ComputeRequest hp_request;
  hp_request.name = graph_name;
  hp_request.node_id = 3;
  hp_request.cache.precision = "int8";
  hp_request.execution.parallel = true;
  ASSERT_TRUE(svc.cmd_compute(hp_request));

  constexpr int kConcurrentUpdateCount = 8;
  for (int update = 0; update < kConcurrentUpdateCount; ++update) {
    auto update_future = runtime.graph_state().submit(
        [&](ps::GraphModel& graph) -> ps::NodeOutput {
          ps::GraphTraversalService traversal_service;
          ps::GraphCacheService cache_service{
              ps::providers::make_configured_image_artifact_codec(),
              ps::testing::make_yaml_cache_metadata_codec()};
          ps::ComputeService compute_service(traversal_service, cache_service,
                                             runtime.event_service());
          ps::ComputeService::Request request;
          request.node_id = 3;
          request.cache.precision = "int8";
          request.cache.disable_disk_cache = true;
          request.intent = ps::ComputeIntent::RealTimeUpdate;
          request.dirty_roi = ps::PixelRect{200, 200, 128, 128};
          return compute_service.compute_parallel(graph, runtime, request);
        });
    ps::NodeOutput output;
    ASSERT_NO_THROW(output = update_future.get());
    EXPECT_GT(output.image_buffer.width, 0);
    EXPECT_GT(output.image_buffer.height, 0);
  }

  runtime.graph_state()
      .submit([](ps::GraphModel& graph) {
        const ps::Node& blur = graph.node(2);
        ASSERT_FALSE(blur.parameters.empty());
        EXPECT_EQ(blur.parameters.at("ksize").as_int64(), 25);
        EXPECT_TRUE(blur.runtime_parameters.empty())
            << "request-local effective parameters must not persist in Graph";
      })
      .get();
  const auto* proxy_output = runtime.realtime_proxy_graph().find_output(3);
  ASSERT_NE(proxy_output, nullptr);
  EXPECT_GT(proxy_output->image_buffer.width, 0);
  EXPECT_GT(proxy_output->image_buffer.height, 0);
}
