#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "execution/physical_execution_routes.hpp"
#include "photospider/core/graph_error.hpp"

namespace ps::execution {
namespace {

/**
 * @brief Verifies the private execution vocabulary is exact and one-way.
 * @throws Standard allocation exceptions from copied discovery values.
 * @note Removed scheduler names and the heterogeneous alias must never regain
 * route visibility through this private owner.
 */
TEST(PhysicalExecutionRoutes, PublishesOnlyTheThreeCanonicalPrivateRoutes) {
  EXPECT_EQ(PhysicalExecutionRoutes::available_types(),
            (std::vector<std::string>{"cpu", "gpu_pipeline", "serial_debug"}));
  EXPECT_EQ(PhysicalExecutionRoutes::description("cpu"),
            "Process-owned CPU execution route.");
  EXPECT_EQ(PhysicalExecutionRoutes::description("gpu_pipeline"),
            "Host-owned GPU-pipeline execution route.");
  EXPECT_EQ(PhysicalExecutionRoutes::description("serial_debug"),
            "Deterministic single-callback debug execution route.");

  for (const std::string& removed :
       {"cpu_work_stealing", "heterogeneous", "scheduler"}) {
    EXPECT_FALSE(PhysicalExecutionRoutes::is_supported(removed));
    EXPECT_THROW(PhysicalExecutionRoutes::description(removed), GraphError);
  }
}

/**
 * @brief Verifies CPU and GPU-pipeline starts overlap and retire independently.
 * @throws Nothing from the exercised route transitions.
 */
TEST(PhysicalExecutionRoutes, TracksConcurrentCpuAndGpuPipelineStarts) {
  PhysicalExecutionRoutes routes;
  EXPECT_TRUE(
      routes.can_start("cpu", Device::CPU, PhysicalExecutionLane::Cpu, 3, 7U));
  EXPECT_TRUE(routes.can_start("gpu_pipeline", Device::CPU,
                               PhysicalExecutionLane::Cpu, 2, 5U));
  EXPECT_TRUE(routes.can_start("gpu_pipeline", Device::GPU_METAL,
                               PhysicalExecutionLane::Gpu, 3, 5U));
  EXPECT_FALSE(routes.can_start("cpu", Device::GPU_METAL,
                                PhysicalExecutionLane::Gpu, 3, 7U));
  EXPECT_FALSE(routes.can_start("gpu_pipeline", Device::GPU_METAL,
                                PhysicalExecutionLane::Cpu, 2, 5U));

  EXPECT_TRUE(routes.commit_start("cpu", Device::CPU));
  EXPECT_TRUE(routes.commit_start("cpu", Device::CPU));
  EXPECT_TRUE(routes.commit_start("gpu_pipeline", Device::CPU));
  EXPECT_TRUE(routes.commit_start("gpu_pipeline", Device::GPU_METAL));
  EXPECT_FALSE(routes.commit_start("gpu_pipeline", Device::GPU_METAL));
  EXPECT_EQ(routes.in_flight("cpu"), 2U);
  EXPECT_EQ(routes.in_flight("gpu_pipeline"), 2U);
  EXPECT_FALSE(routes.drained());

  EXPECT_TRUE(routes.finish("cpu", Device::CPU));
  EXPECT_TRUE(routes.finish("gpu_pipeline", Device::CPU));
  EXPECT_TRUE(routes.finish("gpu_pipeline", Device::GPU_METAL));
  EXPECT_TRUE(routes.finish("cpu", Device::CPU));
  EXPECT_TRUE(routes.drained());
  EXPECT_FALSE(routes.finish("cpu", Device::CPU));
}

/**
 * @brief Verifies serial-debug is worker-zero single-flight and reusable.
 * @throws Nothing from the exercised route transitions.
 */
TEST(PhysicalExecutionRoutes, SerialDebugIsSingleFlightAndReusable) {
  PhysicalExecutionRoutes routes;
  EXPECT_FALSE(routes.can_start("serial_debug", Device::CPU,
                                PhysicalExecutionLane::Cpu, 1, 0U));
  EXPECT_FALSE(routes.can_start("serial_debug", Device::CPU,
                                PhysicalExecutionLane::Cpu, 0, 1U));
  EXPECT_FALSE(routes.can_start("serial_debug", Device::GPU_METAL,
                                PhysicalExecutionLane::Gpu, 0, 0U));
  ASSERT_TRUE(routes.can_start("serial_debug", Device::CPU,
                               PhysicalExecutionLane::Cpu, 0, 0U));
  ASSERT_TRUE(routes.commit_start("serial_debug", Device::CPU));
  EXPECT_FALSE(routes.can_start("serial_debug", Device::CPU,
                                PhysicalExecutionLane::Cpu, 0, 0U));
  EXPECT_FALSE(routes.commit_start("serial_debug", Device::CPU));
  EXPECT_EQ(routes.in_flight("serial_debug"), 1U);

  ASSERT_TRUE(routes.finish("serial_debug", Device::CPU));
  EXPECT_TRUE(routes.can_start("serial_debug", Device::CPU,
                               PhysicalExecutionLane::Cpu, 0, 0U));
  EXPECT_TRUE(routes.commit_start("serial_debug", Device::CPU));
  EXPECT_TRUE(routes.finish("serial_debug", Device::CPU));
  EXPECT_TRUE(routes.drained());
}

/**
 * @brief Verifies idempotent shutdown rejects starts but permits exact drain.
 * @throws Nothing from the exercised route transitions.
 */
TEST(PhysicalExecutionRoutes, ShutdownRejectsNewStartsAndDrainsCommittedWork) {
  PhysicalExecutionRoutes routes;
  ASSERT_TRUE(routes.commit_start("cpu", Device::CPU));
  ASSERT_TRUE(routes.commit_start("gpu_pipeline", Device::GPU_METAL));

  routes.begin_shutdown();
  routes.begin_shutdown();
  EXPECT_FALSE(
      routes.can_start("cpu", Device::CPU, PhysicalExecutionLane::Cpu, 0, 0U));
  EXPECT_FALSE(routes.can_start("gpu_pipeline", Device::GPU_METAL,
                                PhysicalExecutionLane::Gpu, 0, 0U));
  EXPECT_FALSE(routes.can_start("serial_debug", Device::CPU,
                                PhysicalExecutionLane::Cpu, 0, 0U));
  EXPECT_FALSE(routes.commit_start("cpu", Device::CPU));
  EXPECT_FALSE(routes.drained());

  EXPECT_TRUE(routes.finish("cpu", Device::CPU));
  EXPECT_TRUE(routes.finish("gpu_pipeline", Device::GPU_METAL));
  EXPECT_TRUE(routes.drained());
}

}  // namespace
}  // namespace ps::execution
