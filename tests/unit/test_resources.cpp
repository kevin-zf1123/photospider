#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
ResourceLimits limits(std::uint64_t host) {
  ResourceLimits l;
  l.capacity[ResourceKind::Host] = host;
  l.capacity[ResourceKind::Shared] = host;
  l.capacity[ResourceKind::Metadata] = host;
  return l;
}
int ledger() {
  const auto overhead = ResourceBudget::lease_metadata_bytes();
  auto l = limits(overhead + 20);
  l.maximum_work = 10;
  l.maximum_io_bytes = 20;
  l.cleanup = ResourceCapacity::host(2);
  ResourceBudget root(l);
  {
    auto first = root.reserve(ResourceCapacity::host(8)).take_value();
    auto alias = first;
    PS_CHECK(root.statistics().live[ResourceKind::Host] == 8 + overhead);
    PS_CHECK(!root.reserve(ResourceCapacity::host(16)).ok());
    auto invalid = ResourceCapacity::host(11);
    invalid[ResourceKind::Disk] = 100;
    PS_CHECK(!root.reserve(invalid).ok());
    PS_CHECK(root.statistics().live[ResourceKind::Disk] == 0);
    PS_CHECK(first.grow(ResourceCapacity::host(8)).ok());
    PS_CHECK(alias.capacity()[ResourceKind::Host] == 16);
    PS_CHECK(!alias.grow(ResourceCapacity::host(3)).ok());
    PS_CHECK(root.consume({7, 11}).ok());
    PS_CHECK(!root.consume({4, 1}).ok());
    PS_CHECK(root.statistics().issued.io_bytes == 11);
    first = {};
    PS_CHECK(root.statistics().live[ResourceKind::Host] == 16 + overhead);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Host] == 0);
  PS_CHECK(root.statistics().issued.work == 7);
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
int paging() {
  ResourceBudget root(limits(16384));
  std::shared_ptr<const CpuStorage> held;
  {
    auto file = TemporaryStorage::create(root).take_value();
    PS_CHECK(file.append_zeroed(65536).ok());
    PS_CHECK(root.statistics().live[ResourceKind::Disk] == 65536);
    const std::uint64_t sample = 42;
    PS_CHECK(
        file.write(65528,
                   ByteView(reinterpret_cast<const std::uint8_t*>(&sample), 8))
            .ok());
    PS_CHECK(file.freeze_prefix(65536).ok());
    PS_CHECK(
        !file.write(0,
                    ByteView(reinterpret_cast<const std::uint8_t*>(&sample), 8))
             .ok());
    PS_CHECK(file.append_zeroed(8).ok());
    PS_CHECK(file.seal().ok());
    PS_CHECK(!file.append_zeroed(8).ok());
    PS_CHECK(!file.read(0, 16384, 4096).ok());
    held = file.read(65528, 8, 4096).take_value();
    PS_CHECK(root.statistics().peak[ResourceKind::Host] <= 16384);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Disk] != 0);
  std::uint64_t value = 0;
  std::memcpy(&value, held->bytes().data(), 8);
  PS_CHECK(value == 42);
  held.reset();
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int failures() {
  auto l = limits(16384);
  l.capacity[ResourceKind::Disk] = 4096;
  ResourceBudget root(l);
  {
    auto file = TemporaryStorage::create(root).take_value();
    PS_CHECK(file.append_zeroed(8).ok());
    PS_CHECK(!file.append_zeroed(4096).ok());
    PS_CHECK(file.size() == 8);
    PS_CHECK(root.statistics().live[ResourceKind::Disk] == 4096);
    CancellationSource cancel;
    cancel.cancel();
    PS_CHECK(file.append_zeroed(8, cancel.token()).status().code ==
             ErrorCode::Cancelled);
    PS_CHECK(file.read(0, 8, 8, cancel.token()).status().code ==
             ErrorCode::Cancelled);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  l.maximum_io_bytes = 4;
  ResourceBudget tiny(l);
  auto file = TemporaryStorage::create(tiny).take_value();
  PS_CHECK(!file.append_zeroed(8).ok());
  PS_CHECK(file.size() == 0 && tiny.statistics().live[ResourceKind::Disk] == 0);
  return 0;
}
int referenced_owner() {
  auto l = limits(8192);
  l.capacity[ResourceKind::Referenced] = 8;
  ResourceBudget root(l);
  auto value = Value::from_float64(7);
  auto first = root.reference(value.storage()).take_value();
  auto second = root.reference(value.storage()).take_value();
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 8);
  PS_CHECK(!root.reference(Value::from_float64(9).storage()).ok());
  first.reset();
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 8);
  second.reset();
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
  auto third = root.reference(Value::from_float64(9).storage());
  PS_CHECK(third.ok());
  return 0;
}
int execution_owner() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "resource_scalar";
  definition.callback = [](const OperationInvocation& invocation) {
    auto made = MutableValue::allocate(
        {ElementType::Float64, {1}}, Region::whole({1}), invocation.allocator);
    if (!made.ok())
      return Result<Value>(made.status());
    auto writer = made.take_value();
    double x = 19;
    std::memcpy(writer.data(), &x, 8);
    return std::move(writer).publish();
  };
  PS_CHECK(registry->register_operation(definition).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "resource_scalar", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  auto config = ExecutionContextConfig{};
  config.cpu_workers = 1;
  config.managed_resources = limits(16384);
  auto context = std::make_unique<ExecutionContext>(registry, config);
  auto root = context->resource_budget().take_value();
  Value held;
  {
    auto result = context->execute(compiled.value().plan);
    PS_CHECK(result.ok());
    held = result.value().values.at("value");
  }
  const auto retained = root.statistics().live[ResourceKind::Host];
  PS_CHECK(retained >= 8);
  auto alias = held;
  PS_CHECK(root.statistics().live[ResourceKind::Host] == retained);
  auto file = TemporaryStorage::create(root).take_value();
  PS_CHECK(file.append_zeroed(32768).ok());
  context.reset();
  PS_CHECK(held.as_float64().value() == 19);
  held = {};
  alias = {};
  file = {};
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
  auto made = Footprint::from_regions({100}, {Region({{1, 10}})}, sets);
  PS_CHECK(!made.ok() && made.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(root.statistics().issued.work == 3);
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
}  // namespace
int main() {
  PS_CHECK(ledger() == 0);
  PS_CHECK(execution_owner() == 0);
  PS_CHECK(referenced_owner() == 0);
  PS_CHECK(normalized_work() == 0);
  PS_CHECK(paging() == 0);
  PS_CHECK(failures() == 0);
  PS_CHECK(concurrent() == 0);
  return 0;
}
