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

#include "execution/result_checkpoints.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
#if defined(PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
constexpr bool interpose_allocations = false;
#else
constexpr bool interpose_allocations = true;
#endif
std::atomic<bool> inspect_allocations{false};
std::atomic<bool> fail_next_allocation{false};
std::atomic<unsigned> large_allocations{0};
#if !defined(PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
void record(std::size_t size) {
  if (inspect_allocations.load() && size > 16384)
    ++large_allocations;
}
#endif
}  // namespace
// The target statically links the kernel, so this observes its actual C++
// allocation calls, including any accidental ordinary STL metadata copy.
// TSAN owns these symbols; that build retains budget and lifetime assertions
// but leaves allocator interception and fault injection to ordinary builds.
#if !defined(PHOTOSPIDER_TEST_NO_GLOBAL_ALLOCATOR_INTERPOSITION)
void* operator new(std::size_t size) {
  if (fail_next_allocation.exchange(false))
    throw std::bad_alloc();
  record(size);
  if (void* p = std::malloc(size ? size : 1)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
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
  return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
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
#endif

namespace {
int input_facts_lifetime() {
  using namespace ps;  // NOLINT(build/namespaces)
  using execution_internal::ResultInputFacts;
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 65536;
  limits.capacity[ResourceKind::Metadata] = 65536;
  ResourceBudget root(limits);
  {
    ResultInputFacts facts(root);
    for (unsigned port : {1, 0})
      for (unsigned id = 128; id; --id)
        facts[{port, id}] = port * 1000 + id;
    facts[{2, 1}] = 4;
    PS_CHECK(facts.size() == 257);
    ResultInputFacts copied = facts;
    copied[{1, 64}] = 9000;
    PS_CHECK((facts[{1, 64}] == 1064 && copied.size() == facts.size()));
    ResultInputFacts moved(std::move(copied));
    PS_CHECK(copied.size() == 0 && !(copied.begin() != copied.end()));
    copied[{4, 2}] = 7;
    copied = std::move(moved);
    PS_CHECK((moved.size() == 0 && copied[{1, 64}] == 9000));
    copied.clear();
    PS_CHECK(copied.size() == 0 && !(copied.begin() != copied.end()));
    const auto free = 65536 - root.statistics().live[ResourceKind::Host] -
                      ResourceBudget::lease_metadata_bytes();
    auto reservation = root.reserve(ResourceCapacity::host(free, free));
    PS_CHECK(reservation.ok());
    bool rejected = false;
    try {
      facts[{3, 9}] = 9;
    } catch (const std::bad_alloc&) {
      rejected = true;
    }
    PS_CHECK(rejected && facts.size() == 257);
    ResultInputFacts::Key previous{};
    std::size_t count = 0;
    for (const auto& fact : facts) {
      PS_CHECK(previous < fact.first);
      previous = fact.first;
      ++count;
    }
    PS_CHECK(count == facts.size());
    reservation = Result<ResourceLease>(ResourceLease{});
    facts[{3, 9}] = 9;
    PS_CHECK(facts.size() == 258);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Host] == 0);
  return 0;
}
int failure_copy_exhaustion() {
  using namespace ps;  // NOLINT(build/namespaces)
  ResourceBudget root;
  SchemaTemplate schema;
  schema.id = "test.failure";
  ResultTensorSpec tensor;
  tensor.key = "rows";
  tensor.descriptor = {ElementType::Int64, {1}};
  schema.tensors.push_back(std::move(tensor));
  auto builder =
      ResultBuilder::start(root, schema, "copy-failure").take_value();
  auto reference = builder.reference();
  Status failure{ErrorCode::InvalidArgument,
                 std::string(128, 'x'),
                 FailureReason::MalformedEnvelope,
                 {FailureOrigin::Protocol, FailureScope::Group}};
  failure.detail.node_id = 19;
  builder.fail(failure);
  fail_next_allocation = interpose_allocations;
  auto observed = reference.production_status();
  PS_CHECK(!fail_next_allocation && observed.reason == failure.reason &&
           observed.detail.node_id == 19);
  PS_CHECK(interpose_allocations ? observed.message.empty()
                                 : observed.message == failure.message);
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
      fail_next_allocation = interpose_allocations;
      return failed;
    }
  };
  SchemaTemplate schema;
  schema.id = "test.callback_failure";
  ResultTensorSpec tensor;
  tensor.key = "rows";
  tensor.descriptor = {ElementType::Int64, {1}};
  schema.tensors.push_back(std::move(tensor));
  OperationDefinition op;
  op.key = "test.callback_failure";
  auto& output = op.traits.outputs[0];
  output.continuation_bytes = sizeof(State);
  output.maximum_dependency_stages = 1;

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
           result.status().code == ErrorCode::InvalidArgument &&
           result.status().reason == FailureReason::MalformedEnvelope &&
           result.status().detail.origin == FailureOrigin::Protocol &&
           result.status().detail.scope == FailureScope::Group &&
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
  if constexpr (interpose_allocations)
    PS_CHECK(large_allocations == 1);
  SchemaTemplate schema;
  schema.id = "test.large_metadata";
  for (unsigned i = 0; i < 16; ++i) {
    ResultTensorSpec tensor;
    const auto key = "tensor_" + std::to_string(i) + std::string(100, 'x');
    tensor.key.assign(key.data(), key.size());
    tensor.descriptor = {ElementType::UInt8, {1, 1, 1, 1, 1, 1, 1, 1}};
    schema.tensors.push_back(std::move(tensor));
  }
  PS_CHECK(schema.validate(true).ok());
  PS_CHECK(schema.canonical_size() == schema.canonical().size());
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 1024;
  limits.capacity[ResourceKind::Metadata] = 1024;
  ResourceBudget budget(limits);
  large_allocations = 0;
  inspect_allocations = true;
  auto result = ResultBuilder::start(budget, std::move(schema), "scope");
  inspect_allocations = false;
  PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
  if constexpr (interpose_allocations)
    PS_CHECK(large_allocations == 0);
  PS_CHECK(budget.statistics().peak[ResourceKind::Host] <= 16384);
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  SchemaTemplate image_schema;
  image_schema.id = "test.large_image_metadata";
  ResultTensorSpec image;
  image.batch_axes = {1, 1};
  image.layout.spatial = true;
  image.key = "pixels";
  image.descriptor = {ElementType::Float32, {1, 1}};
  image.layout.channel_axis = {};
  image_schema.tensors.push_back(std::move(image));
  PS_CHECK(image_schema.validate(true).ok());
  large_allocations = 0;
  inspect_allocations = true;
  auto refused_image = ResultBuilder::start(budget, image_schema, "scope");
  inspect_allocations = false;
  PS_CHECK(refused_image.status().code == ErrorCode::ResourceExhausted);
  if constexpr (interpose_allocations)
    PS_CHECK(large_allocations == 0);
  for (auto live : budget.statistics().live.values)
    PS_CHECK(live == 0);
  PS_CHECK(input_facts_lifetime() == 0);
  return 0;
}
