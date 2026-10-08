#include <cstdint>
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
int dependency_interests() {
  ResourceBudget budget;
  {
    SharedResults table;
    CancellationSource a, b, c;
    auto first = table.join_call("snapshot", budget, a.token(), {"parent:left"})
                     .take_value();
    auto parent =
        table.acquire("parent:left", budget, a.token(), first).take_value();
    PS_CHECK(parent.add_dependency("middle:left").ok());
    auto middle =
        table.acquire("middle:left", budget, a.token(), first).take_value();
    PS_CHECK(middle.add_dependency("source:left").ok());
    PS_CHECK(middle.add_dependency("source:left").ok());
    auto source =
        table.acquire("source:left", budget, a.token(), first).take_value();
    // A late caller of the parent keeps its transitive exact demand alive.
    auto peer = table.join_call("snapshot", budget, b.token(), {"parent:left"})
                    .take_value();
    auto unrelated =
        table.join_call("snapshot", budget, c.token(), {"parent:right"})
            .take_value();
    auto other = table.acquire("parent:right", budget, c.token(), unrelated)
                     .take_value();
    PS_CHECK(other.add_dependency("source:right").ok());
    auto right = table.acquire("source:right", budget, c.token(), unrelated)
                     .take_value();
    a.cancel();
    source.refresh();
    middle.refresh();
    PS_CHECK(!source.token().cancelled() && source.has_other_waiters());
    PS_CHECK(!middle.token().cancelled() && middle.continue_for_peers());
    b.cancel();
    source.refresh();
    middle.refresh();
    right.refresh();
    PS_CHECK(source.token().cancelled() && middle.token().cancelled());
    PS_CHECK(!right.token().cancelled());
    c.cancel();
    right.refresh();
    PS_CHECK(right.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int reused_interest_slot() {
  ResourceBudget budget;
  {
    SharedResults table;
    auto first =
        table.join_call("snapshot", budget, {}, {"parent:left"}).take_value();
    auto producer =
        table.acquire("parent:left", budget, {}, first).take_value();
    PS_CHECK(producer.add_dependency("source:left").ok());
    auto keeper =
        table.join_call("snapshot", budget, {}, {"keep"}).take_value();
    producer = {};
    first = {};
    auto next =
        table.join_call("snapshot", budget, {}, {"parent:right"}).take_value();
    // Reusing the old caller slot must not inherit its derived left demand.
    auto left = table.acquire("source:left", budget, {}, keeper).take_value();
    left.refresh();
    PS_CHECK(left.token().cancelled());
    auto right = table.acquire("parent:right", budget, {}, next).take_value();
    PS_CHECK(right.add_dependency("source:right").ok());
    auto source = table.acquire("source:right", budget, {}, next).take_value();
    source.refresh();
    PS_CHECK(!source.token().cancelled());
    next.retire_user();
    source.refresh();
    PS_CHECK(source.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int multiple_dependency_parents() {
  ResourceBudget budget;
  {
    SharedResults table;
    CancellationSource a, b;
    auto first =
        table.join_call("snapshot", budget, a.token(), {"first"}).take_value();
    auto second =
        table.join_call("snapshot", budget, b.token(), {"second"}).take_value();
    auto parent = table.acquire("first", budget, a.token(), first).take_value();
    auto other =
        table.acquire("second", budget, b.token(), second).take_value();
    PS_CHECK(parent.add_dependency("child").ok());
    PS_CHECK(other.add_dependency("middle").ok());
    auto middle =
        table.acquire("middle", budget, b.token(), second).take_value();
    PS_CHECK(middle.add_dependency("child").ok());
    auto child = table.acquire("child", budget, a.token(), first).take_value();
    a.cancel();
    child.refresh();
    PS_CHECK(!child.token().cancelled() && child.continue_for_peers());
    second.retire_user();
    child.refresh();
    PS_CHECK(child.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int dependency_admission_failure() {
  ResourceLimits limits;
  limits.maximum_work = 1000;
  ResourceBudget budget(limits);
  {
    SharedResults table;
    auto call =
        table.join_call("snapshot", budget, {}, {"parent"}).take_value();
    auto parent = table.acquire("parent", budget, {}, call).take_value();
    PS_CHECK(
        budget.consume({limits.maximum_work - budget.statistics().issued.work})
            .ok());
    auto failure = parent.add_dependency("child");
    PS_CHECK(failure.code == ErrorCode::ResourceExhausted);
    auto child = table.acquire("child", budget, {}, call);
    PS_CHECK(!child.ok() && child.status().code == ErrorCode::ResourceExhausted);
    parent.fail(failure);
    call.retire_user();
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int dependency_propagation_failure() {
  ResourceLimits limits;
  limits.maximum_work = 1000;
  ResourceBudget budget(limits);
  {
    SharedResults table;
    CancellationSource cancellation;
    auto first =
        table.join_call("snapshot", budget, {}, {"parent"}).take_value();
    auto second =
        table.join_call("snapshot", budget, cancellation.token(), {"middle"})
            .take_value();
    auto parent = table.acquire("parent", budget, {}, first).take_value();
    auto middle = table.acquire("middle", budget, {}, second).take_value();
    PS_CHECK(middle.add_dependency("leaf").ok());
    auto leaf = table.acquire("leaf", budget, {}, second).take_value();
    // Admit the first traversal step, but exhaust work before reaching leaf.
    PS_CHECK(budget
                 .consume({limits.maximum_work -
                           budget.statistics().issued.work - 3})
                 .ok());
    PS_CHECK(parent.add_dependency("middle").code ==
             ErrorCode::ResourceExhausted);
    cancellation.cancel();
    middle.refresh();
    leaf.refresh();
    // Failed admission must not leave either endpoint protected by first.
    PS_CHECK(middle.token().cancelled() && leaf.token().cancelled());
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int retired_dependency_interests() {
  ResourceBudget budget;
  for (unsigned mode : {0U, 1U, 2U}) {
    {
      SharedResults table;
      auto first =
          table.join_call("snapshot", budget, {}, {"parent"}).take_value();
      auto peer =
          table.join_call("snapshot", budget, {}, {"parent"}).take_value();
      auto parent = table.acquire("parent", budget, {}, first).take_value();
      PS_CHECK(parent.add_dependency("prefix").ok());
      auto source = table.acquire("prefix", budget, {}, first).take_value();
      first.retire_user();
      source.refresh();
      PS_CHECK(!source.token().cancelled());
      if (mode == 0)
        parent.publish({}, true, Backend::Cpu);
      else if (mode == 1)
        parent.fail(Status{ErrorCode::OperationFailed, {}});
      else
        parent = {};
      // A live peer of a retired parent no longer needs its partial source.
      source.refresh();
      PS_CHECK(source.token().cancelled());
    }
    for (auto live : budget.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int dependency_producer_epochs() {
  ResourceBudget budget;
  {
    SharedResults table;
    auto first =
        table.join_call("snapshot", budget, {}, {"parent"}).take_value();
    auto peer =
        table.join_call("snapshot", budget, {}, {"parent"}).take_value();
    auto old = table.acquire("parent", budget, {}, first).take_value();
    PS_CHECK(old.add_dependency("old-source").ok());
    auto old_source =
        table.acquire("old-source", budget, {}, first).take_value();
    old.fail(Status{ErrorCode::OperationFailed, {}});
    first.retire_user();
    auto current = table.acquire("parent", budget, {}, peer).take_value();
    PS_CHECK(current.producer());
    PS_CHECK(current.add_dependency("new-source").ok());
    auto source = table.acquire("new-source", budget, {}, peer).take_value();
    auto late =
        table.join_call("snapshot", budget, {}, {"parent"}).take_value();
    PS_CHECK(old.add_dependency("unwanted").code == ErrorCode::Cancelled);
    old = {};
    peer.retire_user();
    source.refresh();
    old_source.refresh();
    PS_CHECK(!source.token().cancelled() && old_source.token().cancelled());
    late.retire_user();
    source.refresh();
    PS_CHECK(source.token().cancelled());
  }
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
    PS_CHECK(table.statistics().first == 1);
    a.retire_user();
    PS_CHECK(!old.continue_for_peers());
    auto b = table.join_call("snapshot", budget, {}, {"r"}).take_value();
    auto replacement = table.acquire("r", budget, {}, b).take_value();
    PS_CHECK(replacement.producer());
    PS_CHECK(table.statistics().first == 2);
    // Completion of the retired epoch cannot modify the new table entry.
    old.fail(Status{ErrorCode::Cancelled, {}});
    PS_CHECK(table.statistics().first == 1);
    replacement.refresh();
    PS_CHECK(!replacement.token().cancelled());
    auto c = table.join_call("snapshot", budget, {}, {"r"}).take_value();
    auto peer = table.acquire("r", budget, {}, c).take_value();
    PS_CHECK(!peer.producer());
    PS_CHECK(table.statistics().second == 1);
    b.retire_user();
    PS_CHECK(replacement.continue_for_peers());
    auto pending = peer.poll(true, 0, 0, {});
    PS_CHECK(pending.ok() && !pending.value());
    Status failure{ErrorCode::OperationFailed,
                   "associated publication failed",
                   FailureReason::InvalidAssociation,
                   {FailureOrigin::Schema, FailureScope::Association}};
    failure.detail.association = 876;
    failure.detail.node_id = 91;
    replacement.fail(failure);
    PS_CHECK(table.statistics().first == 0);
    const auto observed = peer.wait(true, 0, 0, {}).status();
    PS_CHECK(
        observed.code == failure.code && observed.reason == failure.reason &&
        observed.message == failure.message &&
        observed.detail.origin == failure.detail.origin &&
        observed.detail.scope == failure.detail.scope &&
        observed.detail.association == 876 && observed.detail.node_id == 91);
    auto next = table.acquire("r", budget, {}, c).take_value();
    PS_CHECK(next.producer());
    PS_CHECK(table.statistics().first == 1);
    peer = {};
    replacement = {};
    next.refresh();
    PS_CHECK(!next.token().cancelled());
    c.retire_user();
    next.refresh();
    PS_CHECK(next.token().cancelled());
    next = {};
    PS_CHECK(table.statistics().first == 0);
  }
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int fallback_publication() {
  ResourceBudget budget;
  SharedResults table;
  auto call = table.join_call("snapshot", budget, {}, {"r"}).take_value();
  auto producer = table.acquire("r", budget, {}, call).take_value();
  producer.publish({}, false, Backend::Gpu, true);
  PS_CHECK(producer.backend() == Backend::Gpu && producer.fallback_taint());
  producer.publish({}, false, Backend::Gpu, false);
  auto peer_call = table.join_call("snapshot", budget, {}, {"r"}).take_value();
  auto peer = table.acquire("r", budget, {}, peer_call).take_value();
  PS_CHECK(!peer.producer() && peer.backend() == Backend::Gpu &&
           peer.fallback_taint());
  return 0;
}
int completed_statistics() {
  ResourceBudget root;
  {
    SharedResults table;
    PS_CHECK(table.statistics().first == 0 && table.statistics().second == 0);
    auto a = table.join_call("snapshot", root, {}, {"r"}).take_value();
    auto producer = table.acquire("r", root, {}, a).take_value();
    auto b = table.join_call("snapshot", root, {}, {"r"}).take_value();
    auto peer = table.acquire("r", root, {}, b).take_value();
    PS_CHECK(table.statistics().first == 1 && table.statistics().second == 1);
    SchemaTemplate schema;
    schema.id = "test.statistics";
    ResultTensorSpec tensor;
    tensor.key = "sample";
    tensor.descriptor = {ElementType::UInt8, {1}};
    schema.tensors.push_back(tensor);
    auto builder = ResultBuilder::start(root, schema, "stats").take_value();
    PS_CHECK(builder
                 .bind_descriptor_relation(
                     ResultRelation::cartesian(root, 1, {}).take_value())
                 .ok());
    const std::uint8_t value = 7;
    PS_CHECK(
        builder
            .publish_tensor(0, Region::whole({1}), ByteView(&value, 1),
                            ResultRelation::cartesian(root, 1, {}).take_value(),
                            {true, true, true, true})
            .ok());
    auto result = builder.seal().take_value();
    producer.publish(result, false, Backend::Cpu);
    PS_CHECK(table.statistics().first == 1);
    producer.publish(result, true, Backend::Cpu);
    producer.publish(result, true, Backend::Cpu);
    producer.fail(Status{ErrorCode::OperationFailed, {}});
    PS_CHECK(table.statistics().first == 0 && table.statistics().second == 1);
    auto completed = table.acquire("r", root, {}, b).take_value();
    PS_CHECK(!completed.producer());
    auto observed = completed.poll(true, 0, 0, {});
    PS_CHECK(observed.ok() && observed.value() &&
             observed.value()->object_id() == result.object_id());
    PS_CHECK(table.statistics().first == 0 && table.statistics().second == 2);
    producer = {};
    peer = {};
    completed = {};
    PS_CHECK(table.statistics().first == 0);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int protocol_before_cancel() {
  ResourceBudget budget;
  SharedResults table;
  CancellationSource cancellation;
  auto a = table.join_call("snapshot", budget, {}, {"r"}).take_value();
  auto producer = table.acquire("r", budget, {}, a).take_value();
  auto b = table.join_call("snapshot", budget, cancellation.token(), {"r"})
               .take_value();
  auto peer = table.acquire("r", budget, cancellation.token(), b).take_value();
  producer.fail(Status{ErrorCode::InvalidArgument,
                       "bad envelope",
                       FailureReason::MalformedEnvelope,
                       {FailureOrigin::Protocol, FailureScope::Group}});
  cancellation.cancel();
  auto result = peer.poll(true, 0, 0, cancellation.token());
  PS_CHECK(!result.ok() &&
           result.status().reason == FailureReason::MalformedEnvelope &&
           result.status().detail.origin == FailureOrigin::Protocol);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(cancellation_domains() == 0);
  PS_CHECK(dependency_interests() == 0);
  PS_CHECK(reused_interest_slot() == 0);
  PS_CHECK(multiple_dependency_parents() == 0);
  PS_CHECK(dependency_admission_failure() == 0);
  PS_CHECK(dependency_propagation_failure() == 0);
  PS_CHECK(retired_dependency_interests() == 0);
  PS_CHECK(dependency_producer_epochs() == 0);
  PS_CHECK(completed_statistics() == 0);
  PS_CHECK(retirement_epochs() == 0);
  PS_CHECK(escaped_token() == 0);
  PS_CHECK(protocol_before_cancel() == 0);
  PS_CHECK(fallback_publication() == 0);
  return 0;
}
