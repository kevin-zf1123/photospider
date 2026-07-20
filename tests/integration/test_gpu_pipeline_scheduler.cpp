/**
 * @file test_gpu_pipeline_scheduler.cpp
 * @brief M3.5 里程碑测试：GPU Pipeline 调度器和异构调度
 *
 * 验收标准：
 * - 支持 HP 走 GPU、RT 走 CPU 的异构调度
 * - 支持 RT 和 HP 同时运行，RT 优先级高于 HP
 * - 多设备算子实现可正确路由
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "compute/compute_service.hpp"
#include "compute/execution_service.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_cache_service.hpp"
#include "graph/graph_traversal_service.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "runtime/graph_runtime.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "runtime/resource_ledger.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"
#include "scheduler/gpu_pipeline_scheduler.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/serial_debug_scheduler.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"

namespace ps {
namespace {

// =============================================================================
// 测试辅助类和函数
// =============================================================================

// 模拟 GPU 算子（用于测试设备路由）
NodeOutput mock_gpu_op(const Node& node,
                       const std::vector<const NodeOutput*>& inputs) {
  NodeOutput output;
  output.debug.compute_device = "GPU_METAL";

  // 模拟 GPU 计算延迟
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // 创建一个简单的输出图像
  output.image_buffer.width = 64;
  output.image_buffer.height = 64;
  output.image_buffer.channels = 3;

  return output;
}

// 模拟 CPU 算子（快速响应）
NodeOutput mock_cpu_op(const Node& node,
                       const std::vector<const NodeOutput*>& inputs) {
  NodeOutput output;
  output.debug.compute_device = "CPU";

  // 模拟 CPU 计算延迟（比 GPU 更快）
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  // 创建输出
  output.image_buffer.width = 64;
  output.image_buffer.height = 64;
  output.image_buffer.channels = 3;

  return output;
}

// 模拟 Tiled CPU 算子（用于 RT 路径）
void mock_tiled_cpu_op(const Node& node, const OutputTile& output_tile,
                       const std::vector<InputTile>& input_tiles) {
  // 模拟分块计算
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

/**
 * @brief Test scheduler that exposes a GPU device to production compute.
 *
 * The scheduler composes `SerialDebugScheduler` through the public
 * `IScheduler` contract and reports GPU_METAL as available. This isolates
 * operation selection from real Metal hardware so the test can assert
 * `ComputeTaskDispatcher` uses device-aware registry resolution without
 * extending the final built-in scheduler implementation.
 *
 * @throws Forwarded serial lifecycle, allocation, synchronization, and task
 * exceptions according to each public scheduler operation.
 * @note The wrapper owns its serial delegate and borrows host/task state only
 * for the intervals defined by `IScheduler` and `SchedulerTaskRuntime`.
 */
class GpuAvailableSerialScheduler final : public IScheduler {
 public:
  /**
   * @brief Constructs a detached, stopped GPU-capable serial test scheduler.
   * @throws std::system_error if delegate synchronization cannot initialize.
   * @note Construction acquires no graph, host, worker, or task ownership.
   */
  GpuAvailableSerialScheduler() = default;

  /**
   * @brief Releases the owned serial delegate after runtime cleanup.
   * @throws Nothing under the built-in serial scheduler destructor contract.
   * @note GraphRuntime orders shutdown and detach before destroying this test
   * scheduler; the delegate remains the sole lifecycle-state owner.
   */
  ~GpuAvailableSerialScheduler() override = default;

  /**
   * @brief Attaches the owned serial delegate to a borrowed host context.
   * @param host Host context that outlives shutdown and detach.
   * @return Nothing.
   * @throws Nothing.
   * @note Host ownership is not transferred to the wrapper or delegate.
   */
  void attach(SchedulerHostContext& host) noexcept override {
    delegate_.attach(host);
  }

  /**
   * @brief Detaches the owned serial delegate from its borrowed host context.
   * @return Nothing.
   * @throws Nothing.
   * @note The caller must shut down the scheduler before this operation.
   */
  void detach() noexcept override { delegate_.detach(); }

  /**
   * @brief Starts inline serial task admission on the delegate.
   * @return Nothing.
   * @throws Nothing.
   * @note No worker or GPU resource is created; GPU is a routing label only.
   */
  void start() noexcept override { delegate_.start(); }

  /**
   * @brief Stops inline task admission on the delegate.
   * @return Nothing.
   * @throws Nothing.
   * @note Shutdown preserves the borrowed host until `detach()` is called.
   */
  void shutdown() noexcept override { delegate_.shutdown(); }

  /**
   * @brief Returns the delegated serial scheduler identity.
   * @return Owned scheduler name.
   * @throws std::bad_alloc if result allocation fails.
   * @note Identity is diagnostic; the wrapper changes only device capability.
   */
  std::string name() const override { return delegate_.name(); }

  /**
   * @brief Returns the delegated serial scheduler statistics snapshot.
   * @return Owned human-readable statistics text.
   * @throws std::bad_alloc if result allocation fails.
   * @note Statistics are produced from the delegate's lifecycle and counters.
   */
  std::string get_stats() const override { return delegate_.get_stats(); }

  /**
   * @brief Reports the delegated serial scheduler lifecycle state.
   * @return True after start and before shutdown.
   * @throws Nothing.
   * @note The result is an observational snapshot, not synchronization.
   */
  bool is_running() const noexcept override { return delegate_.is_running(); }

  /**
   * @brief Reports CPU and GPU_METAL as available devices.
   * @return Device list used by TaskSubmissionPlan for op selection.
   * @throws std::bad_alloc if vector allocation fails.
   * @note GPU_METAL is a deterministic routing capability for this test; work
   * still executes inline through the owned serial delegate.
   */
  std::vector<Device> available_devices() const override {
    return {Device::CPU, Device::GPU_METAL};
  }

  /**
   * @brief Delegates an initial borrowed-handle batch for inline execution.
   * @param handles Dispatcher-owned handles borrowed for this call.
   * @param total_task_count Complete logical task count for the batch.
   * @param priority Scheduler priority label preserved by the delegate.
   * @return Nothing.
   * @throws std::logic_error if the delegated scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than
   *         the number of valid handles.
   * @throws std::system_error if delegated synchronization fails.
   * @note Handle exceptions are captured and rethrown unchanged by
   *       `wait_for_completion()`; borrowing intervals remain unchanged.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    delegate_.submit_initial_task_handles(std::move(handles), total_task_count,
                                          priority);
  }

  /**
   * @brief Delegates worker-released borrowed handles for inline execution.
   * @param handles Newly ready dispatcher-owned handles.
   * @param priority Scheduler priority label preserved by the delegate.
   * @return Nothing.
   * @throws std::system_error if delegated synchronization fails.
   * @note Handle exceptions are captured and rethrown unchanged by
   *       `wait_for_completion()`; the delegate retains no handle beyond its
   *       active completion fence.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    delegate_.submit_ready_task_handles_from_worker(std::move(handles),
                                                    priority);
  }

  /**
   * @brief Delegates one owned any-thread callback for inline execution.
   * @param task Callback whose state moves into the delegate call.
   * @param priority Scheduler priority label preserved by the delegate.
   * @param epoch Optional scheduler batch epoch.
   * @return Nothing.
   * @throws std::system_error if delegated synchronization fails.
   * @note Callback exceptions are captured and rethrown unchanged by
   *       `wait_for_completion()`; public callback ownership is preserved.
   */
  void submit_ready_task_any_thread(
      SchedulerTaskRuntime::Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) override {
    delegate_.submit_ready_task_any_thread(std::move(task), priority, epoch);
  }

  /**
   * @brief Waits for delegated batch completion.
   * @return Nothing after successful completion or shutdown.
   * @throws The delegate's exact first task exception or synchronization error.
   * @note Return or throw ends the delegate's borrowed-handle interval.
   */
  void wait_for_completion() override { delegate_.wait_for_completion(); }

  /**
   * @brief Publishes an exception into delegated first-exception transport.
   * @param error Exception identity to retain; null is ignored.
   * @return Nothing.
   * @throws Delegate synchronization errors.
   * @note The first accepted non-null exception remains authoritative.
   */
  void set_exception(std::exception_ptr error) override {
    delegate_.set_exception(std::move(error));
  }

  /**
   * @brief Adds dynamically discovered work to delegated completion accounting.
   * @param delta Positive logical task-count increment.
   * @return Nothing.
   * @throws Delegate validation, overflow, or synchronization errors.
   * @note Nonpositive values follow `SerialDebugScheduler` semantics.
   */
  void inc_tasks_to_complete(int delta) override {
    delegate_.inc_tasks_to_complete(delta);
  }

  /**
   * @brief Retires one logical task from delegated completion accounting.
   * @return Nothing.
   * @throws Delegate synchronization errors.
   * @note The serial delegate preserves a zero completion floor.
   */
  void dec_tasks_to_complete() override { delegate_.dec_tasks_to_complete(); }

  /**
   * @brief Publishes one trace through the delegated host context.
   * @param action Stable scheduler trace action.
   * @param node_id Backend node identifier, or -1 when unavailable.
   * @return Nothing.
   * @throws Nothing.
   * @note Worker and epoch attribution remain owned by the serial delegate.
   */
  void log_event(SchedulerTraceAction action, int node_id) noexcept override {
    delegate_.log_event(action, node_id);
  }

 private:
  /**
   * @brief Owned final serial scheduler that supplies all execution semantics.
   * @note The wrapper forwards every lifecycle and dispatch operation to this
   * one delegate; only `available_devices()` is implemented independently.
   */
  SerialDebugScheduler delegate_;
};

/**
 * @brief CPU marker op used to prove production routing does not pick CPU.
 *
 * @param node Node being computed.
 * @param inputs Resolved image inputs, unused for this source-like test op.
 * @return Output tagged as CPU.
 * @throws Nothing directly.
 */
NodeOutput production_cpu_marker_op(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;
  NodeOutput output;
  output.image_buffer.width = 8;
  output.image_buffer.height = 8;
  output.image_buffer.channels = 1;
  output.image_buffer.type = DataType::FLOAT32;
  output.image_buffer.device = Device::CPU;
  return output;
}

/**
 * @brief GPU marker op used to prove production routing selects GPU.
 *
 * @param node Node being computed.
 * @param inputs Resolved image inputs, unused for this source-like test op.
 * @return Output tagged as GPU_METAL.
 * @throws Nothing directly.
 */
NodeOutput production_gpu_marker_op(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;
  NodeOutput output;
  output.image_buffer.width = 8;
  output.image_buffer.height = 8;
  output.image_buffer.channels = 1;
  output.image_buffer.type = DataType::FLOAT32;
  output.image_buffer.device = Device::GPU_METAL;
  return output;
}

/**
 * @brief CPU tiled marker used to catch incorrect fallback selection.
 *
 * @param node Node being computed, unused by the marker.
 * @param output_tile Destination tile whose buffer is tagged as CPU.
 * @param input_tiles Resolved input tiles, unused for generator tests.
 * @throws OpenCV exceptions if the destination tile cannot be viewed.
 * @note The callback writes a visible pixel value and device tag so production
 * compute tests can identify which compatible tiled implementation ran.
 */
void production_cpu_tiled_marker_op(const Node& node,
                                    const OutputTile& output_tile,
                                    const std::vector<InputTile>& input_tiles) {
  (void)node;
  (void)input_tiles;
  output_tile.buffer->device = Device::CPU;
  toCvMat(output_tile).setTo(1.0f);
}

/**
 * @brief GPU tiled marker used to prove fallback preserves HP device priority.
 *
 * @param node Node being computed, unused by the marker.
 * @param output_tile Destination tile whose buffer is tagged as GPU_METAL.
 * @param input_tiles Resolved input tiles, unused for generator tests.
 * @throws std::runtime_error if host-visible output storage cannot be mapped.
 * @throws OpenCV exceptions if the destination tile cannot be viewed.
 * @note The callback emulates GPU output over host-visible test storage. Each
 * serial tile temporarily restores the CPU mapping label required by OpenCV,
 * writes its ROI, and then marks the shared result as GPU_METAL. Restoring the
 * label on every call is required because all output tiles share one buffer.
 */
void production_gpu_tiled_marker_op(const Node& node,
                                    const OutputTile& output_tile,
                                    const std::vector<InputTile>& input_tiles) {
  (void)node;
  (void)input_tiles;
  output_tile.buffer->device = Device::CPU;
  toCvMat(output_tile).setTo(2.0f);
  output_tile.buffer->device = Device::GPU_METAL;
}

class GpuPipelineSchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& registry = OpRegistry::instance();

    // 注册测试用的多设备算子实现
    OpMetadata gpu_meta;
    gpu_meta.device_preference = Device::GPU_METAL;
    gpu_meta.cost_score = 50;  // GPU 成本更低（HP 优先）

    OpMetadata cpu_meta;
    cpu_meta.device_preference = Device::CPU;
    cpu_meta.cost_score = 100;

    OpMetadata rt_cpu_meta;
    rt_cpu_meta.device_preference = Device::CPU;
    rt_cpu_meta.cost_score = 80;
    rt_cpu_meta.tile_preference = TileSizePreference::MICRO;

    // 注册 GPU 版本
    registry.register_impl("gpu_test", "mock_op", Device::GPU_METAL,
                           mock_gpu_op, gpu_meta);

    // 注册 CPU 版本
    registry.register_impl("gpu_test", "mock_op", Device::CPU, mock_cpu_op,
                           cpu_meta);

    // 注册 RT Tiled CPU 版本
    registry.register_impl("gpu_test", "mock_op_rt", Device::CPU,
                           mock_tiled_cpu_op, rt_cpu_meta);
  }

  void TearDown() override {
    // 测试清理
  }
};

// =============================================================================
// 基本功能测试
// =============================================================================

// 测试：调度器工厂可以创建 GPU Pipeline 调度器
TEST_F(GpuPipelineSchedulerTest, FactoryCanCreateScheduler) {
  const auto plan = SchedulerFactory::plan("gpu_pipeline");
  ASSERT_TRUE(plan.has_value());
  ResourceLedger ledger(ResourceVector{plan->reservation_slots()});
  auto reservation =
      ledger.try_reserve(ResourceVector{plan->reservation_slots()});
  ASSERT_TRUE(reservation.has_value());
  auto scheduler = SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(scheduler->name(), "GpuPipelineScheduler");
}

// 测试：调度器工厂支持 "heterogeneous" 别名
TEST_F(GpuPipelineSchedulerTest, FactorySupportsHeterogeneousAlias) {
  const auto plan = SchedulerFactory::plan("heterogeneous");
  ASSERT_TRUE(plan.has_value());
  ResourceLedger ledger(ResourceVector{plan->reservation_slots()});
  auto reservation =
      ledger.try_reserve(ResourceVector{plan->reservation_slots()});
  ASSERT_TRUE(reservation.has_value());
  auto scheduler = SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(scheduler->name(), "GpuPipelineScheduler");
}

// 测试：支持的调度器类型列表包含新类型
TEST_F(GpuPipelineSchedulerTest, SupportedTypesIncludeNewSchedulers) {
  auto types = SchedulerFactory::supported_types();
  EXPECT_TRUE(std::find(types.begin(), types.end(), "gpu_pipeline") !=
              types.end());
  EXPECT_TRUE(std::find(types.begin(), types.end(), "heterogeneous") !=
              types.end());
}

// 测试：调度器可以正常启动和关闭
TEST_F(GpuPipelineSchedulerTest, StartAndShutdown) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 1;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);

  EXPECT_FALSE(scheduler->is_running());

  scheduler->start();
  EXPECT_TRUE(scheduler->is_running());

  scheduler->shutdown();
  EXPECT_FALSE(scheduler->is_running());
}

// 测试：调度器统计信息
TEST_F(GpuPipelineSchedulerTest, GetStats) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();

  std::string stats = scheduler->get_stats();
  EXPECT_FALSE(stats.empty());
  EXPECT_NE(stats.find("CPU Workers"), std::string::npos);

  scheduler->shutdown();
}

// =============================================================================
// 设备路由测试
// =============================================================================

// 测试：HP 模式下 GPU 算子优先
TEST_F(GpuPipelineSchedulerTest, HPPrefersGPU) {
  auto& registry = OpRegistry::instance();

  // 模拟有 GPU 可用
  std::vector<Device> available = {Device::CPU, Device::GPU_METAL};

  auto best = registry.select_best_implementation(
      "gpu_test", "mock_op", available, ComputeIntent::GlobalHighPrecision);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.device_preference, Device::GPU_METAL);
}

// 测试：RT 模式下 CPU Tiled 算子优先
TEST_F(GpuPipelineSchedulerTest, RTPreferstiledCPU) {
  auto& registry = OpRegistry::instance();

  std::vector<Device> available = {Device::CPU, Device::GPU_METAL};

  auto best = registry.select_best_implementation(
      "gpu_test", "mock_op_rt", available, ComputeIntent::RealTimeUpdate);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.device_preference, Device::CPU);
  EXPECT_TRUE(best->is_tiled());
}

// 测试：仅 CPU 可用时 HP 回退到 CPU
TEST_F(GpuPipelineSchedulerTest, HPFallbackToCPU) {
  auto& registry = OpRegistry::instance();

  // 仅 CPU 可用
  std::vector<Device> available = {Device::CPU};

  auto best = registry.select_best_implementation(
      "gpu_test", "mock_op", available, ComputeIntent::GlobalHighPrecision);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.device_preference, Device::CPU);
}

TEST_F(GpuPipelineSchedulerTest, ProductionComputeUsesDeviceImplementation) {
  constexpr const char* kType = "gpu_test";
  constexpr const char* kSubtype = "production_route";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata gpu_meta;
  gpu_meta.device_preference = Device::GPU_METAL;
  gpu_meta.cost_score = 10;
  registry.register_impl(kType, kSubtype, Device::GPU_METAL,
                         production_gpu_marker_op, gpu_meta);

  OpMetadata cpu_meta;
  cpu_meta.device_preference = Device::CPU;
  cpu_meta.cost_score = 100;
  registry.register_impl(kType, kSubtype, Device::CPU, production_cpu_marker_op,
                         cpu_meta);

  GraphRuntime::Info info;
  info.name = "device-routing-production";
  info.root = "build/test-device-routing-production";
  info.cache_root = "build/test-device-routing-production/cache";
  GraphRuntime runtime(info);
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::make_unique<GpuAvailableSerialScheduler>());
  runtime.start();

  Node node;
  node.id = 1;
  node.name = "device_route";
  node.type = kType;
  node.subtype = kSubtype;
  node.parameters["width"] = 8;
  node.parameters["height"] = 8;
  runtime.model().add_node(node);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, runtime.event_service(),
                         execution_service);
  ComputeService::Request request;
  request.node_id = node.id;
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  NodeOutput& result =
      compute.compute_parallel(runtime.model(), runtime, request);
  EXPECT_EQ(result.image_buffer.device, Device::GPU_METAL);
  EXPECT_EQ(result.image_buffer.width, 8);
  EXPECT_EQ(result.image_buffer.height, 8);

  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

TEST_F(GpuPipelineSchedulerTest,
       ProductionTiledFallbackPreservesHpDevicePriority) {
  constexpr const char* kType = "image_generator";
  constexpr const char* kSubtype = "tiled_device_priority_fallback";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata gpu_monolithic_meta;
  gpu_monolithic_meta.device_preference = Device::GPU_METAL;
  gpu_monolithic_meta.cost_score = 1;
  registry.register_impl(kType, kSubtype, Device::GPU_METAL,
                         production_gpu_marker_op, gpu_monolithic_meta);

  OpMetadata cpu_tiled_meta;
  cpu_tiled_meta.device_preference = Device::CPU;
  cpu_tiled_meta.cost_score = 5;
  cpu_tiled_meta.tile_preference = TileSizePreference::MICRO;
  registry.register_impl(kType, kSubtype, Device::CPU,
                         production_cpu_tiled_marker_op, cpu_tiled_meta);

  OpMetadata gpu_tiled_meta;
  gpu_tiled_meta.device_preference = Device::GPU_METAL;
  gpu_tiled_meta.cost_score = 100;
  gpu_tiled_meta.tile_preference = TileSizePreference::MICRO;
  registry.register_impl(kType, kSubtype, Device::GPU_METAL,
                         production_gpu_tiled_marker_op, gpu_tiled_meta);

  GraphRuntime::Info info;
  info.name = "tiled-device-priority-fallback";
  info.root = "build/test-tiled-device-priority-fallback";
  info.cache_root = "build/test-tiled-device-priority-fallback/cache";
  GraphRuntime runtime(info);
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::make_unique<GpuAvailableSerialScheduler>());
  runtime.start();

  Node node;
  node.id = 1;
  node.name = "tiled_device_route";
  node.type = kType;
  node.subtype = kSubtype;
  node.parameters["width"] = 32;
  node.parameters["height"] = 32;
  runtime.model().add_node(node);

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, runtime.event_service(),
                         execution_service);
  ComputeService::Request request;
  request.node_id = node.id;
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;

  NodeOutput& result =
      compute.compute_parallel(runtime.model(), runtime, request);
  ASSERT_TRUE(runtime.model().last_compute_plan.has_value());
  const auto& tasks = runtime.model().last_compute_plan->task_graph.tasks;
  EXPECT_TRUE(std::any_of(tasks.begin(), tasks.end(), [](const auto& task) {
    return task.kind == ps::compute::PlannedTaskKind::Tile;
  }));
  EXPECT_EQ(result.image_buffer.device, Device::GPU_METAL);
  EXPECT_EQ(result.debug.compute_device, "GPU_METAL");
  ASSERT_TRUE(result.image_buffer.data);
  EXPECT_EQ(*static_cast<const float*>(result.image_buffer.data.get()), 2.0f);

  runtime.stop();
  registry.unregister_key(make_key(kType, kSubtype));
}

TEST_F(GpuPipelineSchedulerTest, AvailableDevicesHonorGpuDispatchConfig) {
  GraphRuntime::Info info;
  info.name = "available-device-config";
  info.root = "build/test-available-device-config";
  info.cache_root = "build/test-available-device-config/cache";
  GraphRuntime runtime(info);
  if (runtime.get_metal_device() == nullptr) {
    GTEST_SKIP() << "Metal device is unavailable on this host.";
  }

  GpuPipelineScheduler::Config disabled_workers;
  disabled_workers.gpu_workers = 0;
  disabled_workers.prefer_gpu_for_hp = true;
  GpuPipelineScheduler no_gpu_workers(disabled_workers);
  no_gpu_workers.attach(runtime);
  EXPECT_EQ(no_gpu_workers.available_devices(),
            std::vector<Device>{Device::CPU});

  GpuPipelineScheduler::Config cpu_hp_config;
  cpu_hp_config.gpu_workers = 1;
  cpu_hp_config.prefer_gpu_for_hp = false;
  GpuPipelineScheduler cpu_hp_scheduler(cpu_hp_config);
  cpu_hp_scheduler.attach(runtime);
  EXPECT_EQ(cpu_hp_scheduler.available_devices(),
            std::vector<Device>{Device::CPU});

  GpuPipelineScheduler::Config enabled_config;
  enabled_config.gpu_workers = 1;
  enabled_config.prefer_gpu_for_hp = true;
  GpuPipelineScheduler gpu_enabled_scheduler(enabled_config);
  gpu_enabled_scheduler.attach(runtime);
  EXPECT_EQ(gpu_enabled_scheduler.available_devices(),
            (std::vector<Device>{Device::CPU, Device::GPU_METAL}));
}

// =============================================================================
// 并发调度测试
// =============================================================================

// 测试：RT 和 HP 任务可以同时提交
TEST_F(GpuPipelineSchedulerTest, ConcurrentRTAndHP) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 1;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();

  std::atomic<int> rt_count{0};
  std::atomic<int> hp_count{0};

  // 提交多个 RT 任务
  for (int i = 0; i < 5; ++i) {
    scheduler->submit_rt_task([&rt_count]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      rt_count.fetch_add(1);
    });
  }

  // 提交多个 HP 任务
  for (int i = 0; i < 5; ++i) {
    scheduler->submit_hp_task([&hp_count]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      hp_count.fetch_add(1);
    });
  }

  // 等待一段时间让任务完成
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  scheduler->shutdown();

  EXPECT_EQ(rt_count.load(), 5);
  EXPECT_EQ(hp_count.load(), 5);
}

// 测试：RT 任务优先级高于 HP（抢占）
TEST_F(GpuPipelineSchedulerTest, RTPriorityOverHP) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;  // 单线程更容易观察优先级效果
  config.gpu_workers = 0;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();

  std::vector<std::string> execution_order;
  std::mutex order_mutex;

  // 先提交 HP 任务
  for (int i = 0; i < 3; ++i) {
    scheduler->submit_hp_task([&execution_order, &order_mutex, i]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::lock_guard<std::mutex> lock(order_mutex);
      execution_order.push_back("HP" + std::to_string(i));
    });
  }

  // 稍后提交 RT 任务（应该被优先处理）
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  scheduler->submit_rt_task([&execution_order, &order_mutex]() {
    std::lock_guard<std::mutex> lock(order_mutex);
    execution_order.push_back("RT0");
  });

  // 等待完成
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  scheduler->shutdown();

  // RT 任务应该在至少部分 HP 任务之前执行
  // 由于单线程，第一个 HP 可能已经开始执行，但 RT 应该在后续 HP 之前
  EXPECT_FALSE(execution_order.empty());

  // 检查 RT 不是最后一个执行的（说明优先级生效）
  if (execution_order.size() >= 2) {
    bool rt_not_last = (execution_order.back() != "RT0");
    // 或者 RT 是第一个/第二个执行的
    bool rt_early = false;
    for (size_t i = 0; i < std::min(size_t(2), execution_order.size()); ++i) {
      if (execution_order[i] == "RT0") {
        rt_early = true;
        break;
      }
    }
    EXPECT_TRUE(rt_not_last || rt_early);
  }
}

// =============================================================================
// Epoch 管理测试
// =============================================================================

// 测试：开始新 Epoch 会取消过期任务
TEST_F(GpuPipelineSchedulerTest, NewEpochCancelsStale) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();

  std::atomic<int> executed{0};
  std::promise<void> blocker_started;
  std::promise<void> release_blocker;
  auto blocker_started_future = blocker_started.get_future();
  auto release_blocker_future = release_blocker.get_future().share();

  // Keep the single worker busy so the stale task remains queued until epoch2
  // becomes active. Epoch cancellation is a queued-work guarantee; tasks that
  // have already started may finish.
  scheduler->submit_hp_task([&blocker_started, release_blocker_future]() {
    blocker_started.set_value();
    release_blocker_future.wait();
  });

  if (blocker_started_future.wait_for(std::chrono::seconds(1)) !=
      std::future_status::ready) {
    release_blocker.set_value();
    scheduler->shutdown();
    FAIL() << "Timed out waiting for the blocker task to start";
  }

  // 获取当前 epoch
  uint64_t epoch1 = scheduler->begin_new_epoch();

  // 提交使用旧 epoch 的任务；该任务仍在队列中，稍后应被取消。
  scheduler->submit_hp_task([&executed]() { executed.fetch_add(1); }, epoch1);

  // 开始新 epoch（应取消上面的任务）
  uint64_t epoch2 = scheduler->begin_new_epoch();
  EXPECT_GT(epoch2, epoch1);

  // 提交使用新 epoch 的任务
  std::promise<void> new_task_done;
  auto new_task_done_future = new_task_done.get_future();
  scheduler->submit_hp_task(
      [&executed, &new_task_done]() {
        executed.fetch_add(10);
        new_task_done.set_value();
      },
      epoch2);

  release_blocker.set_value();
  if (new_task_done_future.wait_for(std::chrono::seconds(1)) !=
      std::future_status::ready) {
    scheduler->shutdown();
    FAIL() << "Timed out waiting for the new epoch task to run";
  }
  scheduler->shutdown();

  // 旧任务应该被取消，只有新任务执行
  EXPECT_EQ(executed.load(), 10);
}

// =============================================================================
// 与 GraphRuntime 集成测试
// =============================================================================

TEST(GpuPipelineIntegrationTest, SchedulerWithRuntime) {
  using ps::ComputeIntent;
  using ps::GpuPipelineScheduler;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "gpu_pipeline_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);

  // 如果测试图不存在，跳过测试
  if (!loaded.has_value()) {
    GTEST_SKIP() << "Test graph not found, skipping integration test";
  }

  // 创建并设置 GPU Pipeline 调度器
  auto& runtime = testing::KernelTestAccess::runtime(kernel, graph_name);

  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.prefer_gpu_for_hp = true;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  if (endings->empty()) {
    GTEST_SKIP() << "No ending nodes found";
  }
  int node_id = (*endings)[0];

  Kernel::ComputeRequest hp_request;
  hp_request.name = graph_name;
  hp_request.node_id = node_id;
  hp_request.cache.precision = "int8";
  hp_request.cache.force_recache = true;
  hp_request.execution.parallel = true;
  bool success = svc.cmd_compute(hp_request);
  ASSERT_TRUE(success);
  const auto& result =
      runtime.model().node(node_id).cached_output_high_precision.value();

  // 验证计算完成
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}

// 测试：RT 和 HP 使用不同调度器同时运行
TEST(GpuPipelineIntegrationTest, DualSchedulerConcurrentExecution) {
  using ps::ComputeIntent;
  using ps::CpuWorkStealingScheduler;
  using ps::GpuPipelineScheduler;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "dual_scheduler_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);

  if (!loaded.has_value()) {
    GTEST_SKIP() << "Test graph not found";
  }

  auto& runtime = testing::KernelTestAccess::runtime(kernel, graph_name);

  // 为 HP 设置 GPU Pipeline 调度器
  GpuPipelineScheduler::Config gpu_config;
  gpu_config.cpu_workers = 2;
  gpu_config.prefer_gpu_for_hp = true;
  auto hp_scheduler = std::make_unique<GpuPipelineScheduler>(gpu_config);
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler));

  // 为 RT 设置 CPU Work-Stealing 调度器（高优先级）
  auto rt_scheduler = std::make_unique<CpuWorkStealingScheduler>(4);
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  if (!endings.has_value() || endings->empty()) {
    GTEST_SKIP() << "No ending nodes found";
  }
  int node_id = (*endings)[0];

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  compute::ExecutionService execution_service;
  ComputeService compute(traversal, cache, runtime.event_service(),
                         execution_service);

  ComputeService::Request rt_request;
  rt_request.node_id = node_id;
  rt_request.cache.precision = "int8";
  rt_request.cache.force_recache = true;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = PixelRect{0, 0, 64, 64};
  NodeOutput& rt_result =
      compute.compute_parallel(runtime.model(), runtime, rt_request);

  EXPECT_TRUE(
      runtime.model().node(node_id).cached_output_high_precision.has_value());
  EXPECT_TRUE(rt_result.image_buffer.width > 0 || !rt_result.data.empty());
}

// =============================================================================
// 错误处理测试
// =============================================================================

// 测试：异常传播
TEST_F(GpuPipelineSchedulerTest, ExceptionPropagation) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();

  // 提交一个会抛出异常的任务
  scheduler->submit_hp_task(
      []() { throw std::runtime_error("Test exception"); });

  // 等待异常被处理
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 调度器应该仍在运行（异常被捕获）
  EXPECT_TRUE(scheduler->is_running());

  scheduler->shutdown();
}

TEST_F(GpuPipelineSchedulerTest, StatsExposePlannedTaskRuntimeMetrics) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();

  std::string stats = scheduler->get_stats();

  EXPECT_NE(stats.find("Total scheduled"), std::string::npos);
  EXPECT_NE(stats.find("RT executed"), std::string::npos);
  EXPECT_NE(stats.find("HP CPU executed"), std::string::npos);

  scheduler->shutdown();
}

}  // namespace
}  // namespace ps
