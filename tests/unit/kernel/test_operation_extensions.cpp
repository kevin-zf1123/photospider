#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "photospider/plugin/operation_extensions.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
ExtensionIdentity identity(const char* key) {
  return {key, 1, "fixture/1"};
}
DiagnosticDefinition sum(const char* key) {
  DiagnosticDefinition definition;
  definition.identity = identity(key);
  definition.payload_bytes = sizeof(std::uint64_t);
  definition.validate = [](ByteView value) {
    return value.size() == 8 ? Status::success()
                             : Status{ErrorCode::InvalidArgument, {}};
  };
  definition.merge = [](ByteView a, ByteView b, DiagnosticDestination out) {
    std::uint64_t x, y;
    std::memcpy(&x, a.data(), 8);
    std::memcpy(&y, b.data(), 8);
    if (y > UINT64_MAX - x)
      return Status{ErrorCode::ResourceExhausted, "counter overflow"};
    x += y;
    std::memcpy(out.data, &x, 8);
    return Status::success();
  };
  return definition;
}
Result<ResultProgramPoll> unused(const ResultProgramPhase&) {
  return Result<ResultProgramPoll>(Status{ErrorCode::OperationFailed, {}});
}
int diagnostics() {
  ResourceBudget root;
  auto type = std::make_shared<const DiagnosticDefinition>(sum("fixture.sum"));
  DiagnosticAccumulator total(root), report(root);
  PS_CHECK(total.initialize({type}).ok());
  PS_CHECK(report.initialize({type}).ok());
  std::uint64_t value = 4;
  const ByteView bytes(reinterpret_cast<const std::uint8_t*>(&value), 8);
  PS_CHECK(total.report(type->identity, bytes).ok());
  PS_CHECK(report.report(type->identity, bytes).ok());
  PS_CHECK(total.merge(report).ok());
  std::uint64_t observed = 0;
  auto read = total.find(type->identity);
  PS_CHECK(read.ok());
  std::memcpy(&observed, read.value().data(), 8);
  PS_CHECK(observed == 8);
  value = UINT64_MAX;
  PS_CHECK(!total.report(type->identity, bytes).ok());
  std::memcpy(&observed, total.find(type->identity).value().data(), 8);
  PS_CHECK(observed == 8);
  PS_CHECK(!total.initialize({type, type}).ok());
  PS_CHECK(total.find(type->identity).ok());
  PS_CHECK(!total.report(identity("missing"), bytes).ok());
  total.clear();
  PS_CHECK(total.find(type->identity).status().code == ErrorCode::NotFound);
  return 0;
}
int validators() {
  OperationRegistry registry;
  auto schema = multi_result::schema();
  OperationDefinition operation;
  operation.key = "fixture.validation";
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].output_schema.validators = {
      identity("fixture.range")};
  operation.traits.diagnostics = {identity("fixture.sum")};
  operation.start_result = [](const auto&, const auto&) {
    return ResultContinuation::stateless<unused>();
  };
  PS_CHECK(registry.register_operation(std::move(operation)).ok());
  PS_CHECK(!registry.freeze().ok() && !registry.frozen());
  ValidationDefinition validator;
  validator.identity = identity("fixture.range");
  validator.prepare = [](const ValidationPreparation& preparation) {
    if (!preparation.output || preparation.port != 0 ||
        preparation.schema.tensors.size() != 1)
      return Result<std::shared_ptr<const void>>(
          Status{ErrorCode::TypeMismatch, {}});
    return Result<std::shared_ptr<const void>>(std::make_shared<const int>(17));
  };
  validator.validate = [](const void* state, const ValidationContext&) {
    return *static_cast<const int*>(state) == 17
               ? Status::success()
               : Status{ErrorCode::Internal, {}};
  };
  PS_CHECK(registry.register_validator(validator).ok());
  PS_CHECK(!registry.register_validator(validator).ok());
  PS_CHECK(registry.register_diagnostic(sum("fixture.sum")).ok());
  PS_CHECK(registry.freeze().ok());
  validator.identity = identity("late");
  PS_CHECK(!registry.register_validator(validator).ok());
  auto prepared = registry.prepare_operation("fixture.validation", {}, {});
  PS_CHECK(prepared.ok() &&
           prepared.value()->diagnostic_definitions().size() == 1);
  ResourceBudget root;
  CancellationToken token;
  ValidationContext context{schema, root, token};
  context.consume = [&](auto n) { return root.consume({n}); };
  PS_CHECK(prepared.value()->validate_port(true, 0, context).ok());
  PS_CHECK(!prepared.value()->validate_port(true, 1, context).ok());
  return 0;
}
int concurrent_validator_lifetime() {
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered = 0;
    bool release = false;
  };
  auto gate = std::make_shared<Gate>();
  std::atomic<unsigned> destroyed{0};
  auto owner = std::shared_ptr<const void>(new int(1), [&](const void* p) {
    delete static_cast<const int*>(p);
    ++destroyed;
  });
  auto registry = std::make_shared<OperationRegistry>();
  ValidationDefinition validator;
  validator.owner = owner;
  validator.identity = identity("fixture.concurrent");
  validator.scratch_bytes = 4096;
  validator.prepare = [](const auto&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [gate](const void*, const ValidationContext& context) {
    std::unique_lock<std::mutex> lock(gate->mutex);
    ++gate->entered;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&] { return gate->release; });
    return context.consume(1);
  };
  PS_REQUIRE_OK(registry->register_validator(validator));
  auto op = multi_result::schema();
  OperationDefinition operation;
  operation.key = "fixture.concurrent";
  operation.traits.outputs = {multi_result::output("value", op)};
  operation.traits.outputs[0].output_schema.validators = {validator.identity};
  operation.start_result = [](const auto&, const auto&) {
    return ResultContinuation::stateless<unused>();
  };
  PS_REQUIRE_OK(registry->register_operation(operation));
  PS_REQUIRE_OK(registry->freeze());
  auto prepared =
      registry->prepare_operation(operation.key, {}, {}).take_value();
  ResourceBudget root;
  CancellationToken token;
  ValidationContext context{op, root, token};
  context.consume = [&](auto n) { return root.consume({n}); };
  auto a = std::async(std::launch::async, [prepared, &context] {
    return prepared->validate_port(true, 0, context);
  });
  auto b = std::async(std::launch::async, [prepared, &context] {
    return prepared->validate_port(true, 0, context);
  });
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->changed.wait(lock, [&] { return gate->entered == 2; });
  }
  registry.reset();
  prepared.reset();
  validator = {};
  owner.reset();
  PS_CHECK(destroyed == 0 &&
           root.statistics().live[ResourceKind::Host] >= 8192);
  {
    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->release = true;
  }
  gate->changed.notify_all();
  PS_REQUIRE_OK(a.get());
  PS_REQUIRE_OK(b.get());
  PS_CHECK(destroyed == 1);
  return 0;
}
int diagnostic_transaction() {
  ResourceBudget root;
  auto a = std::make_shared<const DiagnosticDefinition>(sum("fixture.a"));
  auto b = std::make_shared<const DiagnosticDefinition>(sum("fixture.b"));
  DiagnosticAccumulator left(root), right(root);
  PS_REQUIRE_OK(left.initialize({a, b}));
  PS_REQUIRE_OK(right.initialize({a, b}));
  std::uint64_t small = 7, large = UINT64_MAX;
  ByteView one(reinterpret_cast<const std::uint8_t*>(&small), 8);
  ByteView overflow(reinterpret_cast<const std::uint8_t*>(&large), 8);
  PS_REQUIRE_OK(left.report(a->identity, one));
  PS_REQUIRE_OK(left.report(b->identity, one));
  PS_REQUIRE_OK(right.report(a->identity, one));
  PS_REQUIRE_OK(right.report(b->identity, overflow));
  PS_CHECK(!left.merge(right).ok());
  for (auto type : {a, b}) {
    std::uint64_t value = 0;
    auto bytes = left.find(type->identity).take_value();
    std::memcpy(&value, bytes.data(), 8);
    PS_CHECK(value == 7);
  }
  auto invalid = sum("fixture.throw");
  invalid.merge = [](auto, auto, auto) -> Status {
    throw std::runtime_error("codec failed");
  };
  DiagnosticAccumulator thrown(root);
  PS_REQUIRE_OK(thrown.initialize(
      {std::make_shared<const DiagnosticDefinition>(invalid)}));
  PS_CHECK(thrown.report(invalid.identity, one).reason ==
           FailureReason::HostException);
  return 0;
}
}  // namespace
int main() {
  if (concurrent_validator_lifetime() || diagnostic_transaction())
    return 1;
  if (diagnostics())
    return 1;
  return validators();
}
