#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "photospider/plugin/whole_stage.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
using Poll = Result<ResultProgramPoll>;
SchemaTemplate tensor_schema(ElementType type, std::uint64_t count,
                             const char* id) {
  SchemaTemplate schema;
  schema.id = id;
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, {count}};
  tensor.layout.channel_axis.reset();
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
void result_port(OperationPortConstraint* port, const SchemaTemplate& schema) {
  port->result_schema_id = schema.id;
  port->result_schema_version = schema.version;
}
Result<ResultRef> source_result(const ResourceBudget& root,
                                const SchemaTemplate& schema,
                                const TensorSpan& backing) {
  auto started = ResultBuilder::start(root, schema, "test.input");
  if (!started.ok())
    return Result<ResultRef>(started.status());
  auto builder = started.take_value();
  Status status;
  status = builder.publish_tensor(0, backing.region(), backing.bytes());
  return status.ok() ? builder.seal() : Result<ResultRef>(status);
}
double scalar_result(const ResultRef& result) {
  const auto facts = result.descriptor();
  if (!facts.ok())
    throw std::runtime_error(facts.status().message);
  double value = 0;
  const auto status = result.read_tensor(facts.value(), 0, {0}, &value, 8);
  if (!status.ok())
    throw std::runtime_error(status.message);
  return value;
}
OperationTraits traits(SchemaTemplate schema, std::uint32_t inputs,
                       std::uint64_t state) {
  OperationTraits t;
  t.input_count = inputs;
  t.input_schema.resize(inputs);
  auto& output = t.outputs[0];
  output.continuation_bytes = state;
  output.maximum_dependency_stages = 100000;

  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  t.workspace_bytes = 4096;
  return t;
}
struct ForeignState {
  Poll poll(const ResultProgramPhase& phase) {
    ResourceBudget other;
    auto builder = take(ResultBuilder::start(
        other, *phase.query.output.result_schema, phase.query.semantic_key));
    const double value = 7;
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
int foreign_objects() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "foreign";
  operation.traits =
      traits(tensor_schema(ElementType::Float64, 1, "foreign.tensor"), 0,
             sizeof(ForeignState));
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ForeignState>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "foreign", {}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan.value();
  ExecutionContext execution(registry);
  PS_CHECK(execution.execute(plan).status().code == ErrorCode::InvalidArgument);
  return 0;
}
struct ForeignBackingState {
  const TensorSpan* backing;
  explicit ForeignBackingState(const TensorSpan* value) : backing(value) {}
  Poll poll(const ResultProgramPhase& phase) {
    auto started =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!started.ok())
      return Poll(started.status());
    auto builder = started.take_value();
    Status status;
    status = builder.publish_tensor(0, backing->region(), backing->layout(),
                                    backing->storage());
    if (!status.ok())
      return Poll(status);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
int foreign_backing() {
  auto scalar = TensorSpan::from_float64(42);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "foreign_backing";
  operation.traits =
      traits(tensor_schema(ElementType::Float64, 1, "test.foreign_backing"), 0,
             sizeof(ForeignBackingState));
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ForeignBackingState>(allocator, &scalar);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "foreign_backing", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan.value();
  for (std::uint64_t limit : {0U, 8U}) {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Referenced] = limit;
    ExecutionContext context(registry, config);
    auto root = context.resource_budget().take_value();
    bool delivered = false;
    ExecutionOptions options;
    options.result_publication = [&](ValueRef, const ResultRef& result) {
      delivered = true;
      return scalar_result(result) == 42 ? Status::success()
                                         : Status{ErrorCode::Internal, {}};
    };
    auto status = context.execute(plan, {}, {}, options);
    PS_CHECK(status.ok() == (limit == 8));
    PS_CHECK(delivered == (limit == 8));
    if (!limit)
      PS_CHECK(status.status().code == ErrorCode::ResourceExhausted);
    status = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
  }
  return 0;
}
int binding_contract() {
  auto backing = TensorSpan::from_float64(42);
  auto schema = tensor_schema(ElementType::Float64, 1, "test.binding");
  const auto definition = [&] {
    OperationDefinition operation;
    operation.key = "binding_output";
    operation.traits = traits(schema, 0, sizeof(ForeignBackingState));
    operation.start_result = [&](const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
      return ResultContinuation::make<ForeignBackingState>(allocator, &backing);
    };
    return operation;
  };
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->register_operation(definition()).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {2, "unused", std::make_shared<const SchemaTemplate>(schema)}};
  document.nodes = {{1, "binding_output", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  Compiler compiler(registry);
  GraphContext graph(document);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  auto old = document;
  old.schema_version = 4;
  GraphContext old_graph(old);
  PS_CHECK(compiler.compile(old_graph).status().code ==
           ErrorCode::InvalidArgument);
  auto missing = document;
  missing.inputs[0].result_schema.reset();
  GraphContext missing_graph(missing);
  PS_CHECK(compiler.compile(missing_graph).status().code ==
           ErrorCode::InvalidArgument);
  ExecutionContext context(registry);
  auto root = context.resource_budget().take_value();
  auto input = source_result(root, schema, backing).take_value();
  PS_CHECK(context.execute(compiled.value().plan.value()).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(context
               .execute(compiled.value().plan.value(),
                        {{{"unused", input}, {"unused", input}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(context.execute(compiled.value().plan.value(), {{{"extra", input}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(context.execute(compiled.value().plan.value(), {{{"unused", {}}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  ResourceBudget foreign;
  auto other_root = source_result(foreign, schema, backing).take_value();
  PS_CHECK(
      context.execute(compiled.value().plan.value(), {{{"unused", other_root}}})
          .status()
          .code == ErrorCode::InvalidArgument);
  auto wrong_schema = schema;
  wrong_schema.id = "test.wrong_binding";
  auto wrong = source_result(root, wrong_schema, backing).take_value();
  PS_CHECK(context.execute(compiled.value().plan.value(), {{{"unused", wrong}}})
               .status()
               .code == ErrorCode::TypeMismatch);
  auto result =
      context.execute(compiled.value().plan.value(), {{{"unused", input}}});
  PS_CHECK(result.ok() &&
           scalar_result(result.value().results.at("value")) == 42);
  auto changed = document;
  changed.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(wrong_schema);
  GraphContext changed_graph(changed);
  auto changed_plan = compiler.compile(changed_graph);
  PS_CHECK(changed_plan.ok() &&
           compiled.value().plan.value().digest().value !=
               changed_plan.value().plan.value().digest().value);
  return 0;
}
struct EffectState {
  unsigned* calls;
  unsigned* mode;
  CancellationSource* cancellation;
  EffectState(unsigned* count, unsigned* failure, CancellationSource* stop)
      : calls(count), mode(failure), cancellation(stop) {}
  Poll poll(const ResultProgramPhase& phase) {
    ++*calls;
    if (*mode == 1)
      return Poll(Status{ErrorCode::OperationFailed, "unnamed effect failure"});
    if (*mode == 2) {
      cancellation->cancel();
      return Poll(Status{ErrorCode::Cancelled, {}});
    }
    auto made =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!made.ok())
      return Poll(made.status());
    auto builder = made.take_value();
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
      const double numbers[] = {42.0 + slot, 43.0 + slot};
      const auto count = slot + 1;
      const auto published_count = *mode == 3 && slot == 1 ? 1U : count;
      auto status = builder.publish_tensor(
          slot, Region({{0, published_count}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(numbers),
                   published_count * 8));
      if (!status.ok())
        return Poll(status);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
struct EffectReaderState {
  Poll poll(const ResultProgramPhase& phase) {
    double first = 0, second = 0;
    auto status = phase.tensors->at({0, 0}).read({0}, &first, 8);
    if (!status.ok())
      return Poll(status);
    status = phase.tensors->at({0, 1}).read({1}, &second, 8);
    if (!status.ok())
      return Poll(status);
    if (first != 42 || second != 44)
      return Poll(Status{ErrorCode::Internal, "effect tensor coverage"});
    auto made =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!made.ok())
      return Poll(made.status());
    auto builder = made.take_value();
    const double value = first + second;
    status = builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8));
    if (!status.ok())
      return Poll(status);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
int mandatory_effects() {
  unsigned calls = 0, mode = 0;
  CancellationSource cancellation;
  auto schema = tensor_schema(ElementType::Float64, 1, "test.effect");
  auto second = schema.tensors[0];
  second.key = "second";
  second.descriptor.shape = {2};
  schema.tensors.push_back(second);
  auto output = tensor_schema(ElementType::Float64, 1, "test.effect_reader");
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition effect;
  effect.key = "effect";
  effect.traits = traits(schema, 0, sizeof(EffectState));
  effect.traits.deterministic = false;
  effect.traits.side_effect_free = false;
  effect.traits.cacheable = false;
  effect.start_result = [&](const ResultProgramQuery&,
                            const BufferAllocator& allocator) {
    return ResultContinuation::make<EffectState>(allocator, &calls, &mode,
                                                 &cancellation);
  };
  PS_CHECK(
      registry
          ->register_operation(sdk::with_whole_stage(std::move(effect), 100000))
          .ok());
  OperationDefinition reader;
  reader.key = "effect_reader";
  reader.traits = traits(output, 1, sizeof(EffectReaderState));
  result_port(&reader.traits.input_schema[0], schema);
  reader.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
    return ResultContinuation::make<EffectReaderState>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(reader)).ok());
  auto scalar = TensorSpan::from_float64(7);
  OperationDefinition constant;
  constant.key = "effect_constant";
  constant.traits = traits(output, 0, sizeof(ForeignBackingState));
  constant.start_result = [&](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
    return ResultContinuation::make<ForeignBackingState>(allocator, &scalar);
  };
  PS_CHECK(registry->register_operation(std::move(constant)).ok());
  PS_CHECK(registry->freeze().ok());
  ExecutionContext context(registry);
  WorkflowDocument document;
  document.nodes = {{1, "effect", {}, {}},
                    {2, "effect_reader", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"value", 2, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan.value();
  auto run_plan = plan;
  ExecutionBindings run_bindings = {};
  for (unsigned run = 1; run <= 2; ++run) {
    auto result = context.execute(run_plan, run_bindings);
    PS_CHECK(result.ok() && calls == run &&
             scalar_result(result.value().results.at("value")) == 86);
  }
  mode = 3;
  const auto missing_slot_calls = calls;
  bool sealed_sparse = false;
  ExecutionOptions sparse_options;
  sparse_options.result_publication = [&](ValueRef ref,
                                          const ResultRef& object) {
    if (ref.node_id == 1) {
      auto descriptor = object.descriptor();
      if (descriptor.ok()) {
        const auto& coverage = descriptor.value().tensor_coverage(1);
        sealed_sparse = coverage.contains({0}) && !coverage.contains({1});
      }
    }
    return Status::success();
  };
  auto sparse = context.execute(run_plan, run_bindings, {}, sparse_options);
  PS_CHECK(sparse.status().code == ErrorCode::InvalidArgument &&
           sparse.status().detail.origin == FailureOrigin::Protocol &&
           !sealed_sparse && calls == missing_slot_calls + 1);
  mode = 0;
  document.nodes[1] = {2, "effect_constant", {}, {}};
  GraphContext unrelated(document);
  auto independent =
      Compiler(registry).compile(unrelated).take_value().plan.value();
  auto captured_plan = independent;
  ExecutionBindings captured_bindings = {};
  DemandQuery empty{{"value", TileRequest{}}};
  const auto before = calls;
  // Empty result selections still execute the Plan's mandatory effect roots.
  PS_CHECK(
      context.execute_fragments(captured_plan, captured_bindings, empty).ok() &&
      calls == before + 1);
  PS_CHECK(
      context.execute_fragments(captured_plan, captured_bindings, {}).ok() &&
      calls == before + 2);
  mode = 1;
  auto failure = context.execute(captured_plan, captured_bindings);
  PS_CHECK(failure.status().code == ErrorCode::OperationFailed &&
           failure.status().message == "unnamed effect failure" &&
           calls == before + 3);
  mode = 2;
  auto cancelled =
      context.execute(captured_plan, captured_bindings, cancellation.token());
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled &&
           calls == before + 4);
  return 0;
}
int foreign_binding_admission() {
  auto scalar_schema =
      tensor_schema(ElementType::Float32, 1, "test.foreign_scalar");
  auto output_schema =
      tensor_schema(ElementType::Float64, 1, "test.scalar_guard");
  auto backing = TensorSpan::from_float64(7);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "scalar_guard";
  operation.traits = traits(output_schema, 1, sizeof(ForeignBackingState));
  auto& port = operation.traits.input_schema[0];
  result_port(&port, scalar_schema);
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  port.rank = 1;
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ForeignBackingState>(allocator, &backing);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "scalar", std::make_shared<const SchemaTemplate>(scalar_schema)}};
  document.nodes = {{1, "scalar_guard", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan.value();
  ExecutionContext context(registry);
  ResourceLimits limits;
  limits.maximum_work = 100000;
  ResourceBudget foreign(limits);
  const float number = .5F;
  auto numeric = TensorWriter::allocate({ElementType::Float32, {1}},
                                        Region::whole({1}), BufferAllocator{});
  PS_CHECK(numeric.ok());
  auto storage = numeric.take_value();
  std::memcpy(storage.data(), &number, sizeof(number));
  auto published = std::move(storage).publish().take_value();
  auto input = source_result(foreign, scalar_schema, published).take_value();
  const auto used = foreign.statistics().issued.work;
  PS_CHECK(foreign.consume({limits.maximum_work - used}).ok());
  const auto before = foreign.statistics().issued.work;
  PS_CHECK(context.execute(plan, {{{"scalar", input}}}).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(foreign.statistics().issued.work == before);
  return 0;
}
struct SparseNeedPeaks {
  std::uint64_t ready = 0;
};
struct ManyPartsState {
  const Footprint* requested;
  const TensorSpan* backing;
  std::atomic<unsigned>* polls;
  std::atomic<unsigned>* reads;
  SparseNeedPeaks* peaks;
  ManyPartsState(const Footprint* need, const TensorSpan* value,
                 std::atomic<unsigned>* poll_count,
                 std::atomic<unsigned>* read_count, SparseNeedPeaks* observed)
      : requested(need),
        backing(value),
        polls(poll_count),
        reads(read_count),
        peaks(observed) {}
  Poll poll(const ResultProgramPhase& phase) {
    ++*polls;
    peaks->ready = phase.resources.statistics().peak[ResourceKind::Metadata];
    for (const auto& region : requested->boxes()) {
      std::uint8_t value = 1;
      auto status = phase.read_tensor(0, 0, {0, region.dimensions()[1].offset},
                                      &value, 1);
      if (!status.ok())
        return Poll(status);
      ++*reads;
    }
    return ForeignBackingState(backing).poll(phase);
  }
};
int sparse_source_need_budget() {
  std::vector<Region> boxes;
  for (std::uint64_t i = 0; i < 128; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{0, 1}, {2 * i, 1}});
  auto requested = Footprint::from_regions({1, 256}, boxes).take_value();
  auto scalar = TensorSpan::from_float64(42);
  auto input_schema =
      tensor_schema(ElementType::UInt8, 256, "test.source_bytes");
  input_schema.tensors[0].descriptor.shape = {1, 256};
  input_schema.tensors[0].layout.spatial = true;
  std::atomic<unsigned> polls{0}, reads{0};
  SparseNeedPeaks peaks;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "many_parts";
  operation.traits =
      traits(tensor_schema(ElementType::Float64, 1, "test.sparse_output"), 1,
             sizeof(ManyPartsState));
  result_port(&operation.traits.input_schema[0], input_schema);
  operation.map_dependencies =
      [&requested](
          const ResultStaticQuery& query) -> Result<ResultStaticMapping> {
    auto need = sdk::tile_need_for_samples(query, 0, 0, requested);
    if (!need.ok())
      return Result<ResultStaticMapping>(need.status());
    ResultStaticMapping mapping;
    mapping.inputs.push_back(need.take_value());
    return Result<ResultStaticMapping>(std::move(mapping));
  };
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ManyPartsState>(
        allocator, &requested, &scalar, &polls, &reads, &peaks);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema =
      std::make_shared<const SchemaTemplate>(input_schema);
  document.inputs = {declaration};
  document.nodes = {{1, "many_parts", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan =
      Compiler(registry)
          .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
          .take_value()
          .plan.value();
  auto input = TensorSpan::create({ElementType::UInt8, {1, 256}},
                                  Region::whole({1, 256}), {0, {256, 1}},
                                  std::vector<std::uint8_t>(256))
                   .take_value();
  // Measure setup and static-grant admission on this platform first. Standard
  // library object sizes affect managed metadata; no fixed KiB threshold can
  // reliably separate these phases across supported toolchains.
  std::uint64_t need_limit = 0;
  for (bool constrained : {false, true}) {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.gpu_enabled = false;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = 1048576;
    config.managed_resources->capacity[ResourceKind::Metadata] =
        constrained ? need_limit : 1048576;
    ExecutionContext context(registry, config);
    auto root = context.resource_budget().take_value();
    auto source = source_result(root, input_schema, input);
    PS_CHECK(source.ok());
    auto run_plan = plan;
    ExecutionBindings run_bindings = {{{"source", source.value()}}};
    const auto setup_peak = root.statistics().peak[ResourceKind::Metadata];
    polls.store(0);
    reads.store(0);
    ExecutionOptions options;
    options.maximum_dependency_work = 100000000;
    options.dependencies.sets.maximum_work = 100000000;
    auto result = context.execute(run_plan, run_bindings, {}, options);
    if (!constrained && !result.ok())
      std::cerr << "sparse static grant fixture: "
                << static_cast<int>(result.status().code) << " "
                << result.status().message << "\n";
    if (constrained) {
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(polls.load() == 0 && reads.load() == 0);
    } else {
      PS_CHECK(result.ok() &&
               scalar_result(result.value().results.at("value")) == 42);
      PS_CHECK(polls.load() == 1 && reads.load() == 128);
      PS_CHECK(peaks.ready > setup_peak);
      need_limit = setup_peak + (peaks.ready - setup_peak) / 2;
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(foreign_objects() == 0);
  PS_CHECK(foreign_backing() == 0);
  PS_CHECK(binding_contract() == 0);
  PS_CHECK(mandatory_effects() == 0);
  PS_CHECK(foreign_binding_admission() == 0);
  PS_CHECK(sparse_source_need_budget() == 0);
  return 0;
}
