#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include "execution/shared_results.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
std::atomic<bool> inspect_allocations{false};
std::atomic<bool> fail_next_allocation{false};
std::atomic<unsigned> large_allocations{0};
struct Allocation {
  void* pointer = nullptr;
  std::size_t bytes = 0;
};
std::array<Allocation, 256> allocations;
bool track_owners = false;
void track_allocation(void* pointer, std::size_t bytes) {
  if (!track_owners)
    return;
  for (auto& record : allocations)
    if (!record.pointer) {
      record = {pointer, bytes};
      return;
    }
  std::abort();
}
void track_free(void* pointer) {
  if (!pointer)
    return;
  for (auto& record : allocations)
    if (record.pointer == pointer) {
      record = {};
      return;
    }
}
std::size_t retained_allocations() {
  std::size_t bytes = 0;
  for (const auto& record : allocations)
    bytes += record.bytes;
  return bytes;
}
void record(std::size_t size) {
  if (inspect_allocations.load() && size > 16384)
    ++large_allocations;
}
}  // namespace
// The target statically links the kernel, so this observes its actual C++
// allocation calls, including any accidental ordinary STL metadata copy.
void* operator new(std::size_t size) {
  if (fail_next_allocation.exchange(false))
    throw std::bad_alloc();
  record(size);
  if (void* p = std::malloc(size ? size : 1)) {
    track_allocation(p, size);
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  track_free(p);
  std::free(p);
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void operator delete[](void* p) noexcept {
  ::operator delete(p);
}
void operator delete(void* p, std::size_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  ::operator delete(p);
}
void* operator new(std::size_t size, std::align_val_t value) {
  record(size);
  void* p = nullptr;
  const auto alignment = static_cast<std::size_t>(value);
#if defined(_WIN32)
  p = _aligned_malloc(size ? size : 1, alignment);
#else
  if (posix_memalign(&p, alignment, size ? size : 1) != 0)
    p = nullptr;
#endif
  if (!p)
    throw std::bad_alloc();
  track_allocation(p, size);
  return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
  track_free(p);
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete[](void* p, std::align_val_t alignment) noexcept {
  ::operator delete(p, alignment);
}
void operator delete(void* p, std::size_t,
                     std::align_val_t alignment) noexcept {
  ::operator delete(p, alignment);
}
void operator delete[](void* p, std::size_t,
                       std::align_val_t alignment) noexcept {
  ::operator delete(p, alignment);
}

namespace {
int weak_windows() {
  using namespace ps;  // NOLINT(build/namespaces)
  struct ComparableOwner {
    ResourceLease lease;
    std::shared_ptr<const void> a, b;
  };
  std::weak_ptr<ComparableOwner> control;
  track_owners = true;
  {
    auto owner = std::shared_ptr<ComparableOwner>(new ComparableOwner());
    control = owner;
  }
  track_owners = false;
  const auto control_bytes = retained_allocations();
  PS_CHECK(control.expired() && control_bytes > 0);
  control.reset();
  PS_CHECK(retained_allocations() == 0);
  ResourceBudget budget;
  SchemaTemplate schema;
  schema.id = "test.window_owner";
  schema.fields = {{"byte", ElementType::UInt8, {}, {}}};
  auto builder = ResultBuilder::start(budget, schema, "window").take_value();
  auto relation = ResultRelation::cartesian(budget, 1, {0, 15, 0, 0},
                                            DependencyGuarantee::Conservative)
                      .take_value();
  PS_CHECK(builder.bind_descriptor_relation(relation).ok());
  std::uint8_t byte = 7;
  PS_CHECK(builder.append(0, 1, ByteView(&byte, 1)).ok());
  PS_CHECK(builder.publish(0, 1, relation, {true, true, true, true}).ok());
  auto result = builder.seal().take_value();
  auto plan =
      result.prepare_read(result.descriptor().value(), 0, 0, 1).take_value();
  const auto baseline = budget.statistics().live[ResourceKind::Host];
  std::array<std::weak_ptr<const CpuStorage>, 32> weak;
  for (std::size_t i = 0; i < weak.size(); ++i) {
    track_owners = true;
    {
      auto window = plan.load(1).take_value();
      weak[i] = window;
    }
    track_owners = false;
    PS_CHECK(weak[i].expired());
    PS_CHECK(budget.statistics().live[ResourceKind::Host] == baseline);
    // Only the measured ordinary weak control block remains. Coallocating a
    // WindowOwner into it would retain additional uncharged object bytes.
    PS_CHECK(retained_allocations() == control_bytes * (i + 1));
  }
  for (auto& item : weak)
    item.reset();
  PS_CHECK(retained_allocations() == 0);
  return 0;
}
int failure_copy_exhaustion() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.failure";
  schema.fields = {
      {"rows", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}}};
  auto builder =
      ResultBuilder::start(root, schema, "copy-failure").take_value();
  auto reference = builder.reference();
  Status failure{ErrorCode::InvalidArgument,
                 std::string(128, 'x'),
                 FailureReason::MalformedEnvelope,
                 {FailureOrigin::Protocol, FailureScope::Group}};
  failure.detail.node_id = 19;
  builder.fail(failure);
  fail_next_allocation = true;
  auto observed = reference.production_status();
  PS_CHECK(!fail_next_allocation && observed.message.empty() &&
           observed.reason == failure.reason && observed.detail.node_id == 19);
  execution_internal::SharedResults table;
  auto a = table.join_call("snapshot", root, {}, {"r"}).take_value();
  auto producer = table.acquire("r", root, {}, a).take_value();
  auto b = table.join_call("snapshot", root, {}, {"r"}).take_value();
  auto peer = table.acquire("r", root, {}, b).take_value();
  producer.fail(failure);
  fail_next_allocation = true;
  auto waiting = peer.wait(true, 0, 0, {});
  PS_CHECK(!fail_next_allocation && !waiting.ok() &&
           waiting.status().message.empty() &&
           waiting.status().reason == failure.reason &&
           waiting.status().detail.node_id == 19);
  return 0;
}
int callback_failure_copy_exhaustion() {
  using namespace ps;  // NOLINT(build/namespaces)
  struct State {
    Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
      Result<ResultProgramPoll> failed(
          Status{ErrorCode::InvalidArgument,
                 std::string(128, 'x'),
                 FailureReason::MalformedEnvelope,
                 {FailureOrigin::Protocol, FailureScope::Group}});
      fail_next_allocation = true;
      return failed;
    }
  };
  SchemaTemplate schema;
  schema.id = "test.callback_failure";
  schema.fields = {
      {"rows", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}}};
  OperationDefinition op;
  op.key = "test.callback_failure";
  auto& output = op.traits.outputs[0];
  output.dependency_version = 2;
  output.region_rule = OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(State);
  output.maximum_dependency_stages = 1;
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = schema;
  op.start_result = [](const ResultProgramQuery&, const BufferAllocator& host) {
    return ResultContinuation::make<State>(host);
  };
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->register_operation(std::move(op)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.nodes = {{41, "test.callback_failure", {}, {}}};
  doc.outputs = {{"sink", 41, "value"}};
  GraphContext graph(doc);
  auto compiled = Compiler(registry).compile(graph).take_value();
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto result = context.execute(compiled.plan);
  PS_CHECK(!fail_next_allocation && !result.ok() &&
           result.status().reason == FailureReason::MalformedEnvelope &&
           result.status().detail.origin == FailureOrigin::Protocol &&
           result.status().detail.node_id == 41);
  return 0;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(failure_copy_exhaustion() == 0);
  PS_CHECK(callback_failure_copy_exhaustion() == 0);
  inspect_allocations = true;
  void* control = ::operator new(32768);
  inspect_allocations = false;
  ::operator delete(control);
  PS_CHECK(large_allocations == 1);
  SchemaTemplate schema;
  schema.id = "test.large_metadata";
  schema.fields = {
      {"empty", ElementType::UInt8, {ResultExtentKind::RuntimeCount}, {}}};
  for (unsigned i = 0; i < 16; ++i) {
    ResultFacet facet;
    facet.key = "facet_" + std::to_string(i);
    facet.payload.assign(65536, static_cast<std::uint8_t>(i));
    schema.metadata.push_back(std::move(facet));
  }
  PS_CHECK(schema.validate(true).ok());
  PS_CHECK(schema.canonical_size() == schema.canonical().size());
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 16384;
  limits.capacity[ResourceKind::Metadata] = 16384;
  ResourceBudget budget(limits);
  large_allocations = 0;
  inspect_allocations = true;
  auto result = ResultBuilder::start(budget, std::move(schema), "scope");
  inspect_allocations = false;
  PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(large_allocations == 0);
  PS_CHECK(budget.statistics().peak[ResourceKind::Host] <= 16384);
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  PS_CHECK(weak_windows() == 0);
  return 0;
}
