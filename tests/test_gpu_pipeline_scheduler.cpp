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

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/interaction.hpp"
#include "kernel/kernel.hpp"
#include "kernel/scheduler/cpu_work_stealing_scheduler.hpp"
#include "kernel/scheduler/gpu_pipeline_scheduler.hpp"
#include "kernel/scheduler/scheduler_factory.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "ps_types.hpp"

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
void mock_tiled_cpu_op(const Node& node, const Tile& output_tile,
                       const std::vector<Tile>& input_tiles) {
  // 模拟分块计算
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
  auto scheduler = SchedulerFactory::create("gpu_pipeline");
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(scheduler->name(), "GpuPipelineScheduler");
}

// 测试：调度器工厂支持 "heterogeneous" 别名
TEST_F(GpuPipelineSchedulerTest, FactorySupportsHeterogeneousAlias) {
  auto scheduler = SchedulerFactory::create("heterogeneous");
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

  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->metadata.device_preference, Device::GPU_METAL);
}

// 测试：RT 模式下 CPU Tiled 算子优先
TEST_F(GpuPipelineSchedulerTest, RTPreferstiledCPU) {
  auto& registry = OpRegistry::instance();

  std::vector<Device> available = {Device::CPU, Device::GPU_METAL};

  auto best = registry.select_best_implementation(
      "gpu_test", "mock_op_rt", available, ComputeIntent::RealTimeUpdate);

  ASSERT_NE(best, nullptr);
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

  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->metadata.device_preference, Device::CPU);
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

  // 获取当前 epoch
  uint64_t epoch1 = scheduler->begin_new_epoch();

  // 提交使用旧 epoch 的任务
  scheduler->submit_hp_task([&executed]() { executed.fetch_add(1); }, epoch1);

  // 开始新 epoch（应取消上面的任务）
  uint64_t epoch2 = scheduler->begin_new_epoch();
  EXPECT_GT(epoch2, epoch1);

  // 提交使用新 epoch 的任务
  scheduler->submit_hp_task([&executed]() { executed.fetch_add(10); }, epoch2);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "gpu_pipeline_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);

  // 如果测试图不存在，跳过测试
  if (!loaded.has_value()) {
    GTEST_SKIP() << "Test graph not found, skipping integration test";
  }

  // 创建并设置 GPU Pipeline 调度器
  auto& runtime = kernel.runtime(graph_name);

  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.prefer_gpu_for_hp = true;
  config.force_cpu_for_rt = true;

  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  if (endings->empty()) {
    GTEST_SKIP() << "No ending nodes found";
  }
  int node_id = (*endings)[0];

  bool success = svc.cmd_compute(graph_name, node_id, "int8",
                                 /*force*/ true, /*timing*/ false,
                                 /*parallel*/ true);
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

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "dual_scheduler_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);

  if (!loaded.has_value()) {
    GTEST_SKIP() << "Test graph not found";
  }

  auto& runtime = kernel.runtime(graph_name);

  // 为 HP 设置 GPU Pipeline 调度器
  GpuPipelineScheduler::Config gpu_config;
  gpu_config.cpu_workers = 2;
  gpu_config.prefer_gpu_for_hp = true;
  auto hp_scheduler = std::make_unique<GpuPipelineScheduler>(gpu_config);
  hp_scheduler->start();
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler));

  // 为 RT 设置 CPU Work-Stealing 调度器（高优先级）
  auto rt_scheduler = std::make_unique<CpuWorkStealingScheduler>(4);
  rt_scheduler->start();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  if (!endings.has_value() || endings->empty()) {
    GTEST_SKIP() << "No ending nodes found";
  }
  int node_id = (*endings)[0];

  GraphTraversalService traversal;
  GraphCacheService cache;
  ComputeService compute(traversal, cache, runtime.event_service());

  NodeOutput& rt_result = compute.compute_parallel(
      runtime.model(), runtime, ComputeIntent::RealTimeUpdate, node_id, "int8",
      true, false, false, nullptr, cv::Rect(0, 0, 64, 64));

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
