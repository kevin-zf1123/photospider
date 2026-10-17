#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "plugin/failure_latch.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
int failure_latch_publication() {
  auto owner = std::make_shared<plugin_internal::FailureLatch>();
  auto latch = owner;
  owner.reset();
  std::atomic<bool> valid{true};
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      const Status candidate{t % 2 ? ErrorCode::Cancelled : ErrorCode::Internal,
                             t % 2 ? "cancelled winner" : "internal winner"};
      const auto recorded = latch->record(candidate);
      for (unsigned i = 0; i < 1000; ++i) {
        const auto code = latch->load();
        const auto snapshot = latch->snapshot();
        if (code == ErrorCode::Ok || code != recorded.code ||
            snapshot.code != code || snapshot.message != recorded.message ||
            snapshot.message != (code == ErrorCode::Cancelled
                                     ? "cancelled winner"
                                     : "internal winner"))
          valid = false;
      }
    });
  }
  for (auto& thread : threads)
    thread.join();
  PS_CHECK(valid);
  auto expected = ErrorCode::Ok;
  PS_CHECK(!latch->compare_exchange_strong(expected, ErrorCode::Cancelled));
  PS_CHECK(expected == latch->load());
  plugin_internal::FailureLatch success_record;
  PS_CHECK(success_record.record(Status::success()).code ==
           ErrorCode::Internal);
  PS_CHECK(success_record.load() == ErrorCode::Internal);
  plugin_internal::FailureLatch success_exchange;
  expected = ErrorCode::Ok;
  PS_CHECK(success_exchange.compare_exchange_strong(expected, ErrorCode::Ok));
  PS_CHECK(success_exchange.load() == ErrorCode::Internal);
  PS_CHECK(success_exchange.snapshot().code == ErrorCode::Internal);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(failure_latch_publication() == 0);
  return 0;
}
