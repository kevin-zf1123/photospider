#include <memory>
#include <string>
#include <vector>

#include "execution/shared_results.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using execution_internal::SharedResults;

int cancellation_domains() {
  ResourceBudget budget;
  {
    SharedResults table;
    CancellationSource a, b;
    auto call_a =
        table.join_call("snapshot", budget, a.token(), {"r"}).take_value();
    auto call_b =
        table.join_call("snapshot", budget, b.token(), {"s"}).take_value();
    auto r = table.acquire("r", budget, a.token(), call_a).take_value();
    auto s = table.acquire("s", budget, b.token(), call_b).take_value();
    a.cancel();
    call_a.refresh();
    r.refresh();
    s.refresh();
    PS_CHECK(r.token().cancelled() && !s.token().cancelled());
    b.cancel();
    call_b.refresh();
    s.refresh();
    PS_CHECK(s.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  {
    SharedResults table;
    CancellationSource a, b;
    auto call_a = table.join_call("snapshot", budget, a.token(), {"r", "sum"})
                      .take_value();
    auto call_b = table.join_call("snapshot", budget, b.token(), {"r", "sum"})
                      .take_value();
    auto r = table.acquire("r", budget, a.token(), call_a).take_value();
    auto sum = table.acquire("sum", budget, b.token(), call_b).take_value();
    // B has a declared future interest in r even before acquiring its waiter.
    a.cancel();
    call_a.refresh();
    r.refresh();
    sum.refresh();
    PS_CHECK(!r.token().cancelled() && r.has_other_waiters());
    b.cancel();
    call_b.refresh();
    r.refresh();
    sum.refresh();
    PS_CHECK(r.token().cancelled() && sum.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}

int escaped_token() {
  ResourceBudget budget;
  CancellationToken retained;
  {
    SharedResults table;
    auto call = table.join_call("snapshot", budget, {}, {"r"}).take_value();
    auto producer = table.acquire("r", budget, {}, call).take_value();
    retained = producer.token();
  }
  PS_CHECK(retained.cancelled() &&
           budget.statistics().live[ResourceKind::Host] > 0);
  retained = {};
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int retirement_epochs() {
  ResourceBudget budget;
  {
    SharedResults table;
    auto a = table.join_call("snapshot", budget, {}, {"r"}).take_value();
    auto old = table.acquire("r", budget, {}, a).take_value();
    PS_CHECK(old.producer());
    a.retire_user();
    PS_CHECK(!old.continue_for_peers());
    auto b = table.join_call("snapshot", budget, {}, {"r"}).take_value();
    auto replacement = table.acquire("r", budget, {}, b).take_value();
    PS_CHECK(replacement.producer());
    // Completion of the retired epoch cannot modify the new table entry.
    old.fail(ErrorCode::Cancelled);
    replacement.refresh();
    PS_CHECK(!replacement.token().cancelled());
    auto c = table.join_call("snapshot", budget, {}, {"r"}).take_value();
    auto peer = table.acquire("r", budget, {}, c).take_value();
    PS_CHECK(!peer.producer());
    b.retire_user();
    PS_CHECK(replacement.continue_for_peers());
    replacement.fail(ErrorCode::OperationFailed);
    PS_CHECK(peer.wait(true, 0, 0, {}).status().code ==
             ErrorCode::OperationFailed);
    auto next = table.acquire("r", budget, {}, c).take_value();
    PS_CHECK(next.producer());
    peer = {};
    replacement = {};
    next.refresh();
    PS_CHECK(!next.token().cancelled());
    c.retire_user();
    next.refresh();
    PS_CHECK(next.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(cancellation_domains() == 0);
  PS_CHECK(retirement_epochs() == 0);
  PS_CHECK(escaped_token() == 0);
  return 0;
}
