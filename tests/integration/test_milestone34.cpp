// Photospider M3.4 Kernel Integration Tests
// Tests for scheduler injection in load_graph and CLI scheduler commands

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "runtime/resource_ledger.hpp"
#include "scheduler/cpu_work_stealing_scheduler.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/serial_debug_scheduler.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"

// =============================================================================
// M3.4: SchedulerFactory Tests
// =============================================================================

TEST(M34_SchedulerFactory, CreateCpuWorkStealing) {
  const auto plan = ps::SchedulerFactory::plan("cpu_work_stealing");
  ASSERT_TRUE(plan.has_value());
  ps::ResourceLedger ledger(ps::ResourceVector{plan->reservation_slots()});
  auto reservation =
      ledger.try_reserve(ps::ResourceVector{plan->reservation_slots()});
  ASSERT_TRUE(reservation.has_value());
  auto scheduler = ps::SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);
  // Note: CpuWorkStealingScheduler::name() returns "CpuWorkStealingScheduler"
  EXPECT_EQ(scheduler->name(), "CpuWorkStealingScheduler");
}

TEST(M34_SchedulerFactory, CreateSerialDebug) {
  const auto plan = ps::SchedulerFactory::plan("serial_debug");
  ASSERT_TRUE(plan.has_value());
  ps::ResourceLedger ledger(ps::ResourceVector{});
  auto reservation = ledger.try_reserve(ps::ResourceVector{});
  ASSERT_TRUE(reservation.has_value());
  auto scheduler = ps::SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(scheduler->name(), "serial_debug");
}

TEST(M34_SchedulerFactory, PlanUnsupportedReturnsNullopt) {
  EXPECT_FALSE(ps::SchedulerFactory::plan("unknown_type").has_value());
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
  ps::Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto& config = kernel.get_scheduler_config();

  EXPECT_EQ(config.hp_type, "cpu_work_stealing");
  EXPECT_EQ(config.rt_type, "cpu_work_stealing");
  EXPECT_EQ(config.worker_count, 0);
}

TEST(M34_KernelScheduler, SetSchedulerConfig) {
  ps::Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();

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
  using ps::ComputeIntent;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_scheduler_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";

  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  // Verify HP scheduler was injected
  auto hp_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info.has_value());
  EXPECT_EQ(hp_info->first, "CpuWorkStealingScheduler");

  // Verify RT scheduler was injected
  auto rt_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(rt_info.has_value());
  EXPECT_EQ(rt_info->first, "CpuWorkStealingScheduler");
}

TEST(M34_KernelScheduler, CustomSchedulerConfigOnLoad) {
  using ps::ComputeIntent;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();

  // Set custom config before loading
  ps::Kernel::SchedulerConfig custom_config;
  custom_config.hp_type = "serial_debug";
  custom_config.rt_type = "serial_debug";
  kernel.set_scheduler_config(custom_config);

  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();

  const std::string graph_name = "m34_custom_scheduler_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";

  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  // Verify HP scheduler uses serial_debug
  auto hp_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info.has_value());
  EXPECT_EQ(hp_info->first, "serial_debug");

  // Verify RT scheduler uses serial_debug
  auto rt_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(rt_info.has_value());
  EXPECT_EQ(rt_info->first, "serial_debug");
}

TEST(M34_KernelScheduler, ReplaceScheduler) {
  using ps::ComputeIntent;
  using ps::GraphRuntime;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  Kernel::SchedulerConfig scheduler_config;
  scheduler_config.worker_count = 3;
  kernel.set_scheduler_config(scheduler_config);
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_replace_scheduler_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";

  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  // Initially CpuWorkStealingScheduler
  auto hp_info_before =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info_before.has_value());
  EXPECT_EQ(hp_info_before->first, "CpuWorkStealingScheduler");
  const auto initial_route =
      ps::testing::KernelTestAccess::runtime(kernel, graph_name)
          .get_scheduler_execution_route(ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(initial_route.domain,
            GraphRuntime::SchedulerExecutionRoute::Domain::ProcessCpuService);

  // Replace with serial_debug
  bool replaced = kernel.replace_scheduler(
      graph_name, ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_TRUE(replaced);

  // Verify it's now serial_debug
  auto hp_info_after =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info_after.has_value());
  EXPECT_EQ(hp_info_after->first, "serial_debug");
  const auto serial_route =
      ps::testing::KernelTestAccess::runtime(kernel, graph_name)
          .get_scheduler_execution_route(ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(serial_route.domain,
            GraphRuntime::SchedulerExecutionRoute::Domain::PerGraphScheduler);

  ASSERT_TRUE(kernel.replace_scheduler(
      graph_name, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing"));
  const auto restored_route =
      ps::testing::KernelTestAccess::runtime(kernel, graph_name)
          .get_scheduler_execution_route(ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(restored_route.domain,
            GraphRuntime::SchedulerExecutionRoute::Domain::ProcessCpuService);
}

TEST(M34_KernelScheduler, ReplaceSchedulerInvalidType) {
  using ps::ComputeIntent;
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_invalid_replace_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";

  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  // Try to replace with invalid type
  bool replaced = kernel.replace_scheduler(
      graph_name, ComputeIntent::GlobalHighPrecision, "invalid_type");
  EXPECT_FALSE(replaced);

  // Scheduler should remain unchanged
  auto hp_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_info.has_value());
  EXPECT_EQ(hp_info->first, "CpuWorkStealingScheduler");
}

TEST(M34_KernelScheduler, GetSchedulerInfoNonExistentGraph) {
  ps::Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();

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

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "m34_compute_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";

  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = node_id;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.execution.parallel = true;
  bool success = svc.cmd_compute(request);
  ASSERT_TRUE(success);
  const auto& result = ps::testing::KernelTestAccess::model(kernel, graph_name)
                           .node(node_id)
                           .cached_output_high_precision.value();

  // Verify computation completed
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}

TEST(M34_Integration, ComputeWithSerialScheduler) {
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();

  // Use serial scheduler for easier debugging/verification
  ps::Kernel::SchedulerConfig config;
  config.hp_type = "serial_debug";
  config.rt_type = "serial_debug";
  kernel.set_scheduler_config(config);

  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();

  const std::string graph_name = "m34_serial_compute_test";
  const std::string graph_path =
      "tests/fixtures/graphs/scheduler_test_parallel.yaml";

  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = node_id;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.execution.parallel = true;
  bool success = svc.cmd_compute(request);
  ASSERT_TRUE(success);
  const auto& result = ps::testing::KernelTestAccess::model(kernel, graph_name)
                           .node(node_id)
                           .cached_output_high_precision.value();

  // Verify computation completed
  EXPECT_TRUE(result.image_buffer.width > 0 || !result.data.empty());
}
