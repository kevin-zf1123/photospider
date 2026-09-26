#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "execution/memory_budget.hpp"
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
int on_demand_payload() {
  auto root = std::make_shared<ResourceBudget>(ResourceLimits{});
  auto budget = std::make_shared<execution_internal::MemoryBudget>(8192, root);
  auto observation = std::make_shared<execution_internal::MemoryObservation>();
  auto cached_reservation = budget->reserve(7000, {}, observation).take_value();
  auto cached = cached_reservation->allocator().allocate(7000).take_value();
  cached_reservation->seal();
  auto state_reservation = budget->reserve(256, {}, observation).take_value();
  auto state = state_reservation->allocator().allocate(256).take_value();
  state_reservation->seal();
  PS_CHECK(!budget->reserve(2000, {}, observation).ok());
  unsigned reclaimed = 0;
  BufferAllocator escaped;
  MutableBuffer output;
  {
    execution_internal::ScopedMemoryAdmission admission(
        [&](std::uint64_t bytes) {
          ++reclaimed;
          cached = {};
          return budget->reserve(bytes, {}, observation);
        });
    escaped = budget->on_demand_allocator(observation, admission.callback());
    const auto before = budget->live();
    auto scoped = escaped.limited(2000);
    PS_CHECK(budget->live() == before);  // A view allocates no output bytes.
    auto allocated = scoped.allocate(2000);
    PS_CHECK(allocated.ok());
    output = allocated.take_value();
    PS_CHECK(reclaimed == 1 && budget->live() == 2256);
    PS_CHECK(escaped.owns(*std::move(output).freeze()));
    PS_CHECK(budget->live() == 256);
    PS_CHECK(scoped.allocate(2001).status().reason ==
             FailureReason::CapacityLimit);
  }
  PS_CHECK(escaped.allocate(1).status().code == ErrorCode::OperationFailed);
  PS_CHECK(reclaimed == 1);  // No callback may access retired driver state.
  state = {};
  PS_CHECK(budget->live() == 0 && budget->available() == 8192);
  PS_CHECK(root->statistics().live[ResourceKind::Payload] == 0);
  NumericDiagnostics report;
  report.profile = CpuNumericProfile::Strict;
  report.implementation[0] = 'x';
  report.view_elements = UINT64_MAX;
  NumericDiagnostics merged;
  PS_CHECK(merge_numeric_diagnostics(&merged, report).ok());
  report.view_elements = 1;
  report.copied_elements = 1;
  PS_CHECK(merge_numeric_diagnostics(&merged, report).reason ==
           FailureReason::CapacityLimit);
  PS_CHECK(merged.view_elements == UINT64_MAX && merged.copied_elements == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(failure_latch_publication() == 0);
  PS_CHECK(on_demand_payload() == 0);
  return 0;
}
