// Photospider M3.4 Kernel Integration Tests
// Tests for scheduler injection in load_graph and CLI scheduler commands

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "kernel/interaction.hpp"
#include "kernel/kernel.hpp"
#include "kernel/scheduler/cpu_work_stealing_scheduler.hpp"
#include "kernel/scheduler/scheduler_factory.hpp"
#include "kernel/scheduler/serial_debug_scheduler.hpp"

// =============================================================================
// M3.4: SchedulerFactory Tests
// =============================================================================

TEST(M34_SchedulerFactory, CreateCpuWorkStealing) {
  auto scheduler = ps::SchedulerFactory::create("cpu_work_stealing");
  ASSERT_NE(scheduler, nullptr);
  // Note: CpuWorkStealingScheduler::name() returns "CpuWorkStealingScheduler"
  EXPECT_EQ(scheduler->name(), "CpuWorkStealingScheduler");
}

TEST(M34_SchedulerFactory, CreateSerialDebug) {
  auto scheduler = ps::SchedulerFactory::create("serial_debug");
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(scheduler->name(), "serial_debug");
}

TEST(M34_SchedulerFactory, CreateUnsupportedReturnsNull) {
  auto scheduler = ps::SchedulerFactory::create("unknown_type");
  EXPECT_EQ(scheduler, nullptr);
}

TEST(M34_SchedulerFactory, SupportedTypes) {
  auto types = ps::SchedulerFactory::supported_types();
  EXPECT_GE(types.size(), 2);
  EXPECT_NE(std::find(types.begin(), types.end(), "cpu_work_stealing"),
            types.end());
  EXPECT_NE(std::find(types.begin(), types.end(), "serial_debug"), types.end());
}

TEST(M34_SchedulerFactory, IsSupported) {
  EXPECT_TRUE(ps::SchedulerFactory::is_supported("cpu_work_stealing"));
  EXPECT_TRUE(ps::SchedulerFactory::is_supported("serial_debug"));
  EXPECT_FALSE(ps::SchedulerFactory::is_supported("unknown"));
}

// =============================================================================
// M3.4: Kernel Scheduler Injection Tests
// =============================================================================

TEST(M34_KernelScheduler, DefaultSchedulerConfig) {
  ps::Kernel kernel;
  const auto& config = kernel.get_scheduler_config();
  
  EXPECT_EQ(config.hp_type, "cpu_work_stealing");
  EXPECT_EQ(config.rt_type, "cpu_work_stealing");
  EXPECT_EQ(config.worker_count, 0);
}

TEST(M34_KernelScheduler, SetSchedulerConfig) {
  ps::Kernel kernel;
  
  ps::Kernel::SchedulerConfig custom_config;
  custom_config.hp_type = "serial_debug";
  custom_config.rt_type = "cpu_work_stealing";
  custom_config.worker_count = 4;
  
  kernel.set_scheduler_config(custom_config);
  
  const auto& config = kernel.get_scheduler_config();
  EXPECT_EQ(config.hp_type, "serial_debug");
  EXPECT_EQ(config.rt_type, "cpu_work_stealing");
  EXPECT_EQ(config.worker_count, 4);
}

TEST(M34_KernelScheduler, LoadGraphInjectsSchedulers) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::ComputeIntent;

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_scheduler_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());
  
  // Verify HP scheduler was injected
  auto hp_info = kernel.get_scheduler_info(graph_name, 
                                            ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info.has_value());
  EXPECT_EQ(hp_info->first, "CpuWorkStealingScheduler");
  
  // Verify RT scheduler was injected
  auto rt_info = kernel.get_scheduler_info(graph_name,
                                            ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(rt_info.has_value());
  EXPECT_EQ(rt_info->first, "CpuWorkStealingScheduler");
}

TEST(M34_KernelScheduler, CustomSchedulerConfigOnLoad) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::ComputeIntent;

  Kernel kernel;
  
  // Set custom config before loading
  ps::Kernel::SchedulerConfig custom_config;
  custom_config.hp_type = "serial_debug";
  custom_config.rt_type = "serial_debug";
  kernel.set_scheduler_config(custom_config);
  
  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  
  const std::string graph_name = "m34_custom_scheduler_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());
  
  // Verify HP scheduler uses serial_debug
  auto hp_info = kernel.get_scheduler_info(graph_name,
                                            ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info.has_value());
  EXPECT_EQ(hp_info->first, "serial_debug");
  
  // Verify RT scheduler uses serial_debug
  auto rt_info = kernel.get_scheduler_info(graph_name,
                                            ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(rt_info.has_value());
  EXPECT_EQ(rt_info->first, "serial_debug");
}

TEST(M34_KernelScheduler, ReplaceScheduler) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::ComputeIntent;

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_replace_scheduler_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());
  
  // Initially CpuWorkStealingScheduler
  auto hp_info_before = kernel.get_scheduler_info(graph_name,
                                                   ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info_before.has_value());
  EXPECT_EQ(hp_info_before->first, "CpuWorkStealingScheduler");
  
  // Replace with serial_debug
  bool replaced = kernel.replace_scheduler(graph_name,
                                           ComputeIntent::GlobalHighPrecision,
                                           "serial_debug");
  EXPECT_TRUE(replaced);
  
  // Verify it's now serial_debug
  auto hp_info_after = kernel.get_scheduler_info(graph_name,
                                                  ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info_after.has_value());
  EXPECT_EQ(hp_info_after->first, "serial_debug");
}

TEST(M34_KernelScheduler, ReplaceSchedulerInvalidType) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::ComputeIntent;

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_invalid_replace_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());
  
  // Try to replace with invalid type
  bool replaced = kernel.replace_scheduler(graph_name,
                                           ComputeIntent::GlobalHighPrecision,
                                           "invalid_type");
  EXPECT_FALSE(replaced);
  
  // Scheduler should remain unchanged
  auto hp_info = kernel.get_scheduler_info(graph_name,
                                            ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info.has_value());
  EXPECT_EQ(hp_info->first, "CpuWorkStealingScheduler");
}

TEST(M34_KernelScheduler, GetSchedulerInfoNonExistentGraph) {
  ps::Kernel kernel;
  
  auto info = kernel.get_scheduler_info("non_existent",
                                         ps::ComputeIntent::GlobalHighPrecision);
  EXPECT_FALSE(info.has_value());
}

// =============================================================================
// M3.4: SerialDebugScheduler Tests
// =============================================================================

TEST(M34_SerialDebugScheduler, BasicLifecycle) {
  ps::SerialDebugScheduler scheduler;
  
  EXPECT_FALSE(scheduler.is_running());
  EXPECT_EQ(scheduler.name(), "serial_debug");
  
  scheduler.start();
  EXPECT_TRUE(scheduler.is_running());
  
  scheduler.shutdown();
  EXPECT_FALSE(scheduler.is_running());
}

TEST(M34_SerialDebugScheduler, GetStats) {
  ps::SerialDebugScheduler scheduler;
  scheduler.start();
  
  std::string stats = scheduler.get_stats();
  EXPECT_FALSE(stats.empty());
  EXPECT_NE(stats.find("SerialDebugScheduler"), std::string::npos);
  
  scheduler.shutdown();
}

// =============================================================================
// M3.4: Integration Test - Compute with Injected Scheduler
// =============================================================================

TEST(M34_Integration, ComputeWithInjectedScheduler) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::ComputeIntent;
  using ps::ComputeOptions;

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_compute_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  auto& runtime = kernel.runtime(graph_name);
  
  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  // Use submit_compute through the injected scheduler
  ComputeOptions opts;
  opts.intent = ComputeIntent::GlobalHighPrecision;
  opts.node_id = node_id;
  opts.cache_precision = "int8";
  opts.force_recache = true;

  auto future = runtime.submit_compute(opts);
  
  // Get result
  auto result = future.get();
  
  // Verify computation completed
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}

TEST(M34_Integration, ComputeWithSerialScheduler) {
  using ps::InteractionService;
  using ps::Kernel;
  using ps::ComputeIntent;
  using ps::ComputeOptions;

  Kernel kernel;
  
  // Use serial scheduler for easier debugging/verification
  ps::Kernel::SchedulerConfig config;
  config.hp_type = "serial_debug";
  config.rt_type = "serial_debug";
  kernel.set_scheduler_config(config);
  
  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  
  const std::string graph_name = "m34_serial_compute_test";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  auto& runtime = kernel.runtime(graph_name);
  
  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  // Use submit_compute through the serial scheduler
  ComputeOptions opts;
  opts.intent = ComputeIntent::GlobalHighPrecision;
  opts.node_id = node_id;
  opts.cache_precision = "int8";
  opts.force_recache = true;

  auto future = runtime.submit_compute(opts);
  
  // Get result
  auto result = future.get();
  
  // Verify computation completed
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}
