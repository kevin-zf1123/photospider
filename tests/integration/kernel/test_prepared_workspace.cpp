#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <utility>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Probe {
  std::atomic<unsigned> prepared{0}, linked{0}, completed{0};
};
OperationDefinition operation(const std::shared_ptr<Probe>& probe) {
  const auto schema = multi_result::schema(ElementType::UInt8);
  OperationDefinition op;
  op.key = "test.scratch";
  op.traits.outputs = {multi_result::output("value", schema)};
  op.traits.requires_metadata_specialization = true;
  op.traits.parameter_schema = {
      {"bytes", OperationParameterType::Int64, true, true, 0, 4096}};
  op.prepare_static = [schema, probe](const auto&, const auto& parameters) {
    ++probe->prepared;
    OperationPreparation result;
    result.outputs.resize(1);
    result.outputs[0].metadata.result_schema =
        std::make_shared<const SchemaTemplate>(schema);
    result.state = std::make_shared<const std::uint64_t>(
        std::get<std::int64_t>(parameters.at("bytes")));
    return Result<OperationPreparation>(std::move(result));
  };
  op.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    const auto bytes = *static_cast<const std::uint64_t*>(query.static_state);
    if (!query.requests[0].empty()) {
      auto task =
          take(builder.add_task("scratch", "scratch-v1", {}, {bytes, 0, 0}));
      check(builder.write_output(task, 0, 0, 0));
    }
    return builder.finish();
  };
  op.prepare_kernel =
      [probe](
          const physical::KernelDesc& descriptor,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    const auto bytes = *static_cast<const std::uint64_t*>(query.static_state);
    if (descriptor.symbol != "scratch" || descriptor.contract != "scratch-v1" ||
        placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host ||
        query.scratch != std::array<std::uint64_t, 3>{bytes, 0, 0} ||
        !query.reads.empty() || query.writes.size() != 1 ||
        query.writes[0].bytes != 1)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid scratch signature"});
    struct State {
      std::uint64_t bytes;
      std::shared_ptr<Probe> probe;
    };
    ++probe->linked;
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = new State{bytes, probe};
    kernel.release = [](const void* state) {
      delete static_cast<const State*>(state);
    };
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      // Check the host's scratch binding before exercising its storage.
      if (invocation->scratch_count != (state.bytes ? 1U : 0U) ||
          (state.bytes &&
           (invocation->scratch[0].bytes != state.bytes ||
            invocation->scratch[0].domain != PS_KERNEL_DOMAIN_HOST_V8 ||
            !invocation->scratch[0].allocation))) {
        *status =
            kernel_failure(ErrorCode::Internal, "incorrect scratch binding");
        return;
      }
      auto* output =
          static_cast<std::uint8_t*>(invocation->outputs[0].allocation) +
          invocation->outputs[0].byte_offset;
      *output = 0;
      if (state.bytes) {
        auto* scratch =
            static_cast<std::uint8_t*>(invocation->scratch[0].allocation);
        std::memset(scratch, 17, state.bytes);
        *output = scratch[state.bytes - 1];
      }
      ++state.probe->completed;
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(kernel);
  };
  return op;
}
int retained_output_admission() {
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation(probe)));
  PS_REQUIRE_OK(registry->freeze());
  ResourceBudget root;
  {
    WorkflowDocument document;
    document.nodes = {{1, "test.scratch", {}, {{"bytes", INT64_C(32)}}}};
    document.outputs = {{"value", 1, "value"}};
    GraphContext graph(document);
    auto compiled = Compiler(registry).compile(graph);
    PS_REQUIRE_OK(compiled);
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 33;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    auto prepared = context.prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(prepared);
    ResultRef retained;
    {
      auto submitted = context.submit(prepared.value());
      PS_REQUIRE_OK(submitted);
      auto completed = submitted.value().join();
      PS_REQUIRE_OK(completed);
      retained = completed.value().results.at("value");
    }
    PS_CHECK(probe->completed == 1);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 1);
    {
      auto submitted = context.submit(prepared.value());
      const auto status = submitted.ok() ? submitted.value().join().status()
                                         : submitted.status();
      PS_CHECK(status.code == ErrorCode::ResourceExhausted);
    }
    PS_CHECK(probe->completed == 1);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 1);
    retained = {};
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    {
      auto submitted = context.submit(prepared.value());
      PS_REQUIRE_OK(submitted);
      auto completed = submitted.value().join();
      PS_REQUIRE_OK(completed);
      const auto& output = completed.value().results.at("value");
      std::uint8_t value = 0;
      PS_REQUIRE_OK(
          output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1));
      PS_CHECK(value == 17 && probe->completed == 2);
    }
    PS_CHECK(probe->prepared == 1 && probe->linked == 1);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    PS_CHECK(root.statistics().peak[ResourceKind::Payload] == 33);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int preparation_metadata() {
  auto probe = std::make_shared<Probe>();
  struct Borrowed {
    std::array<const void*, 4> addresses{};
    bool reject = false;
  };
  auto borrowed = std::make_shared<Borrowed>();
  auto definition = operation(probe);
  auto base = definition.prepare_kernel;
  definition.prepare_kernel = [base, borrowed](
                                  const auto& kernel, const auto& placement,
                                  const auto& query, const auto& resources,
                                  const auto* device) {
    const std::array<const void*, 4> addresses{&query.operation, &query.inputs,
                                               &query.parameters,
                                               &query.task_parameters};
    if (!borrowed->addresses[0])
      borrowed->addresses = addresses;
    else if (borrowed->addresses != addresses)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::Internal, "preparation input owner changed"});
    if (borrowed->reject)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::ResourceExhausted, "injected preparation failure"});
    return base(kernel, placement, query, resources, device);
  };
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(std::move(definition)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.scratch", {}, {{"bytes", INT64_C(32)}}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  const auto baseline = root.statistics().allocated_metadata_bytes;
  auto first = take(context.prepare(*compiled.value().plan));
  const auto retained = root.statistics().allocated_metadata_bytes - baseline;
  PS_CHECK(retained > ResourceBudget::lease_metadata_bytes());
  auto copy = first;
  PS_CHECK(root.statistics().allocated_metadata_bytes == baseline + retained);
  // A second host thread has a distinct query stack but borrows the same
  // immutable owners. Independent preparations own independent host tables.
  auto second = take(std::async(std::launch::async, [&] {
                       return context.prepare(*compiled.value().plan);
                     }).get());
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           baseline + 2 * retained);
  first = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           baseline + 2 * retained);
  copy = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == baseline + retained);
  second = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
  borrowed->reject = true;
  auto failed = context.prepare(*compiled.value().plan);
  PS_CHECK(!failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
  return 0;
}
}  // namespace
int main() {
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(operation(probe)));
  check(registry->freeze());
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  for (std::int64_t bytes : {0, 4096}) {
    WorkflowDocument document;
    document.nodes = {{1, "test.scratch", {}, {{"bytes", bytes}}}};
    document.outputs = {{"value", 1, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan.value();
    PS_CHECK(plan.program().tasks[0].scratch[0] ==
             static_cast<unsigned>(bytes));
    auto prepared = take(context.prepare(plan));
    const auto preparations = probe->prepared.load();
    const auto links = probe->linked.load();
    for (unsigned i = 0; i < 2; ++i) {
      {
        auto run = take(context.submit(prepared));
        auto result = take(run.join());
        const auto& output = result.results.at("value");
        std::uint8_t value = 255;
        check(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1));
        PS_CHECK(value == (bytes ? 17 : 0));
        PS_CHECK(probe->prepared == preparations && probe->linked == links);
      }
      PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    }
    if (bytes) {
      auto limited_config = config;
      limited_config.maximum_live_bytes = 2048;
      ExecutionContext limited(registry, limited_config);
      auto executable = take(limited.prepare(plan));
      const auto before = probe->completed.load();
      auto submitted = limited.submit(executable);
      const auto status = submitted.ok() ? submitted.value().join().status()
                                         : submitted.status();
      PS_CHECK(status.code == ErrorCode::ResourceExhausted);
      PS_CHECK(probe->completed == before && probe->prepared == preparations);
      PS_CHECK(take(limited.resource_budget())
                   .statistics()
                   .live[ResourceKind::Payload] == 0);
    }
  }
  PS_CHECK(probe->completed == 4);
  auto invalid =
      registry->prepare_operation("test.scratch", {}, {{"bytes", INT64_C(-1)}});
  PS_CHECK(!invalid.ok() &&
           invalid.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(retained_output_admission() == 0);
  PS_CHECK(preparation_metadata() == 0);
  return 0;
}
