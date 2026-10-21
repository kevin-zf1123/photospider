#include <atomic>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct NullDiagnostic final : std::exception {
  const char* what() const noexcept override { return nullptr; }
};
[[noreturn]] void raise(unsigned mode) {
  if (mode == 0)
    throw std::runtime_error("kernel fixture exception");
  if (mode == 1)
    throw NullDiagnostic();
  if (mode == 2)
    throw std::bad_alloc();
  throw 17;
}
bool expected(const Status& status, unsigned mode) {
  if (mode == 2)
    return status.code == ErrorCode::ResourceExhausted;
  return status.code == ErrorCode::OperationFailed &&
         status.reason == FailureReason::HostException &&
         (mode != 0 ||
          status.message.find("kernel fixture exception") != std::string::npos);
}
enum class Boundary { Lower, Prepare, Invoke };
struct Counts {
  std::atomic<unsigned> lower{0}, prepare{0}, invoke{0}, destroyed{0};
};
OperationDefinition definition(unsigned mode, Boundary boundary,
                               const std::shared_ptr<Counts>& counts) {
  OperationDefinition op;
  op.key = "test.exception";
  op.traits.outputs = {multi_result::output("value")};
  op.lower = [mode, boundary, counts](const OperationLowering& query) {
    ++counts->lower;
    if (boundary == Boundary::Lower)
      raise(mode);
    OperationProgramBuilder builder(query);
    auto task = take(builder.add_task("throw", "throw-v1"));
    check(builder.write_output(task, 0, 0, 0));
    return builder.finish();
  };
  op.prepare_kernel =
      [mode, boundary, counts](
          const physical::KernelDesc&, const physical::Placement&,
          const KernelPreparation&, const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    ++counts->prepare;
    if (boundary == Boundary::Prepare)
      raise(mode);
    struct State {
      unsigned mode;
      std::shared_ptr<Counts> counts;
      ~State() { ++counts->destroyed; }
    };
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = new State{mode, counts};
    kernel.release = [](const void* state) {
      delete static_cast<const State*>(state);
    };
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8*) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      ++state.counts->invoke;
      raise(state.mode);
    };
    return Result<ps_prepared_kernel_v8>(kernel);
  };
  return op;
}
int workflow(unsigned mode, Boundary boundary) {
  auto counts = std::make_shared<Counts>();
  {
    auto registry = std::make_shared<OperationRegistry>();
    check(registry->register_operation(definition(mode, boundary, counts)));
    check(registry->freeze());
    WorkflowDocument document;
    document.nodes = {{41, "test.exception", {}, {}}};
    document.outputs = {{"value", 41, "value"}};
    GraphContext graph(document);
    auto compiled = Compiler(registry).compile(graph);
    if (boundary == Boundary::Lower) {
      PS_CHECK(!compiled.ok() && expected(compiled.status(), mode));
      PS_CHECK(counts->lower == 1 && !counts->prepare && !counts->invoke);
      return 0;
    }
    PS_REQUIRE_OK(compiled);
    ExecutionContext context(registry);
    auto root = take(context.resource_budget());
    auto prepared = context.prepare(compiled.value().plan.value());
    if (boundary == Boundary::Prepare) {
      PS_CHECK(!prepared.ok() && expected(prepared.status(), mode));
      PS_CHECK(counts->prepare == 1 && !counts->invoke && !counts->destroyed);
      return 0;
    }
    PS_REQUIRE_OK(prepared);
    for (unsigned i = 1; i <= 2; ++i) {
      {
        std::vector<CallRecord> records;
        ExecutionOptions options;
        options.on_call_retired = [&](const CallRecord& record) {
          records.push_back(record);
        };
        auto run = take(context.submit(prepared.value(), {}, {}, options));
        auto result = run.join();
        PS_CHECK(!result.ok() && expected(result.status(), mode));
        PS_CHECK(result.status().detail.node_id == 41);
        PS_CHECK(records.size() == 1 &&
                 records[0].outcome == CallOutcome::Failed &&
                 records[0].values.empty());
        auto again = run.join();
        PS_CHECK(!again.ok() && again.status().code == result.status().code &&
                 again.status().reason == result.status().reason &&
                 again.status().message == result.status().message);
        PS_CHECK(counts->lower == 1 && counts->prepare == 1 &&
                 counts->invoke == i && !counts->destroyed);
      }
      PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    }
  }
  PS_CHECK(counts->destroyed == 1);
  return 0;
}
}  // namespace
int main() {
  for (unsigned mode = 0; mode < 4; ++mode)
    for (auto boundary : {Boundary::Lower, Boundary::Prepare, Boundary::Invoke})
      PS_CHECK(workflow(mode, boundary) == 0);
  return 0;
}
