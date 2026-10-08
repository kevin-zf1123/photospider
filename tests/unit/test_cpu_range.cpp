#include <cfenv>  // NOLINT(build/c++11)
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "execution/cpu_range.hpp"
#include "support/test_support.hpp"

namespace {
struct Coverage {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> blocks;
};
int record(void* user, std::uint64_t begin, std::uint64_t end,
           std::uint32_t slot) {
  if (slot != 0 || begin >= end || std::fegetround() != FE_TONEAREST)
    return 1;
  static_cast<Coverage*>(user)->blocks.emplace_back(begin, end);
  return 0;
}
}  // namespace

int main() {
  std::mutex mutex;
  std::condition_variable changed;
  ps::execution_internal::WaitingAdmission admission(1);
  ps::execution_internal::CpuRangeQueue queue(mutex, changed, 1, admission);
  ps::CancellationSource cancellation;
  Coverage coverage;
  const int before = std::fegetround();
  PS_CHECK(std::fesetround(FE_DOWNWARD) == 0);
  // Exact endpoint arithmetic independently of an enclosing Root's prior work.
  PS_CHECK(queue
               .run(UINT64_MAX, UINT64_MAX - 1, 1, record, &coverage,
                    cancellation.token(), nullptr, {})
               .ok());
  PS_CHECK(std::fegetround() == FE_DOWNWARD);
  PS_CHECK(std::fesetround(before) == 0);
  PS_CHECK(coverage.blocks.size() == 2);
  PS_CHECK(coverage.blocks[0].first == 0);
  PS_CHECK(coverage.blocks[0].second == UINT64_MAX - 1);
  PS_CHECK(coverage.blocks[1].first == UINT64_MAX - 1);
  PS_CHECK(coverage.blocks[1].second == UINT64_MAX);
  coverage.blocks.clear();
  PS_CHECK(queue
               .run(UINT64_MAX, UINT64_MAX, 0, record, &coverage,
                    cancellation.token(), nullptr, {})
               .ok());
  PS_CHECK(coverage.blocks.size() == 1);
  PS_CHECK(coverage.blocks[0].first == 0);
  PS_CHECK(coverage.blocks[0].second == UINT64_MAX);
  coverage.blocks.clear();
  PS_CHECK(
      queue.run(0, 1, 0, record, &coverage, cancellation.token(), nullptr, {})
          .ok());
  PS_CHECK(coverage.blocks.empty());
  return 0;
}
