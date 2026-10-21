#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/photospider.hpp"
#include "support/fixed_operation_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
ResourceLimits limits(std::uint64_t host) {
  ResourceLimits l;
  l.capacity[ResourceKind::Host] = host;
  l.capacity[ResourceKind::Shared] = host;
  l.capacity[ResourceKind::Metadata] = host;
  return l;
}
int concurrent_work() {
  ResourceLimits l;
  l.maximum_work = 10000;
  l.maximum_stages = 2000;
  ResourceBudget root(l);
  std::array<ResourceWork, 8> accepted{};
  std::atomic<bool> valid{true};
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < accepted.size(); ++t)
    threads.emplace_back([&, t] {
      Status failure;
      for (unsigned i = 0; i < 3000; ++i) {
        const bool mixed = (i + t) % 2;
        ResourceWork next = mixed ? ResourceWork{3, 1} : ResourceWork{1};
        if (root.try_consume(next, failure)) {
          accepted[t].work += next.work;
          accepted[t].stages += next.stages;
        }
        const auto seen = root.statistics().issued;
        if (seen.work > 10000 || seen.stages > 2000 ||
            seen.work < seen.stages * 3)
          valid = false;
      }
    });
  for (auto& thread : threads)
    thread.join();
  ResourceWork sum;
  for (const auto& a : accepted) {
    sum.work += a.work;
    sum.stages += a.stages;
  }
  const auto got = root.statistics().issued;
  PS_CHECK(valid);
  PS_CHECK(got.work == 10000);
  PS_CHECK(got.work == sum.work && got.stages == sum.stages);
  Status failure;
  PS_CHECK(!root.try_consume({UINT64_MAX, UINT64_MAX}, failure));
  PS_CHECK(failure.reason == FailureReason::WorkLimit);
  for (auto maximum : {UINT64_C(0), UINT64_MAX}) {
    ResourceLimits exact;
    exact.maximum_work = maximum;
    ResourceBudget bounded(exact);
    Status failure;
    PS_CHECK(bounded.try_consume({maximum}, failure));
    PS_CHECK(bounded.statistics().issued.work == maximum);
    PS_CHECK(bounded.try_consume({0}, failure));
    PS_CHECK(!bounded.try_consume({1}, failure));
    PS_CHECK(failure.reason == FailureReason::WorkLimit);
    PS_CHECK(bounded.statistics().issued.work == maximum);
  }
  return 0;
}
int available_capacity() {
  ResourceLimits l;
  l.capacity.values.fill(4096);
  l.cleanup.values.fill(16);
  ResourceBudget root(l);
  const auto empty = root.available_capacity();
  for (auto bytes : empty.values)
    PS_CHECK(bytes == 4080);
  ErrorCode failure = ErrorCode::Ok;
  ResourceAllocationScope scope(root, &failure);
  {
    auto lease = root.reserve(ResourceCapacity::host(64, 16)).take_value();
    const auto available = root.available_capacity();
    const auto live = root.statistics().live;
    for (std::size_t i = 0; i < live.values.size(); ++i)
      PS_CHECK(available.values[i] + live.values[i] == empty.values[i]);
    PS_CHECK(failure == ErrorCode::Ok);
  }
  PS_CHECK(root.available_capacity().values == empty.values);
  return 0;
}
int ledger() {
  const auto overhead = ResourceBudget::lease_metadata_bytes();
  auto l = limits(overhead + 20);
  l.maximum_work = 10;
  l.maximum_stages = 1;
  l.cleanup = ResourceCapacity::host(2);
  ResourceBudget root(l);
  {
    auto first = root.reserve(ResourceCapacity::host(8)).take_value();
    auto alias = first;
    PS_CHECK(root.statistics().live[ResourceKind::Host] == 8 + overhead);
    PS_CHECK(!root.reserve(ResourceCapacity::host(16)).ok());
    auto invalid = ResourceCapacity::host(11);
    invalid[ResourceKind::Queue] = 100;
    PS_CHECK(!root.reserve(invalid).ok());
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
    PS_CHECK(first.grow(ResourceCapacity::host(8)).ok());
    PS_CHECK(alias.capacity()[ResourceKind::Host] == 16);
    PS_CHECK(!alias.grow(ResourceCapacity::host(3)).ok());
    PS_CHECK(root.consume({7}).ok());
    PS_CHECK(root.consume({4}).reason == FailureReason::WorkLimit);
    first = {};
    PS_CHECK(root.statistics().live[ResourceKind::Host] == 16 + overhead);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Host] == 0);
  PS_CHECK(root.statistics().issued.work == 7);
  Status detail{ErrorCode::Internal, "unchanged on success"};
  PS_CHECK(root.try_consume({0}, detail));
  PS_CHECK(detail.code == ErrorCode::Internal);
  PS_CHECK(!root.try_consume({4}, detail));
  PS_CHECK(detail.code == ErrorCode::ResourceExhausted);
  PS_CHECK(detail.reason == FailureReason::WorkLimit);
  PS_CHECK(root.statistics().issued.work == 7);

  PS_CHECK(root.consume({0, 1}).ok());
  PS_CHECK(root.consume({0, 1}).reason == FailureReason::StageLimit);
  PS_CHECK(root.statistics().protected_cleanup[ResourceKind::Host] == 2);
  auto shared = ResourceCapacity::host(10);
  shared[ResourceKind::Shared] = 10;
  shared[ResourceKind::Device] = 10;
  auto uma = root.reserve(shared).take_value();
  PS_CHECK(root.statistics().live[ResourceKind::Host] == 10 + overhead);
  PS_CHECK(root.statistics().live[ResourceKind::Device] == 10);
  uma.quarantine();
  uma = {};
  PS_CHECK(root.statistics().quarantined[ResourceKind::Host] == 10 + overhead);
  PS_CHECK(!root.reserve(ResourceCapacity::host(9)).ok());
  return 0;
}
int referenced_owner() {
  auto l = limits(8192);
  l.capacity[ResourceKind::Referenced] = 8;
  ResourceBudget root(l);
  auto value = TensorSpan::from_float64(7);
  auto first = root.reference(value.storage()).take_value();
  auto second = root.reference(value.storage()).take_value();
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 8);
  PS_CHECK(!root.reference(TensorSpan::from_float64(9).storage()).ok());
  first.reset();
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 8);
  second.reset();
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
  auto third = root.reference(TensorSpan::from_float64(9).storage());
  PS_CHECK(third.ok());
  return 0;
}
int execution_owner() {
  auto registry = std::make_shared<OperationRegistry>();
  auto definition = test::fixed_fixture::operation("resource_scalar", 0);
  PS_CHECK(registry->register_operation(definition).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "resource_scalar", {}, {}}};
  document.outputs = {{"value", 1, "out0"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  auto config = ExecutionContextConfig{};
  config.cpu_workers = 1;
  config.maximum_queued_tasks = 8;
  config.maximum_runs = 1;
  config.managed_resources = limits(65536);
  auto context = std::make_unique<ExecutionContext>(registry, config);
  auto root = context->resource_budget().take_value();
  ResultRef held;
  {
    auto prepared = context->prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(prepared);
    auto submitted = context->submit(prepared.value());
    PS_REQUIRE_OK(submitted);
    auto result = submitted.value().join();
    if (!result.ok())
      std::cerr << result.status().message
                << " code=" << static_cast<unsigned>(result.status().code)
                << " reason=" << static_cast<unsigned>(result.status().reason)
                << "\n";
    PS_CHECK(result.ok());
    held = result.value().results.at("value");
  }
  const auto retained = root.statistics().live[ResourceKind::Host];
  PS_CHECK(retained >= 8);
  auto alias = held;
  PS_CHECK(root.statistics().live[ResourceKind::Host] == retained);
  context.reset();
  std::uint8_t number = 0;
  PS_CHECK(held.read_tensor(held.descriptor().take_value(), 0, {0}, &number, 1)
               .ok());
  PS_CHECK(number == 1);
  held = {};
  alias = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}

int normalized_work() {
  auto l = limits(8192);
  l.maximum_work = 3;
  ResourceBudget root(l);
  FootprintLimits sets;
  sets.consume_work = [&](std::uint64_t count) {
    return root.consume({count});
  };
  auto made = Footprint::from_regions(
      {100}, {Region({{1, 10}}), Region({{20, 10}})}, sets);
  PS_CHECK(!made.ok() && made.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(root.statistics().issued.work > 0 &&
           root.statistics().issued.work <= l.maximum_work);
  return 0;
}
int allocator_ownership() {
  ResourceBudget root(limits(4096));
  {
    ResourceVector<std::uint64_t> first{ResourceAllocator<std::uint64_t>(root)};
    first.assign(16, 7);
    ResourceBudget other(limits(4096));
    ResourceVector<std::uint64_t> empty{
        ResourceAllocator<std::uint64_t>(other)};
    empty = first;
    PS_CHECK(empty == first && empty.get_allocator().owned_by(root));
    PS_CHECK(other.statistics().live[ResourceKind::Host] == 0);
    const auto original = root.statistics().live[ResourceKind::Host];
    auto copy = first;
    PS_CHECK(copy == first &&
             root.statistics().live[ResourceKind::Host] > original);
    auto moved = std::move(first);
    PS_CHECK(moved.size() == 16 && first.empty());
    first.push_back(9);  // A moved-from allocator remains usable.
    auto before = root.statistics().live[ResourceKind::Host];
    bool refused = false;
    try {
      copy.reserve(4096);
    } catch (const std::bad_alloc&) {
      refused = true;
    }
    PS_CHECK(refused && copy.size() == 16 &&
             root.statistics().live[ResourceKind::Host] == before);
    ErrorCode failure = ErrorCode::Ok;
    {
      ResourceAllocationScope scope(root, &failure);
      ResourceVector<std::uint8_t> scoped;
      PS_CHECK(scoped.get_allocator().owned_by(root));
      try {
        scoped.resize(10000);
      } catch (const std::bad_alloc&) {
      }
      PS_CHECK(failure == ErrorCode::ResourceExhausted);
    }
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  CancellationToken retained;
  {
    CancellationSource a(root), b(root);
    retained =
        CancellationToken::combine({a.token(), b.token()}, root).take_value();
    a.cancel();
  }
  PS_CHECK(retained.cancelled() &&
           root.statistics().live[ResourceKind::Host] > 0);
  retained = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int combined_tokens() {
  CancellationSource a, b;
  auto both = CancellationToken::combine({a.token(), b.token()});
  PS_REQUIRE_OK(both);
  auto nested = CancellationToken::combine({both.value(), a.token(), {}});
  PS_REQUIRE_OK(nested);
  PS_CHECK(!nested.value().cancelled());
  b.cancel();
  PS_CHECK(both.value().cancelled() && nested.value().cancelled() &&
           !a.token().cancelled());
  std::vector<CancellationSource> sources(65);
  std::vector<CancellationToken> tokens;
  for (auto& source : sources)
    tokens.push_back(source.token());
  PS_CHECK(CancellationToken::combine(tokens).status().code ==
           ErrorCode::ResourceExhausted);
  tokens.pop_back();
  auto all = CancellationToken::combine(tokens);
  PS_REQUIRE_OK(all);
  auto duplicate = CancellationToken::combine({all.value(), tokens[0], {}});
  PS_REQUIRE_OK(duplicate);
  PS_CHECK(!all.value().cancelled() && !duplicate.value().cancelled());
  PS_CHECK(CancellationToken::combine({all.value(), sources.back().token()})
               .status()
               .code == ErrorCode::ResourceExhausted);
  sources[32].cancel();
  PS_CHECK(all.value().cancelled() && duplicate.value().cancelled() &&
           !sources[0].token().cancelled() &&
           !sources.back().token().cancelled());
  return 0;
}
int concurrent() {
  ResourceBudget root(limits(1000));
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      for (unsigned j = 0; j < 1000; ++j) {
        auto lease = root.reserve(ResourceCapacity::host(29));
        if (lease.ok()) {
          auto alias = lease.value();
          (void)alias;
        }
      }
    });
  for (auto& thread : threads)
    thread.join();
  PS_CHECK(root.statistics().live[ResourceKind::Host] == 0);
  PS_CHECK(root.statistics().peak[ResourceKind::Host] <= 1000);
  return 0;
}
int concurrent_references() {
  auto l = limits(16384);
  l.capacity[ResourceKind::Referenced] = 8;
  auto value = TensorSpan::from_float64(7);
  ResourceBudget root(l);
  for (unsigned round = 0; round < 64; ++round) {
    std::array<std::shared_ptr<const CpuStorage>, 16> aliases;
    std::array<std::thread, 16> threads;
    std::atomic<unsigned> ready{0};
    for (unsigned i = 0; i < threads.size(); ++i)
      threads[i] = std::thread([&, i] {
        ready.fetch_add(1);
        while (ready.load() != threads.size())
          std::this_thread::yield();
        auto referenced = root.reference(value.storage());
        if (referenced.ok())
          aliases[i] = referenced.take_value();
      });
    for (auto& thread : threads)
      thread.join();
    for (const auto& alias : aliases)
      PS_CHECK(alias && alias.get() == value.storage().get());
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 8);
    aliases = {};
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
  }
  // Exercise a new first reference racing the retirement of the last alias.
  std::array<std::thread, 8> threads;
  std::atomic<unsigned> failures{0};
  for (auto& thread : threads)
    thread = std::thread([&] {
      for (unsigned i = 0; i < 1000; ++i)
        if (!root.reference(value.storage()).ok())
          failures.fetch_add(1);
    });
  for (auto& thread : threads)
    thread.join();
  PS_CHECK(failures == 0);
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}

}  // namespace
int main() {
  PS_CHECK(available_capacity() == 0);
  PS_CHECK(ledger() == 0);
  PS_CHECK(concurrent_work() == 0);
  PS_CHECK(execution_owner() == 0);
  PS_CHECK(referenced_owner() == 0);
  PS_CHECK(normalized_work() == 0);
  PS_CHECK(concurrent() == 0);
  PS_CHECK(concurrent_references() == 0);
  PS_CHECK(allocator_ownership() == 0);
  PS_CHECK(combined_tokens() == 0);
  return 0;
}
