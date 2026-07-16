#include <gtest/gtest.h>

#include <cstddef>

#include "graph_cli/compute_event_paging.hpp"
#include "photospider/core/result_types.hpp"
#include "photospider/host/event_stream.hpp"

namespace graph_cli {
namespace {

/**
 * @brief Creates one successful synthetic compute-event page.
 * @param has_more Whether the synthetic backend reports a following page.
 * @return Successful status-oriented batch result.
 * @throws Nothing.
 */
ps::Result<ps::ComputeEventBatch> successful_page(bool has_more) {
  ps::Result<ps::ComputeEventBatch> result;
  result.value.has_more = has_more;
  return result;
}

TEST(ComputeEventPollingPass, StopsAtFixedRingCoverageBudget) {
  std::size_t fetch_count = 0;
  std::size_t consume_count = 0;

  EXPECT_TRUE(run_compute_event_polling_pass(
      [&] {
        ++fetch_count;
        return successful_page(true);
      },
      [&](const ps::ComputeEventBatch&) { ++consume_count; }));

  EXPECT_EQ(kComputeEventPollingPageBudget, 8u);
  EXPECT_EQ(fetch_count, kComputeEventPollingPageBudget);
  EXPECT_EQ(consume_count, kComputeEventPollingPageBudget);
}

TEST(ComputeEventPollingPass, StopsEarlyWhenBackendHasNoMoreEvents) {
  std::size_t fetch_count = 0;
  std::size_t consume_count = 0;

  EXPECT_TRUE(run_compute_event_polling_pass(
      [&] {
        ++fetch_count;
        return successful_page(fetch_count < 3);
      },
      [&](const ps::ComputeEventBatch&) { ++consume_count; }));

  EXPECT_EQ(fetch_count, 3u);
  EXPECT_EQ(consume_count, 3u);
}

TEST(ComputeEventPollingPass, StopsBeforeConsumingFailedPage) {
  std::size_t fetch_count = 0;
  std::size_t consume_count = 0;

  EXPECT_FALSE(run_compute_event_polling_pass(
      [&] {
        ++fetch_count;
        auto result = successful_page(true);
        if (fetch_count == 2) {
          result.status.ok = false;
        }
        return result;
      },
      [&](const ps::ComputeEventBatch&) { ++consume_count; }));

  EXPECT_EQ(fetch_count, 2u);
  EXPECT_EQ(consume_count, 1u);
}

}  // namespace
}  // namespace graph_cli
