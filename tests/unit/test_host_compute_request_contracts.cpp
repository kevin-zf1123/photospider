#include <gtest/gtest.h>

#include <type_traits>

#include "photospider/host/compute_request.hpp"

namespace ps {
namespace {

static_assert(std::is_aggregate_v<HostComputeExecutionOptions>,
              "Host compute execution options must remain an aggregate");

/**
 * @brief Proves legacy two-boolean aggregate initialization keeps its meaning.
 *
 * @return Nothing; GoogleTest records any field-order regression.
 * @throws Nothing.
 * @note Both direct and nested initializers intentionally provide only the
 *       historical `parallel, quiet` values. The appended optional Run cap
 *       must therefore remain absent.
 */
TEST(HostComputeRequestContracts,
     LegacyTwoBooleanAggregatePreservesParallelAndQuiet) {
  const HostComputeExecutionOptions direct{true, true};
  const HostComputeRequest nested{{}, {}, {}, {true, true}, {}, {}, {}};

  EXPECT_TRUE(direct.parallel);
  EXPECT_TRUE(direct.quiet);
  EXPECT_FALSE(direct.maximum_parallelism.has_value());
  EXPECT_TRUE(nested.execution.parallel);
  EXPECT_TRUE(nested.execution.quiet);
  EXPECT_FALSE(nested.execution.maximum_parallelism.has_value());
}

}  // namespace
}  // namespace ps
