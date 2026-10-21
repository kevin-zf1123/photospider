#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "support/status_check.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void check(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
template <class T>
T take(Result<T> result) {
  check(result.status());
  return result.take_value();
}
SchemaTemplate schema(ElementType type = ElementType::Float64,
                      std::vector<std::uint64_t> shape = {1}) {
  SchemaTemplate result;
  result.id = "test.prepared";
  ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {type, std::move(shape)};
  result.tensors.push_back(std::move(tensor));
  return result;
}
OperationOutputTraits output(std::string key) {
  OperationOutputTraits result;
  result.key = std::move(key);
  result.result_schema = schema();
  result.output_schema.result_schema_id = "test.prepared";
  result.output_schema.result_schema_version = 1;
  return result;
}
std::vector<OperationMetadata> metadata() {
  return {{std::make_shared<const SchemaTemplate>(schema())}};
}
std::map<std::string, ParameterValue> parameters(bool input) {
  return {{"input", input}, {"tag", 0.0}};
}
std::uint64_t bits(double value) {
  std::uint64_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned arrived = 0;
  bool held = false, released = false;
  bool meet() {
    std::unique_lock<std::mutex> lock(mutex);
    ++arrived;
    changed.notify_all();
    return changed.wait_for(lock, std::chrono::seconds(5), [&] {
      return arrived == 2 && (!held || released);
    });
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return arrived == 2; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
struct Counts {
  std::uint64_t rejected_object = 0;
  std::atomic<unsigned> prepared{0}, destroyed{0}, linked{0}, invoked{0},
      released{0};
  std::atomic<bool> invalid_release{false};
  std::shared_ptr<Gate> gate;
};
struct Program {
  std::shared_ptr<Counts> counts;
  bool input;
  std::uint64_t tag;
  std::shared_ptr<const SchemaTemplate> input_schema;
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  Program(std::shared_ptr<Counts> counts, bool input, std::uint64_t tag,
          std::shared_ptr<const SchemaTemplate> input_schema)
      : counts(std::move(counts)),
        input(input),
        tag(tag),
        input_schema(std::move(input_schema)) {}
  ~Program() {
    alive->store(false);
    ++counts->destroyed;
  }
};
struct Kernel {
  const Program* program;
  std::shared_ptr<Counts> counts;
  std::shared_ptr<std::atomic<bool>> alive;
  unsigned output;
  Kernel(const Program* program, unsigned output)
      : program(program),
        counts(program->counts),
        alive(program->alive),
        output(output) {}
  ~Kernel() {
    if (!alive->load())
      counts->invalid_release = true;
    ++counts->released;
  }
};
OperationDefinition definition(const std::shared_ptr<Counts>& counts) {
  OperationDefinition op;
  op.key = "test.prepared";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].tensor_key = "number";
  op.traits.input_schema[0].element_type =
      static_cast<unsigned>(ElementType::Float64);
  op.traits.input_schema[0].rank = 1;
  op.traits.requires_metadata_specialization = true;
  op.traits.parameter_schema = {{"input", OperationParameterType::Bool},
                                {"tag", OperationParameterType::Float64}};
  op.traits.outputs = {output("value"), output("constant")};
  op.prepare_static = [counts](const auto& inputs, const auto& parameters) {
    const bool input = std::get<bool>(parameters.at("input"));
    if (inputs.size() != 1 || !inputs[0].result_schema)
      return Result<OperationPreparation>(
          Status{ErrorCode::TypeMismatch, "complete inputs required"});
    OperationPreparation prepared;
    prepared.outputs.resize(2);
    for (unsigned i = 0; i < 2; ++i) {
      prepared.outputs[i].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(schema());
      prepared.outputs[i].input_indices = input && i == 0
                                              ? std::vector<std::uint32_t>{0}
                                              : std::vector<std::uint32_t>{};
    }
    prepared.state = std::make_shared<const Program>(
        counts, input, bits(std::get<double>(parameters.at("tag"))),
        inputs[0].result_schema);
    ++counts->prepared;
    return Result<OperationPreparation>(std::move(prepared));
  };
  op.lower = [](const OperationLowering& query) {
    const auto& state = *static_cast<const Program*>(query.static_state);
    if (query.inputs.size() != 1 ||
        !query.inputs[0].result_schema->same_schema(schema()) ||
        query.inputs[0].result_schema != state.input_schema ||
        bits(std::get<double>(query.parameters.at("tag"))) != state.tag ||
        std::get<bool>(query.parameters.at("input")) != state.input)
      return Result<OperationProgram>(
          Status{ErrorCode::Stale, "static snapshot changed"});
    OperationProgramBuilder builder(query);
    for (unsigned i = 0; i < query.requests.size(); ++i) {
      for (const auto& member : query.requests[i].members) {
        for (auto tile : member.second) {
          const auto task = take(builder.add_task("number", "number-v1"));
          if (state.input && i == 0)
            check(builder.read_input(task, 0, 0, tile));
          else
            check(builder.read_input_schema(task, 0));
          check(builder.write_output(task, i, member.first, tile));
        }
      }
    }
    return builder.finish();
  };
  op.prepare_kernel = [](const physical::KernelDesc& kernel,
                         const physical::Placement& placement,
                         const KernelPreparation& query, const ResourceSet&,
                         const ps_native_gpu_service_v2*) {
    const auto* state = static_cast<const Program*>(query.static_state);
    if (kernel.symbol != "number" || kernel.contract != "number-v1" ||
        placement.backend != physical::Backend::Cpu ||
        query.writes.size() != 1 || query.writes[0].port > 1 ||
        query.writes[0].bytes != sizeof(double) ||
        query.reads.size() !=
            static_cast<unsigned>(state->input && query.writes[0].port == 0) ||
        bits(std::get<double>(query.parameters.at("tag"))) != state->tag)
      return Result<ps_prepared_kernel_v8>(
          Status{ErrorCode::TypeMismatch, "kernel signature mismatch"});
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = new Kernel(state, query.writes[0].port);
    result.release = [](const void* context) {
      delete static_cast<const Kernel*>(context);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const Kernel*>(invocation->prepared_context);
      ++state.counts->invoked;
      if (!state.alive->load() ||
          (state.counts->gate && !state.counts->gate->meet())) {
        *status = kernel_failure(ErrorCode::OperationFailed,
                                 "lost owner or concurrent invocation timeout");
        return;
      }
      double value = 7 + state.output;
      if (state.program->input && state.output == 0) {
        if (state.counts->rejected_object &&
            invocation->input_object_ids[0] == state.counts->rejected_object) {
          *status =
              kernel_failure(ErrorCode::OperationFailed, "selected Run failed");
          return;
        }
        double source;
        const auto& input = invocation->inputs[0];
        std::memcpy(&source,
                    static_cast<const std::uint8_t*>(input.allocation) +
                        input.byte_offset,
                    sizeof(source));
        value += source;
      }
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          &value, sizeof(value));
      *status = {};
    };
    ++state->counts->linked;
    return Result<ps_prepared_kernel_v8>(result);
  };
  return op;
}
std::shared_ptr<OperationRegistry> registry(
    const std::shared_ptr<Counts>& counts) {
  auto operations = std::make_shared<OperationRegistry>();
  check(operations->register_operation(definition(counts)));
  check(operations->freeze());
  return operations;
}
WorkflowDocument document(bool input) {
  WorkflowDocument result;
  result.inputs = {{1, "input", metadata()[0].result_schema}};
  result.nodes = {
      {1, "test.prepared", {WorkflowInputReference{1}}, parameters(input)}};
  result.outputs = {{"value", 1, "value"}, {"constant", 1, "constant"}};
  return result;
}
ExecutionPlan plan(const std::shared_ptr<OperationRegistry>& operations,
                   const GraphContext& graph) {
  Compiler compiler(operations);
  auto ir = take(compiler.analyze(graph.snapshot()));
  PlanningOptions options;
  options.tile_extent = 1;
  return take(compiler.plan(ir, options));
}
ExecutionBindings binding(double value) {
  auto builder = take(
      ResultBuilder::start(ResourceBudget{}, schema(), "test.source", {}, 1));
  check(builder.publish_tensor(
      0, Region::whole({1}),
      ByteView(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value))));
  return {{{"input", take(builder.seal())}}};
}
double number(const ResultRef& result) {
  double value;
  check(result.read_tensor(take(result.descriptor()), 0, {0}, &value,
                           sizeof(value)));
  return value;
}
int compiled_reuse() {
  auto counts = std::make_shared<Counts>();
  {
    auto operations = registry(counts);
    GraphContext graph(document(true));
    Compiler compiler(operations);
    auto ir = take(compiler.analyze(graph.snapshot()));
    PlanningOptions options;
    options.tile_extent = 1;
    auto compiled = take(compiler.plan(ir, options));
    PS_CHECK(counts->prepared == 1);
    const auto& prepared = ir.nodes()[0].prepared;
    PS_CHECK(prepared == compiled.calls()[0].prepared);
    PS_CHECK(prepared->traits().outputs[0].input_indices ==
             std::optional<std::vector<std::uint32_t>>({{0}}));
    PS_CHECK(prepared->traits().outputs[1].input_indices->empty());
    auto selected = take(compiled.select_outputs(
        {{"value", compiled.output_requests().at("value")}}));
    PS_CHECK(selected.calls()[0].prepared == prepared);
    PS_CHECK(selected.program().tasks.size() == 1);
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    ExecutionContext context(operations, config);
    auto executable = take(context.prepare(compiled));
    PS_CHECK(counts->linked == 2 && counts->invoked == 0);
    for (double source : {10., 20.}) {
      auto run = take(context.submit(executable, binding(source)));
      auto result = take(run.join());
      PS_CHECK(number(result.results.at("value")) == source + 7);
      PS_CHECK(number(result.results.at("constant")) == 8);
    }
    PS_CHECK(counts->prepared == 1 && counts->linked == 2 &&
             counts->invoked == 4);
    ExecutionContext foreign(registry(std::make_shared<Counts>()));
    PS_CHECK(foreign.prepare(compiled).status().code == ErrorCode::Stale);
    PS_CHECK(foreign.submit(executable, binding(10)).status().code ==
             ErrorCode::Stale);
  }
  PS_CHECK(counts->destroyed == 1 && counts->released == 2 &&
           !counts->invalid_release);
  return 0;
}
int static_snapshot_and_lifetime() {
  static_assert(!std::is_copy_constructible_v<PreparedOperation> &&
                !std::is_move_constructible_v<PreparedOperation>);
  for (auto tag : {UINT64_C(0), UINT64_C(0x8000000000000000),
                   UINT64_C(0x7ff8000000000042)}) {
    auto counts = std::make_shared<Counts>();
    auto operations = registry(counts);
    auto source_schema = std::make_shared<SchemaTemplate>(schema());
    std::vector<OperationMetadata> inputs = {{source_schema}};
    auto params = parameters(false);
    std::memcpy(&std::get<double>(params["tag"]), &tag, sizeof(tag));
    auto prepared =
        take(operations->prepare_operation("test.prepared", inputs, params));
    // Caller-owned metadata and maps cannot change the published preparation.
    source_schema->tensors[0].descriptor.shape[0] = 2;
    params["input"] = true;
    params["tag"] = 99.0;
    inputs.clear();
    TileRequest request;
    request.members[0] = take(TileSet::range(ResourceBudget{}, 0, 1));
    auto lowered =
        take(prepared->lower({request, {}}, 1, physical::Backend::Cpu, {}));
    lowered.program.kernels[0].operation = "test.other";
    auto foreign =
        prepared->prepare_kernel(lowered.program, 0, lowered.outputs);
    PS_CHECK(!foreign.ok() &&
             foreign.status().code == ErrorCode::TypeMismatch &&
             counts->linked == 0);
    lowered.program.kernels[0].operation = "test.prepared";
    auto kernel =
        take(prepared->prepare_kernel(lowered.program, 0, lowered.outputs));
    auto copy = kernel;
    prepared.reset();
    operations.reset();
    PS_CHECK(counts->destroyed == 0 && counts->released == 0);
    double value = 0;
    const auto& tile =
        lowered.program.tiles[lowered.program.tasks[0].writes[0]];
    const auto layout = kernel_layout(lowered.program.layouts[tile.layout]);
    ps_kernel_mutable_view_v8 output{
        &value, sizeof(value), 0, &layout, PS_KERNEL_DOMAIN_HOST_V8, 0, 0, 0,
        0};
    ps_kernel_invocation_v8 invocation{};
    invocation.outputs = &output;
    invocation.output_count = 1;
    kernel.bind(&invocation);
    kernel = {};
    check(kernel_status(copy.invoke(invocation)));
    PS_CHECK(value == 7 && counts->invoked == 1 && counts->destroyed == 0);
    copy = {};
    PS_CHECK(counts->destroyed == 1 && counts->released == 1 &&
             !counts->invalid_release);
  }
  return 0;
}
int concurrent_reuse() {
  for (bool reads_input : {false, true}) {
    auto counts = std::make_shared<Counts>();
    counts->gate = std::make_shared<Gate>();
    {
      auto operations = registry(counts);
      auto source = document(reads_input);
      source.outputs.resize(1);
      GraphContext graph(source);
      auto compiled = plan(operations, graph);
      ExecutionContextConfig config;
      config.cpu_workers = 2;
      ExecutionContext context(operations, config);
      auto prepared = take(context.prepare(compiled));
      auto first = take(context.submit(prepared, binding(1)));
      auto second = take(context.submit(prepared, binding(2)));
      PS_CHECK(number(take(first.join()).results.at("value")) ==
               (reads_input ? 8 : 7));
      PS_CHECK(number(take(second.join()).results.at("value")) ==
               (reads_input ? 9 : 7));
      PS_CHECK(counts->gate->arrived == 2 && counts->prepared == 1 &&
               counts->linked == 1 && counts->invoked == 2);
    }
    PS_CHECK(counts->destroyed == 1 && counts->released == 1 &&
             !counts->invalid_release);
  }
  return 0;
}
int concurrent_mixed_outcomes(bool failure) {
  auto counts = std::make_shared<Counts>();
  counts->gate = std::make_shared<Gate>();
  counts->gate->held = true;
  auto first_binding = binding(1);
  if (failure)
    counts->rejected_object = first_binding.inputs[0].result.object_id();
  {
    auto operations = registry(counts);
    auto source = document(true);
    source.outputs.resize(1);
    GraphContext graph(source);
    auto compiled = plan(operations, graph);
    ExecutionContextConfig config;
    config.cpu_workers = 2;
    ExecutionContext context(operations, config);
    const auto root = take(context.resource_budget());
    auto prepared = take(context.prepare(compiled));
    {
      std::atomic<unsigned> first_exports{0}, second_exports{0};
      ExecutionOptions a, b;
      a.on_call_retired = [&](const CallRecord& record) {
        if (record.outcome == CallOutcome::Succeeded)
          ++first_exports;
      };
      b.on_call_retired = [&](const CallRecord& record) {
        if (record.outcome == CallOutcome::Succeeded)
          ++second_exports;
      };
      auto first = take(context.submit(prepared, first_binding, {}, a));
      auto second = take(context.submit(prepared, binding(2), {}, b));
      const bool both_entered = counts->gate->wait();
      const bool cancelled = failure || first.request_cancel();
      counts->gate->release();
      auto rejected = first.join();
      auto completed = second.join();
      PS_CHECK(both_entered && cancelled);
      PS_CHECK(!rejected.ok() &&
               rejected.status().code == (failure ? ErrorCode::OperationFailed
                                                  : ErrorCode::Cancelled));
      if (failure)
        PS_CHECK(rejected.status().message == "selected Run failed");
      PS_REQUIRE_OK(completed);
      PS_CHECK(number(completed.value().results.at("value")) == 9);
      PS_CHECK(first_exports == 0 && second_exports == 1);
      PS_CHECK(counts->prepared == 1 && counts->linked == 1 &&
               counts->invoked == 2 && !counts->released);
    }
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Queue] ==
             config.maximum_queued_tasks);
  }
  PS_CHECK(counts->destroyed == 1 && counts->released == 1 &&
           !counts->invalid_release);
  return 0;
}
int projected_execution(bool supports_gpu) {
  auto counts = std::make_shared<Counts>();
  auto operations = std::make_shared<OperationRegistry>();
  auto operation = definition(counts);
  operation.traits.supports_gpu = supports_gpu;
  check(operations->register_operation(std::move(operation)));
  auto source_calls = std::make_shared<std::atomic<unsigned>>(0);
  OperationDefinition source;
  source.key = "test.unwanted";
  source.traits.outputs = {output("value")};
  source.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty())
      check(builder.write_output(take(builder.add_task("fail", "fail-v1")), 0,
                                 0, 0));
    return builder.finish();
  };
  source.prepare_kernel = [source_calls](const auto&, const auto&, const auto&,
                                         const auto&,
                                         const ps_native_gpu_service_v2*) {
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    kernel.context = new std::shared_ptr<std::atomic<unsigned>>(source_calls);
    kernel.release = [](const void* context) {
      delete static_cast<const std::shared_ptr<std::atomic<unsigned>>*>(
          context);
    };
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      ++**static_cast<const std::shared_ptr<std::atomic<unsigned>>*>(
          invocation->prepared_context);
      *status =
          kernel_failure(ErrorCode::OperationFailed, "selected source failed");
    };
    return Result<ps_prepared_kernel_v8>(kernel);
  };
  check(operations->register_operation(std::move(source)));
  check(operations->freeze());
  ExecutionContext context(operations);
  for (bool input : {false, true}) {
    WorkflowDocument document;
    document.nodes = {{1, "test.unwanted", {}, {}},
                      {2,
                       "test.prepared",
                       {WorkflowNodeOutput{1, "value"}},
                       parameters(input)}};
    document.outputs = {{"value", 2, "value"}};
    GraphContext graph(document);
    auto compiled = plan(operations, graph);
    auto executable = take(context.prepare(compiled));
    auto run = take(context.submit(executable));
    auto result = run.join();
    if (!input) {
      PS_CHECK(result.ok() && number(result.value().results.at("value")) == 7);
      PS_CHECK(*source_calls == 0 && compiled.program().calls[0].tasks.empty());
    } else {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::OperationFailed);
      PS_CHECK(*source_calls == 1);
    }
  }
  return 0;
}
int projection_validation(bool supports_gpu) {
  auto counts = std::make_shared<Counts>();
  auto operations = std::make_shared<OperationRegistry>();
  auto op = definition(counts);
  op.traits.supports_gpu = supports_gpu;
  // Static preparation may narrow a registered projection, never broaden it.
  op.traits.outputs[0].input_indices = std::vector<std::uint32_t>{};
  check(operations->register_operation(std::move(op)));
  check(operations->freeze());
  auto inputs = metadata();
  PS_CHECK(
      operations->prepare_operation("test.prepared", inputs, parameters(false))
          .ok());
  PS_CHECK(
      operations->prepare_operation("test.prepared", inputs, parameters(true))
          .status()
          .code == ErrorCode::InvalidArgument);
  const auto calls = counts->prepared.load();
  // Empty runtime projections still require the complete static input contract.
  PS_CHECK(
      !operations->prepare_operation("test.prepared", {}, parameters(false))
           .ok());
  inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(schema(ElementType::Int64));
  PS_CHECK(
      operations->prepare_operation("test.prepared", inputs, parameters(false))
          .status()
          .code == ErrorCode::TypeMismatch);
  PS_CHECK(counts->prepared == calls && counts->invoked == 0);
  PS_CHECK(counts->destroyed == counts->prepared);
  for (const auto& ports :
       {std::vector<std::uint32_t>{0, 0}, std::vector<std::uint32_t>{1}}) {
    auto invalid = std::make_shared<OperationRegistry>();
    auto operation = definition(counts);
    operation.traits.supports_gpu = supports_gpu;
    const auto prepare = operation.prepare_static;
    operation.prepare_static = [prepare, ports](const auto& inputs,
                                                const auto& parameters) {
      auto result = prepare(inputs, parameters);
      if (!result.ok())
        return result;
      auto prepared = result.take_value();
      prepared.outputs[0].input_indices = ports;
      return Result<OperationPreparation>(std::move(prepared));
    };
    check(invalid->register_operation(std::move(operation)));
    check(invalid->freeze());
    auto rejected = invalid->prepare_operation("test.prepared", metadata(),
                                               parameters(false));
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(counts->invoked == 0 && counts->destroyed == counts->prepared);
  }
  return 0;
}

int floating_owner_restoration() {
  struct Restore {
    std::fenv_t saved{};
    Restore() { std::fegetenv(&saved); }
    ~Restore() { std::fesetenv(&saved); }
  } restore;
  for (unsigned mode = 0; mode < 3; ++mode) {
    PS_CHECK(std::fesetround(FE_DOWNWARD) == 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    unsigned schemas_destroyed = 0, states_destroyed = 0;
    bool inherited = false;
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition operation;
    operation.key = "test.environment";
    operation.traits.outputs = {output("value")};
    operation.traits.requires_metadata_specialization = true;
    const auto specialize = [&](const auto&, const auto&) {
      inherited = std::fegetround() == FE_DOWNWARD &&
                  std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
      std::fesetround(FE_UPWARD);
      std::feraiseexcept(FE_INVALID);
      OperationOutputSpecialization output;
      output.metadata.result_schema = std::shared_ptr<const SchemaTemplate>(
          new SchemaTemplate(schema()), [&](const SchemaTemplate* schema) {
            delete schema;
            ++schemas_destroyed;
            std::fesetround(FE_TOWARDZERO);
            std::feraiseexcept(FE_OVERFLOW);
          });
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    };
    if (!mode) {
      operation.specialize_metadata = specialize;
    } else {
      operation.prepare_static = [&, mode](const auto& inputs,
                                           const auto& parameters) {
        OperationPreparation prepared;
        prepared.outputs = take(specialize(inputs, parameters));
        if (mode == 2)
          prepared.outputs.push_back(prepared.outputs.front());
        prepared.state =
            std::shared_ptr<const void>(new int(7), [&](const void* state) {
              delete static_cast<const int*>(state);
              ++states_destroyed;
              std::fesetround(FE_TOWARDZERO);
              std::feraiseexcept(FE_OVERFLOW);
            });
        return Result<OperationPreparation>(std::move(prepared));
      };
    }
    operation.lower = [](const OperationLowering& query) {
      OperationProgramBuilder builder(query);
      return builder.finish();
    };
    check(registry->register_operation(std::move(operation)));
    check(registry->freeze());
    {
      auto prepared = registry->prepare_operation("test.environment", {}, {});
      PS_CHECK(prepared.ok() == (mode != 2));
      PS_CHECK(inherited && schemas_destroyed == 1);
      PS_CHECK(std::fegetround() == FE_DOWNWARD &&
               std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    }
    registry.reset();
    PS_CHECK(states_destroyed == (mode ? 1U : 0U));
    PS_CHECK(std::fegetround() == FE_DOWNWARD &&
             std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
  }
  return 0;
}

}  // namespace
int main() {
  PS_CHECK(floating_owner_restoration() == 0);
  PS_CHECK(compiled_reuse() == 0);
  PS_CHECK(static_snapshot_and_lifetime() == 0);
  PS_CHECK(projection_validation(false) == 0);
  PS_CHECK(projection_validation(true) == 0);
  PS_CHECK(concurrent_reuse() == 0);
  PS_CHECK(concurrent_mixed_outcomes(false) == 0);
  PS_CHECK(concurrent_mixed_outcomes(true) == 0);
  PS_CHECK(projected_execution(false) == 0);
  PS_CHECK(projected_execution(true) == 0);
  return 0;
}
