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
  EXPECT_TRUE(routes.can_start("cpu", 3, 7U));
  EXPECT_TRUE(routes.can_start("gpu_pipeline", 2, 5U));

  EXPECT_TRUE(routes.commit_start("cpu"));
  EXPECT_TRUE(routes.commit_start("cpu"));
  EXPECT_TRUE(routes.commit_start("gpu_pipeline"));
  EXPECT_EQ(routes.in_flight("cpu"), 2U);
  EXPECT_EQ(routes.in_flight("gpu_pipeline"), 1U);
  EXPECT_FALSE(routes.drained());

  EXPECT_TRUE(routes.finish("cpu"));
  EXPECT_TRUE(routes.finish("gpu_pipeline"));
  EXPECT_TRUE(routes.finish("cpu"));
  EXPECT_TRUE(routes.drained());
  EXPECT_FALSE(routes.finish("cpu"));
}

/**
 * @brief Verifies serial-debug is worker-zero single-flight and reusable.
 * @throws Nothing from the exercised route transitions.
 */
TEST(PhysicalExecutionRoutes, SerialDebugIsSingleFlightAndReusable) {
  PhysicalExecutionRoutes routes;
  EXPECT_FALSE(routes.can_start("serial_debug", 1, 0U));
  EXPECT_FALSE(routes.can_start("serial_debug", 0, 1U));
  ASSERT_TRUE(routes.can_start("serial_debug", 0, 0U));
  ASSERT_TRUE(routes.commit_start("serial_debug"));
  EXPECT_FALSE(routes.can_start("serial_debug", 0, 0U));
  EXPECT_FALSE(routes.commit_start("serial_debug"));
  EXPECT_EQ(routes.in_flight("serial_debug"), 1U);

  ASSERT_TRUE(routes.finish("serial_debug"));
  EXPECT_TRUE(routes.can_start("serial_debug", 0, 0U));
  EXPECT_TRUE(routes.commit_start("serial_debug"));
  EXPECT_TRUE(routes.finish("serial_debug"));
  EXPECT_TRUE(routes.drained());
}

/**
 * @brief Verifies idempotent shutdown rejects starts but permits exact drain.
 * @throws Nothing from the exercised route transitions.
 */
TEST(PhysicalExecutionRoutes, ShutdownRejectsNewStartsAndDrainsCommittedWork) {
  PhysicalExecutionRoutes routes;
  ASSERT_TRUE(routes.commit_start("cpu"));
  ASSERT_TRUE(routes.commit_start("gpu_pipeline"));

  routes.begin_shutdown();
  routes.begin_shutdown();
  EXPECT_FALSE(routes.can_start("cpu", 0, 0U));
  EXPECT_FALSE(routes.can_start("gpu_pipeline", 0, 0U));
  EXPECT_FALSE(routes.can_start("serial_debug", 0, 0U));
  EXPECT_FALSE(routes.commit_start("cpu"));
  EXPECT_FALSE(routes.drained());

  EXPECT_TRUE(routes.finish("cpu"));
  EXPECT_TRUE(routes.finish("gpu_pipeline"));
  EXPECT_TRUE(routes.drained());
}

}  // namespace
}  // namespace ps::execution
