#pragma once

#include <cstddef>

#include "photospider/host/event_stream.hpp"

namespace graph_cli {

/**
 * @brief Maximum compute-event pages consumed by one CLI polling pass.
 *
 * The budget is the ceiling of the fixed production ring capacity divided by
 * the maximum public drain page. One pass can therefore consume the complete
 * ring state that existed without a concurrent producer, while a live producer
 * cannot make the pass chase pages indefinitely.
 *
 * @throws Nothing.
 * @note This is a private graph_cli policy constant, not installed Host ABI.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
inline constexpr std::size_t kComputeEventPollingPageBudget =
    (ps::kComputeEventRingCapacity + ps::kComputeEventDrainMaxLimit - 1) /
    ps::kComputeEventDrainMaxLimit;
// NOLINTEND

static_assert(kComputeEventPollingPageBudget == 8);

/**
 * @brief Runs one fixed-budget compute-event polling pass.
 *
 * @tparam FetchPage Callable returning a status-oriented batch result.
 * @tparam ConsumePage Callable accepting one successful batch value.
 * @param fetch_page Callable that destructively requests the next bounded page.
 * @param consume_page Callable that observes each successful page exactly once.
 * @return true when every attempted Host call succeeds, false at the first
 *         failed status.
 * @throws Any exception propagated by either callable or returned value
 *         construction, including `std::bad_alloc`.
 * @note The pass stops early when `has_more` is false and otherwise performs at
 *       most `kComputeEventPollingPageBudget` calls. A later polling pass
 *       rechecks events left behind by the fixed budget or published later.
 */
template <typename FetchPage, typename ConsumePage>
bool run_compute_event_polling_pass(FetchPage&& fetch_page,
                                    ConsumePage&& consume_page) {
  for (std::size_t page_index = 0; page_index < kComputeEventPollingPageBudget;
       ++page_index) {
    auto batch = fetch_page();
    if (!batch.status.ok) {
      return false;
    }
    const bool has_more = batch.value.has_more;
    consume_page(batch.value);
    if (!has_more) {
      return true;
    }
  }
  return true;
}

}  // namespace graph_cli
