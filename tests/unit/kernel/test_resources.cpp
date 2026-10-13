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
SchemaTemplate resource_schema() {
  SchemaTemplate schema;
  schema.id = "test.resource_scalar";
  ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {ElementType::Float64, {1}};
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
OperationOutputTraits result_output(const SchemaTemplate& schema,
                                    std::uint64_t state = 0) {
  OperationOutputTraits output;
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  output.continuation_bytes = state;
  output.maximum_dependency_stages = 2;
  return output;
}
struct ResourceScalar {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    auto made =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!made.ok())
      return Poll(made.status());
    auto builder = made.take_value();
    auto buffer = phase.allocator.allocate(8);
    if (!buffer.ok())
      return Poll(buffer.status());
    auto storage = buffer.take_value();
    const double number = 19;
    std::memcpy(storage.data(), &number, 8);
    auto status = builder.publish_tensor(0, Region::whole({1}), {0, {8}},
                                         std::move(storage).freeze());
    if (!status.ok())
      return Poll(status);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
int execution_owner() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "resource_scalar";
  definition.traits.workspace_bytes = 8;
  definition.traits.outputs[0] =
      result_output(resource_schema(), sizeof(ResourceScalar));
  definition.traits.cacheable = false;
  definition.start_result = [](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ResourceScalar>(allocator);
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
  config.managed_resources = limits(65536);
  auto context = std::make_unique<ExecutionContext>(registry, config);
  auto root = context->resource_budget().take_value();
  ResultRef held;
  {
    auto result = context->execute(compiled.value().plan);
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
  double number = 0;
  PS_CHECK(held.read_tensor(held.descriptor().take_value(), 0, {0}, &number, 8)
               .ok());
  PS_CHECK(number == 19);
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
  auto value = Value::from_float64(7);
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
struct RegionalMetadataProbe {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, true, false, {}});
      return Result<ResultProgramPoll>(std::move(need));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{phase.results.at(0), true});
  }
};
int regional_scope_retention() {
  auto registry = std::make_shared<OperationRegistry>();
  auto schema = resource_schema();
  schema.tensors.clear();
  schema.fields = {{"first", ElementType::Int64, {}, {}},
                   {"second", ElementType::Int64, {}, {}}};
  OperationDefinition definition;
  definition.key = "test.regional_metadata";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  definition.traits.input_schema[0].kind = OperationPortKind::Result;
  definition.traits.input_schema[0].result_schema_id = schema.id;
  definition.traits.input_schema[0].result_schema_version = schema.version;
  definition.traits.outputs[0] =
      result_output(schema, sizeof(RegionalMetadataProbe));
  definition.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<RegionalMetadataProbe>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(definition)).ok());
  PS_CHECK(registry->freeze().ok());
  ResourceBudget root(ResourceLimits{});
  ResultProgramMetadata metadata;
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  metadata.output.result_schema = metadata.inputs[0].result_schema;
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "scope-retention";
  auto allocator = root.allocator();
  ResultContinuation continuation;
  {
    ResourceAllocationScope scope(root);
    continuation =
        registry->start_result("test.regional_metadata", query, allocator)
            .take_value();
  }
  ResultObjectInputs objects;
  ResultProgramPhase phase{
      query,
      objects,
      allocator,
      root,
      [&](std::uint64_t amount) { return root.consume({amount}); },
      {}};
  std::optional<ResultProgramPoll> pending;
  {
    // Direct hosts establish the poll's metadata allocation domain explicitly.
    ResourceAllocationScope scope(root);
    pending.emplace(continuation.poll(phase).take_value());
  }
  PS_CHECK(std::get<ResultProgramNeed>(*pending).tiles.size() == 1);
  PS_CHECK(std::get<ResultProgramNeed>(*pending).tiles.get_allocator().owned_by(
      root));
  ResultRef retained;
  {
    auto builder =
        ResultBuilder::start(root, schema, query.semantic_key).take_value();
    const std::int64_t number = 7;
    for (unsigned field = 0; field < 2; ++field) {
      PS_CHECK(
          builder
              .append(
                  field, 1,
                  ByteView(reinterpret_cast<const std::uint8_t*>(&number), 8))
              .ok());
      PS_CHECK(builder.publish(field, 1).ok());
    }
    objects.emplace(0, builder.seal().take_value());
    auto done = continuation.poll(phase).take_value();
    retained = std::get<ResultPublication>(done).result;
  }
  continuation = {};
  objects.clear();
  PS_CHECK(root.statistics().live[ResourceKind::Payload] >= 16);
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
  const auto with_need = root.statistics().live[ResourceKind::Metadata];
  pending.reset();
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] < with_need);
  retained = {};
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
  PS_CHECK(regional_scope_retention() == 0);
  return 0;
}
