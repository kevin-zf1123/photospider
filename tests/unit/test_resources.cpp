#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "execution/memory_budget.hpp"
#include "photospider/execution/resource_allocator.hpp"
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
  l.maximum_stages = 1;
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
    PS_CHECK(root.consume({4, 1}).reason == FailureReason::WorkLimit);
    PS_CHECK(root.statistics().issued.io_bytes == 11);
    first = {};
    PS_CHECK(root.statistics().live[ResourceKind::Host] == 16 + overhead);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Host] == 0);
  PS_CHECK(root.statistics().issued.work == 7);
  PS_CHECK(root.consume({0, 0, 0, 1}).ok());
  PS_CHECK(root.consume({0, 0, 0, 1}).reason == FailureReason::StageLimit);
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
int allocator_ownership() {
  ResourceBudget root(limits(4096));
  {
    ResourceVector<std::uint64_t> first{ResourceAllocator<std::uint64_t>(root)};
    first.assign(16, 7);
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
int dependency_metadata_owners() {
  ResourceBudget root(ResourceLimits{});
  DependencyCertificate retained;
  DependencyNeedBatch batch;
  {
    ResourceAllocationScope scope(root);
    auto coverage = Footprint::all({2}).take_value();
    const auto first =
        Footprint::from_regions({2}, {Region({{0, 1}})}).take_value();
    const auto second =
        Footprint::from_regions({2}, {Region({{1, 1}})}).take_value();
    retained = DependencyCertificate::create(
                   "metadata-owner", coverage, {{2}},
                   {{{0}, {{0, 1, first, {}}}}, {{1}, {{0, 1, second, {}}}}})
                   .take_value();
    batch = DependencyNeedBatch(retained.rows());
  }
  const auto baseline = root.statistics().live[ResourceKind::Metadata];
  PS_CHECK(baseline > 0);
  {
    auto copy = retained;
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] > baseline);
    auto subset = Footprint::from_regions({2}, {Region({{0, 1}})}).take_value();
    auto restricted = retained.restrict(subset).take_value();
    PS_CHECK(restricted.rows().size() == 1);
    auto moved = std::move(copy);
    copy = retained;
    copy = std::move(moved);  // Retire nonempty old storage before its lease.
    std::string identity;
    identity.reserve(4096);
    identity = "long-capacity";
    const auto before = root.statistics().live[ResourceKind::Metadata];
    auto renamed = retained.with_identity(std::move(identity)).take_value();
    PS_CHECK(renamed.identity() == "long-capacity");
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] >= before + 4096);
    auto batch_copy = batch;
    PS_CHECK(batch_copy.associations.size() == 2);
    auto low_limits = limits(1);
    low_limits.capacity[ResourceKind::Metadata] = 1;
    ResourceBudget low(low_limits);
    ErrorCode failure = ErrorCode::Ok;
    bool rejected = false;
    {
      ResourceAllocationScope scope(low, &failure);
      try {
        copy = retained;
      } catch (const std::bad_alloc&) {
        rejected = true;
      }
    }
    PS_CHECK(rejected && failure == ErrorCode::ResourceExhausted);
    PS_CHECK(copy.valid() && copy.identity() == retained.identity());
  }
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] == baseline);
  retained = {};
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
  batch = {};
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] == 0);
  return 0;
}
struct RegionalMetadataProbe {
  bool requested = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch
          batch;  // Deliberately use the public mutable builder.
      auto status = phase.query.outputs.visit(
          [&](const auto& coordinate) {
            auto point =
                Footprint::from_regions({2}, {Region({{coordinate[0], 1}})});
            if (!point.ok())
              return point.status();
            batch.associations.push_back(
                {coordinate, {{0, 1, point.take_value(), {}}}});
            return Status::success();
          },
          2);
      return status.ok() ? Result<DependencyPoll>(std::move(batch))
                         : Result<DependencyPoll>(status);
    }
    return Result<DependencyPoll>(phase.inputs[0]);
  }
};
int static_piece_work() {
  auto registry = make_default_operation_registry(false);
  OperationDefinition definition;
  definition.key = "test.static_piece_work";
  definition.traits.input_count = 128;
  definition.traits.input_schema.resize(128);
  auto& output = definition.traits.outputs[0];
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {64};
  output.output_element_type = ElementType::Int64;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(RegionalMetadataProbe);
  output.maximum_dependency_stages = 2;
  output.static_dependency_pieces = std::vector<DependencyMapPiece>{
      {Footprint::from_regions({64}, {Region({{0, 32}})}).take_value(),
       {{0, 1, {{0, {}, 0}}, {}}}},
      {Footprint::from_regions({64}, {Region({{32, 32}})}).take_value(),
       {{1, 1, {{0, {}, -32}}, {}}}}};
  unsigned starts = 0;
  definition.start_dependency = [&](const auto&, const auto& allocator) {
    ++starts;
    return DependencyContinuation::make<RegionalMetadataProbe>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(definition)).ok());
  PS_CHECK(registry->freeze().ok());
  DependencyRequest request;
  request.inputs.resize(128, {{ElementType::Int64, {32}}, {}});
  request.snapshot_identity = "piece-work";
  std::vector<Region> boxes;
  for (std::uint64_t i = 0; i < 20; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{3 * i, 1}});
  request.outputs = Footprint::from_regions({64}, boxes).take_value();
  request.limits.maximum_work = 50;
  std::uint64_t prior_work = 0;
  request.limits.sets.consume_work = [&](std::uint64_t amount) {
    prior_work += amount;
    return Status::success();
  };
  auto stopped = registry->start_dependency("test.static_piece_work", request);
  PS_CHECK(stopped.status().reason == FailureReason::WorkLimit);
  PS_CHECK(starts == 0);
  PS_CHECK(prior_work > 0);
  request.outputs = Footprint::all({64}).take_value();
  request.limits.maximum_work = 10000;
  std::uint64_t root_work = 0;
  auto limited = registry->start_dependency(
      "test.static_piece_work", request, BufferAllocator{},
      [&](std::uint64_t amount) {
        root_work += amount;
        return root_work > 200 ? Status{ErrorCode::ResourceExhausted,
                                        "root work", FailureReason::WorkLimit}
                               : Status::success();
      });
  PS_CHECK(limited.status().reason == FailureReason::WorkLimit);
  PS_CHECK(starts == 0);
  auto accepted = registry->start_dependency("test.static_piece_work", request);
  PS_CHECK(accepted.ok() && starts == 1);
  return 0;
}
int regional_scope_retention() {
  auto registry = make_default_operation_registry(false);
  OperationDefinition definition;
  definition.key = "test.regional_metadata";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  auto& output = definition.traits.outputs[0];
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.preserve_output_views = true;
  output.maximum_output_payload_bytes = 0;
  output.continuation_bytes = sizeof(RegionalMetadataProbe);
  output.maximum_dependency_stages = 2;
  definition.start_dependency = [](const auto&, const auto& allocator) {
    return DependencyContinuation::make<RegionalMetadataProbe>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(definition)).ok());
  PS_CHECK(registry->freeze().ok());
  ResourceBudget root(ResourceLimits{});
  std::shared_ptr<DependencySession> session;
  DependencyRequest request;
  request.inputs = {{{ElementType::Int64, {2}}, {}}};
  request.outputs = Footprint::all({2}).take_value();
  request.snapshot_identity = "scope-retention";
  {
    ResourceAllocationScope scope(root);
    session = registry
                  ->start_dependency("test.regional_metadata", request,
                                     root.allocator())
                  .take_value();
  }
  // Poll outside the start scope, then retain its mutable-built event beyond
  // the Session. The host must reseal it under the retained metadata root.
  std::optional<DependencyProgress> pending(
      session->poll(root.allocator()).take_value());
  PS_CHECK(std::get<DependencyNeedBatch>(*pending).associations.size() == 2);
  auto buffer = root.allocator().allocate(16).take_value();
  auto input =
      Value::from_storage({ElementType::Int64, {2}}, Region::whole({2}),
                          {0, {8}}, std::move(buffer).freeze())
          .take_value();
  auto fragments =
      ValueFragments::create(input.descriptor(), {}, request.outputs, {input})
          .take_value();
  PS_CHECK(session->supply({fragments}, request.snapshot_identity).ok());
  auto done = session->poll(root.allocator()).take_value();
  auto certificate = std::get<DependencyResult>(done).certificate;
  done = DependencyNeedBatch{};
  session.reset();
  fragments = {};
  input = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
  pending.reset();
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
  certificate = {};
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] == 0);
  return 0;
}
int mixed_certificate_roots() {
  ResourceBudget root(ResourceLimits{});
  const auto left =
      Footprint::from_regions({2}, {Region({{0, 1}})}).take_value();
  const auto right =
      Footprint::from_regions({2}, {Region({{1, 1}})}).take_value();
  for (bool mapped : {false, true}) {
    const auto make = [&](const Footprint& coverage) {
      if (mapped)
        return DependencyCertificate::create_mapped(
                   "mixed", coverage, {{2}},
                   {{coverage, {{0, 1, {{0, {}}}, {}}}}})
            .take_value();
      const auto coordinate = coverage.boxes()[0].dimensions()[0].offset;
      return DependencyCertificate::create(
                 "mixed", coverage, {{2}},
                 {{{coordinate}, {{0, 1, coverage, {}}}}})
          .take_value();
    };
    auto caller = make(left);
    DependencyCertificate managed;
    {
      ResourceAllocationScope scope(root);
      managed = make(right);
    }
    auto merged = caller.merge(managed).take_value();
    managed = {};
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
    PS_CHECK(merged.coverage() == Footprint::all({2}).take_value());
    merged = {};
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] == 0);
  }
  std::uint64_t baseline_delta = 0, baseline_capacity = 0;
  for (unsigned length : {1U, 32U, 64U}) {
    DependencyCertificate source;
    {
      ResourceAllocationScope scope(root);
      source = DependencyCertificate::create(std::string(length, 'x'), left,
                                             {{2}}, {{{0}, {{0, 1, left, {}}}}})
                   .take_value();
    }
    const auto before = root.statistics().live[ResourceKind::Metadata];
    auto copy = source;
    const auto delta = root.statistics().live[ResourceKind::Metadata] - before;
    if (length == 1) {
      baseline_delta = delta;
      baseline_capacity = copy.identity().capacity();
    } else {
      PS_CHECK(delta - baseline_delta >=
               copy.identity().capacity() - baseline_capacity);
    }
  }
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] == 0);
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
  PS_CHECK(ledger() == 0);
  PS_CHECK(execution_owner() == 0);
  PS_CHECK(referenced_owner() == 0);
  PS_CHECK(normalized_work() == 0);
  PS_CHECK(paging() == 0);
  PS_CHECK(failures() == 0);
  PS_CHECK(concurrent() == 0);
  PS_CHECK(concurrent_references() == 0);
  PS_CHECK(allocator_ownership() == 0);
  PS_CHECK(on_demand_payload() == 0);
  PS_CHECK(dependency_metadata_owners() == 0);
  PS_CHECK(regional_scope_retention() == 0);
  PS_CHECK(static_piece_work() == 0);
  PS_CHECK(mixed_certificate_roots() == 0);
  return 0;
}
#include <array>
#include <atomic>
