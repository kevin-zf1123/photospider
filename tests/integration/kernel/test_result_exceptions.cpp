#include <atomic>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;
struct NullDiagnostic final : std::exception {
  const char* what() const noexcept override { return nullptr; }
};
[[noreturn]] void raise(unsigned mode) {
  if (mode == 0)
    throw std::runtime_error("Result fixture exception");
  if (mode == 1)
    throw NullDiagnostic();
  if (mode == 2)
    throw std::bad_alloc();
  throw 17;
}
bool expected(const Status& status, unsigned mode, bool sticky) {
  if (sticky || mode == 2)
    return status.code == ErrorCode::ResourceExhausted;
  const char* message = mode == 0 ? "Result fixture exception"
                        : mode == 1
                            ? ""
                            : "operation raised a nonstandard exception";
  return status.code == ErrorCode::OperationFailed &&
         status.reason == FailureReason::HostException &&
         status.message == message;
}
struct Counts {
  unsigned polls = 0, destroyed = 0, scalar_starts = 0;
};
struct ThrowingState {
  unsigned mode;
  bool sticky;
  Counts* counts;
  ThrowingState(unsigned mode, bool sticky, Counts* counts)
      : mode(mode), sticky(sticky), counts(counts) {}
  ~ThrowingState() { ++counts->destroyed; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    if (sticky) {
      auto rejected = phase.allocator.allocate(9);
      if (rejected.ok())
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "expected bounded allocator failure"});
    }
    raise(mode);
  }
};
OperationDefinition definition(unsigned mode, bool start, bool sticky,
                               Counts* counts) {
  OperationDefinition op;
  op.key = "test.result_exception";
  op.traits.outputs = {multi_result::output("value")};
  op.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  op.traits.outputs[0].continuation_bytes = sizeof(ThrowingState);
  op.traits.workspace_bytes = 8;
  op.traits.cacheable = false;
  op.start_result = [=](const auto&, const BufferAllocator& allocator) {
    ++counts->scalar_starts;
    if (start) {
      if (sticky)
        static_cast<void>(allocator.allocate(sizeof(ThrowingState) + 1));
      raise(mode);
    }
    return ResultContinuation::make<ThrowingState>(allocator, mode, sticky,
                                                   counts);
  };
  return op;
}
int direct(unsigned mode, bool start, bool sticky,
           bool throwing_observer = false) {
  Counts counts;
  ResourceBudget root;
  {
    ResourceAllocationScope scope(root);
    OperationRegistry registry;
    PS_CHECK(
        registry.register_operation(definition(mode, start, sticky, &counts))
            .ok());
    PS_CHECK(registry.freeze().ok());
    ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const SchemaTemplate>(multi_result::schema());
    const std::map<std::string, ParameterValue> parameters;
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "test.result_exception.left";
    query.snapshot_identity = "exception.snapshot";
    ResultObjectInputs objects;
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    auto allocator = root.allocator().limited(8, [failure](ErrorCode code) {
      auto ok = ErrorCode::Ok;
      failure->compare_exchange_strong(ok, code);
    });
    ResultProgramPhase phase{
        query,
        objects,
        allocator,
        root,
        [&](std::uint64_t amount) { return root.consume({amount}); },
        failure};
    if (throwing_observer) {
      phase.failure_observer = [](const Status&) {
        throw std::runtime_error("observer must not replace producer failure");
      };
    }
    auto begun =
        registry.start_result("test.result_exception", query, root.allocator());
    if (start) {
      PS_CHECK(!begun.ok() && expected(begun.status(), mode, sticky));
    } else {
      PS_CHECK(begun.ok());
      auto continuation = begun.take_value();
      auto polled = continuation.poll(phase);
      PS_CHECK(!polled.ok() && expected(polled.status(), mode, sticky));
      auto repeated = continuation.poll(phase);
      PS_CHECK(!repeated.ok() &&
               repeated.status().code == polled.status().code);
      PS_CHECK(counts.polls == 1);
    }
  }
  PS_CHECK(counts.destroyed == (start ? 0U : 1U));
  PS_CHECK(counts.scalar_starts == 1);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
struct ServiceFailure {
  unsigned mode;
  Counts* counts;
  ServiceFailure(unsigned mode, Counts* counts) : mode(mode), counts(counts) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    double value = 0;
    auto failed = phase.read_tensor(0, 0, {0}, &value, sizeof(value));
    if (mode == 0)
      return Result<ResultProgramPoll>(failed);
    if (mode == 1)
      return Result<ResultProgramPoll>(ResultProgramNeed{});
    return Result<ResultProgramPoll>(
        Status{ErrorCode::ResourceExhausted, "later callback failure"});
  }
};
int direct_service_failure(unsigned mode) {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  Counts counts;
  OperationRegistry registry;
  OperationDefinition op;
  op.key = "test.service_failure";
  op.traits.outputs = {multi_result::output("value")};
  op.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  op.traits.outputs[0].continuation_bytes = sizeof(ServiceFailure);
  op.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<ServiceFailure>(allocator, mode, &counts);
  };
  PS_CHECK(registry.register_operation(std::move(op)).ok());
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(multi_result::schema());
  const std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "direct.service";
  auto continuation = take(
      registry.start_result("test.service_failure", query, root.allocator()));
  ResultObjectInputs inputs;
  const auto allocator = root.allocator();
  auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  ResultProgramPhase phase{
      query,
      inputs,
      allocator,
      root,
      [&](std::uint64_t work) { return root.consume({work}); },
      failure};
  auto result = continuation.poll(phase);
  PS_CHECK(!result.ok() && result.status().code == ErrorCode::InvalidArgument);
  if (mode == 0) {
    PS_CHECK(result.status().reason == FailureReason::UnauthorizedRead);
    PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol);
    PS_CHECK(result.status().detail.scope == FailureScope::Group);
    PS_CHECK(result.status().message == "missing authorized image input");
  }
  PS_CHECK(continuation.poll(phase).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(counts.polls == 1);
  return 0;
}
int workflow(unsigned mode, bool start, bool sticky) {
  Counts counts;
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(
      registry->register_operation(definition(mode, start, sticky, &counts))
          .ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.nodes = {{1, "test.result_exception", {}, {}}};
  doc.outputs = {{"value", 1, "value"}};
  GraphContext graph(doc);
  auto plan = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto result = context.execute(plan.plan);
    if (result.ok() || !expected(result.status(), mode, sticky))
      std::cerr << "mode=" << mode << " start=" << start << " sticky=" << sticky
                << " code=" << static_cast<unsigned>(result.status().code)
                << " reason=" << static_cast<unsigned>(result.status().reason)
                << " message=" << result.status().message << '\n';
    PS_CHECK(!result.ok() && expected(result.status(), mode, sticky));
  }
  auto root = take(context.resource_budget());
  PS_CHECK(counts.scalar_starts == 2);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
}  // namespace
int main() try {
  for (unsigned mode = 0; mode < 4; ++mode)
    for (bool start : {false, true})
      for (bool sticky : {false, true}) {
        PS_CHECK(direct(mode, start, sticky) == 0);
        PS_CHECK(workflow(mode, start, sticky) == 0);
      }
  for (unsigned mode = 0; mode < 3; ++mode)
    PS_CHECK(direct_service_failure(mode) == 0);
  PS_CHECK(direct(0, false, false, true) == 0);
  PS_CHECK(direct(0, false, true, true) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
