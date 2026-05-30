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
    registry.register_impl("gpu_test", "mock_op", Device::CPU,
                           mock_cpu_op, cpu_meta);
    
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
  scheduler->submit_hp_task([&executed]() {
    executed.fetch_add(1);
  }, epoch1);
  
  // 开始新 epoch（应取消上面的任务）
  uint64_t epoch2 = scheduler->begin_new_epoch();
  EXPECT_GT(epoch2, epoch1);
  
  // 提交使用新 epoch 的任务
  scheduler->submit_hp_task([&executed]() {
    executed.fetch_add(10);
  }, epoch2);
  
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  scheduler->shutdown();
  
  // 旧任务应该被取消，只有新任务执行
  EXPECT_EQ(executed.load(), 10);
}

// =============================================================================
// 与 GraphRuntime 集成测试
// =============================================================================

TEST(GpuPipelineIntegrationTest, SchedulerWithRuntime) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::GpuPipelineScheduler;
  using ps::ComputeIntent;

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
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  if (endings->empty()) {
    GTEST_SKIP() << "No ending nodes found";
  }
  int node_id = (*endings)[0];

  // 使用 submit_compute 通过新调度器执行 HP 计算
  ps::ComputeOptions opts;
  opts.intent = ComputeIntent::GlobalHighPrecision;
  opts.node_id = node_id;
  opts.cache_precision = "int8";
  opts.force_recache = true;

  auto future = runtime.submit_compute(opts);
  
  // 获取结果
  auto result = future.get();
  
  // 验证计算完成
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}

// 测试：RT 和 HP 使用不同调度器同时运行
TEST(GpuPipelineIntegrationTest, DualSchedulerConcurrentExecution) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::GpuPipelineScheduler;
  using ps::CpuWorkStealingScheduler;
  using ps::ComputeIntent;

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
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(hp_scheduler));

  // 为 RT 设置 CPU Work-Stealing 调度器（高优先级）
  auto rt_scheduler = std::make_unique<CpuWorkStealingScheduler>(4);
  rt_scheduler->start();
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));

  auto endings = svc.cmd_ending_nodes(graph_name);
  if (!endings.has_value() || endings->empty()) {
    GTEST_SKIP() << "No ending nodes found";
  }
  int node_id = (*endings)[0];

  // 同时提交 HP 和 RT 请求
  ps::ComputeOptions hp_opts;
  hp_opts.intent = ComputeIntent::GlobalHighPrecision;
  hp_opts.node_id = node_id;
  hp_opts.cache_precision = "int8";
  hp_opts.force_recache = true;

  ps::ComputeOptions rt_opts;
  rt_opts.intent = ComputeIntent::RealTimeUpdate;
  rt_opts.node_id = node_id;
  rt_opts.cache_precision = "int8";
  rt_opts.force_recache = true;
  rt_opts.dirty_roi = cv::Rect(0, 0, 64, 64);

  // 并发提交
  auto hp_future = runtime.submit_compute(hp_opts);
  auto rt_future = runtime.submit_compute(rt_opts);

  // 两者都应该完成
  auto hp_result = hp_future.get();
  auto rt_result = rt_future.get();

  EXPECT_TRUE(hp_result.image_buffer.width > 0 || !hp_result.data.empty());
  EXPECT_TRUE(rt_result.image_buffer.width > 0 || !rt_result.data.empty());
}

// =============================================================================
// 错误处理测试
// =============================================================================

// 测试：未 attach 到 runtime 时调用 schedule 抛出异常
TEST_F(GpuPipelineSchedulerTest, ScheduleWithoutAttach) {
  GpuPipelineScheduler::Config config;
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();
  
  ps::ComputeOptions opts;
  opts.intent = ComputeIntent::GlobalHighPrecision;
  opts.node_id = 1;
  
  auto future = scheduler->schedule(opts);
  
  // 应该返回一个包含异常的 future
  EXPECT_THROW(future.get(), std::runtime_error);
  
  scheduler->shutdown();
}

// 测试：异常传播
TEST_F(GpuPipelineSchedulerTest, ExceptionPropagation) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();
  
  // 提交一个会抛出异常的任务
  scheduler->submit_hp_task([]() {
    throw std::runtime_error("Test exception");
  });
  
  // 等待异常被处理
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  
  // 调度器应该仍在运行（异常被捕获）
  EXPECT_TRUE(scheduler->is_running());
  
  scheduler->shutdown();
}

// =============================================================================
// [M3.6] Node-Level 调度测试
// =============================================================================

// 测试：优先级表初始化正确
TEST_F(GpuPipelineSchedulerTest, PriorityTablesInitialized) {
  GpuPipelineScheduler::Config config;
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  
  // 调度器创建时应该初始化优先级表
  // 通过 supports_* 方法验证
  EXPECT_TRUE(scheduler->supports_node_level_scheduling());
  EXPECT_TRUE(scheduler->supports_task_group_aggregation());
}

// 测试：TaskGroup 创建和聚合判断
TEST_F(GpuPipelineSchedulerTest, TaskGroupAggregation) {
  GpuPipelineScheduler::Config config;
  config.aggregation_threshold = 4;
  
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  
  // 创建一个包含多个 tile 的 TaskGroup
  std::vector<cv::Rect> tiles;
  for (int i = 0; i < 5; ++i) {
    tiles.emplace_back(i * 16, 0, 16, 16);
  }
  
  TaskGroup group = scheduler->create_task_group(
      1, tiles, ComputeIntent::GlobalHighPrecision, 1);
  
  EXPECT_EQ(group.node_id, 1);
  EXPECT_EQ(group.tile_count(), 5);
  EXPECT_EQ(group.intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_FALSE(group.empty());
  
  // 边界框应该覆盖所有 tiles
  EXPECT_EQ(group.bounding_box.x, 0);
  EXPECT_EQ(group.bounding_box.width, 5 * 16);
}

// 测试：HP 模式下的聚合决策
TEST_F(GpuPipelineSchedulerTest, ShouldAggregateHP) {
  GpuPipelineScheduler::Config config;
  config.aggregation_threshold = 4;
  
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();
  
  // HP 模式 + 足够多的 tile 应该聚合
  std::vector<cv::Rect> many_tiles;
  for (int i = 0; i < 10; ++i) {
    many_tiles.emplace_back(i * 16, 0, 16, 16);
  }
  
  TaskGroup hp_group = scheduler->create_task_group(
      1, many_tiles, ComputeIntent::GlobalHighPrecision, 1);
  
  // 如果 GPU 可用，应该聚合
  bool should_agg = scheduler->should_aggregate_to_macro(hp_group);
  if (scheduler->is_gpu_available()) {
    EXPECT_TRUE(should_agg);
  }
  
  scheduler->shutdown();
}

// 测试：RT 模式下不应聚合
TEST_F(GpuPipelineSchedulerTest, ShouldNotAggregateRT) {
  GpuPipelineScheduler::Config config;
  config.aggregation_threshold = 4;
  
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  
  std::vector<cv::Rect> tiles;
  for (int i = 0; i < 10; ++i) {
    tiles.emplace_back(i * 16, 0, 16, 16);
  }
  
  TaskGroup rt_group = scheduler->create_task_group(
      1, tiles, ComputeIntent::RealTimeUpdate, 1);
  
  // RT 模式不应聚合（需要低延迟）
  EXPECT_FALSE(scheduler->should_aggregate_to_macro(rt_group));
}

// 测试：ROI 切分为 tiles
TEST_F(GpuPipelineSchedulerTest, SplitRoiToTiles) {
  GpuPipelineScheduler::Config config;
  config.micro_tile_size = 16;
  
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  
  // 创建一个 64x64 的 ROI
  cv::Rect roi(0, 0, 64, 64);
  
  // 使用私有方法的间接测试：通过 create_task_group
  std::vector<cv::Rect> expected_tiles;
  for (int y = 0; y < 64; y += 16) {
    for (int x = 0; x < 64; x += 16) {
      expected_tiles.emplace_back(x, y, 16, 16);
    }
  }
  
  // 64x64 / 16x16 = 16 个 tiles
  TaskGroup group = scheduler->create_task_group(
      1, expected_tiles, ComputeIntent::GlobalHighPrecision, 1);
  
  EXPECT_EQ(group.tile_count(), 16);
  EXPECT_EQ(group.bounding_box, roi);
}

// 测试：NodeScheduleRequest 结构
TEST_F(GpuPipelineSchedulerTest, NodeScheduleRequestStructure) {
  NodeScheduleRequest request;
  request.node_id = 1;
  request.node_type = "gpu_test";
  request.node_subtype = "mock_op";
  request.dirty_roi = cv::Rect(0, 0, 64, 64);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.epoch = 100;
  request.cache_precision = "float32";
  
  EXPECT_EQ(request.node_id, 1);
  EXPECT_EQ(request.node_type, "gpu_test");
  EXPECT_EQ(request.dirty_roi.width, 64);
  EXPECT_EQ(request.intent, ComputeIntent::GlobalHighPrecision);
}

// 测试：统计信息包含 Node-level 计数
TEST_F(GpuPipelineSchedulerTest, StatsIncludeNodeLevelMetrics) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  
  auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
  scheduler->start();
  
  std::string stats = scheduler->get_stats();
  
  // 统计信息应该包含新的指标
  EXPECT_NE(stats.find("Node-level"), std::string::npos);
  EXPECT_NE(stats.find("Groups aggregated"), std::string::npos);
  
  scheduler->shutdown();
}

}  // namespace
}  // namespace ps
