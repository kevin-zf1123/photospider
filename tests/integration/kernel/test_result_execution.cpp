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
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
using Poll = Result<ResultProgramPoll>;
SchemaTemplate ids_schema() {
  SchemaTemplate schema;
  schema.id = "test.selected_ids";
  schema.publication = PublishPolicy::StablePrefix;
  schema.fields = {
      {"ids", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}}};
  return schema;
}
SchemaTemplate sum_schema() {
  SchemaTemplate schema;
  schema.id = "test.id_sum";
  schema.fields = {{"sum", ElementType::Int64, {}, {}}};
  return schema;
}
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
  port->kind = OperationPortKind::Result;
  port->result_schema_id = schema.id;
  port->result_schema_version = schema.version;
}
Result<ResultRef> source_result(const ResourceBudget& root,
                                const SchemaTemplate& schema,
                                const Value& backing) {
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
  output.region_rule = OperationRegionRule::Tiled;
  output.continuation_bytes = state;
  output.maximum_dependency_stages = 100000;
  output.output_schema.kind = OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  t.workspace_bytes = 4096;
  return t;
}
struct SelectState {
  std::uint64_t position = 0, count = 0, emitted = 0;
  unsigned stage = 0, failure = 0;
  bool supplied = false;
  ResultBuilder builder;
  std::atomic<unsigned>* reads;
  ResultRef* captured;
  SelectState(std::uint64_t count, unsigned failure,
              std::atomic<unsigned>* reads, ResultRef* captured)
      : count(count), failure(failure), reads(reads), captured(captured) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (!stage++) {
      builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {count, count * 8}));
      check(builder.publish(0, 0));
      if (failure == 3)
        *captured = builder.reference();
      return Poll(ResultPublication{builder.reference(), false});
    }
    if (failure == 3) {
      Status status{ErrorCode::OperationFailed, "producer stopped after prefix",
                    FailureReason::NotConverged};
      builder.fail(status);
      return Poll(status);
    }
    if (position < count && !supplied) {
      supplied = true;
      return Poll(ResultProgramNeed{{{0, true, false, {}}}});
    }
    if (position < count) {
      const auto batch =
          std::min(count - position,
                   std::max<std::uint64_t>(1, phase.query.page_bytes / 16));
      auto bytes = take(phase.allocator.allocate(batch * 8));
      std::uint64_t selected = 0;
      for (std::uint64_t i = 0; i < batch; ++i) {
        check(phase.consume_work(1));
        std::uint8_t value = 0;
        check(phase.read_tensor(0, 0, {position + i}, &value, 1));
        ++*reads;
        if (value % 3 == 1) {
          const auto id = static_cast<std::int64_t>(position + i);
          std::memcpy(bytes.data() + selected++ * 8, &id, 8);
        }
      }
      position += batch;
      supplied = false;
      if (selected)
        check(builder.append(0, selected, {bytes.data(), selected * 8}));
      emitted += selected;
      check(builder.publish(0, emitted));
      return Poll(ResultPublication{builder.reference(), false});
    }
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
struct SumState {
  bool requested = false;
  Poll poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      return Poll(ResultProgramNeed{{{0, true, false, {}}}});
    }
    const auto& source = phase.results.at(0);
    auto descriptor = take(source.descriptor());
    std::int64_t sum = 0;
    for (std::uint64_t position = 0; position < descriptor.rows(0);) {
      const auto count =
          std::min(descriptor.rows(0) - position, phase.query.page_bytes / 8);
      auto page = take(take(source.prepare_read(descriptor, 0, position, count))
                           .load(phase.query.page_bytes));
      for (std::size_t offset = 0; offset < page->bytes().size(); offset += 8) {
        std::int64_t id = 0;
        std::memcpy(&id, page->bytes().data() + offset, 8);
        sum += id;
      }
      position += count;
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, {source.object_id()}));
    check(
        builder.append(0, 1, {reinterpret_cast<const std::uint8_t*>(&sum), 8}));
    check(builder.publish(0, 1));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
// Whole input authorization, bounded field paging, and staged publication.
struct ForwardState {
  bool requested = false;
  std::uint64_t position = 0;
  ResultBuilder builder;
  Poll poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      return Poll(ResultProgramNeed{{{0, true, false, {}}}});
    }
    const auto& source = phase.results.at(0);
    auto facts = take(source.descriptor());
    if (!builder.reference().valid())
      builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {37, 37 * 8}, {source.object_id()}));
    if (position == facts.rows(0))
      return Poll(ResultPublication{take(builder.seal()), true});
    const auto batch =
        std::min(facts.rows(0) - position, phase.query.page_bytes / 8);
    auto page = take(take(source.prepare_read(facts, 0, position, batch))
                         .load(phase.query.page_bytes));
    check(builder.append(0, batch, page->bytes()));
    position += batch;
    requested = false;
    check(builder.publish(0, position));
    return Poll(ResultPublication{builder.reference(), false});
  }
};
struct FirstState {
  bool requested = false;
  Poll poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      return Poll(ResultProgramNeed{{{0, true, false, {}}}});
    }
    const auto& source = phase.results.at(0);
    auto facts = take(source.descriptor());
    if (!facts.rows(0))
      return Poll(Status{ErrorCode::TypeMismatch, "nonempty test fixture"});
    auto page = take(take(source.prepare_read(facts, 0, 0, 1)).load(8));
    std::int64_t id = 0;
    std::memcpy(&id, page->bytes().data(), 8);
    const double value = static_cast<double>(id);
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), 8}));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
int prefix_retirement(const std::shared_ptr<OperationRegistry>& registry,
                      const ExecutionContextConfig& config,
                      WorkflowDocument document, const Value& input,
                      std::atomic<unsigned>& forward_starts) {
  document.nodes.push_back(
      {4, "forward_ids", {WorkflowNodeOutput{1, "value"}}, {}});
  document.nodes.push_back(
      {5, "first_id", {WorkflowNodeOutput{4, "value"}}, {}});
  document.outputs = {{"a_first", 5, "value"}, {"z_all", 4, "value"}};
  for (unsigned failure = 0; failure < 4; ++failure) {
    if (failure == 3)
      document.nodes.back().inputs = {WorkflowNodeOutput{1, "value"}};
    GraphContext graph(document);
    auto plan =
        Compiler(registry)
            .compile(graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1})
            .take_value()
            .plan;
    auto concurrent_config = config;
    // Two independent runs retain their own field and dependency metadata.
    concurrent_config.managed_resources->capacity[ResourceKind::Host] *= 2;
    concurrent_config.managed_resources->capacity[ResourceKind::Metadata] *= 2;
    ExecutionContext execution(registry, concurrent_config);
    auto root = execution.resource_budget().take_value();
    auto source = source_result(root, *document.inputs[0].result_schema, input)
                      .take_value();
    auto frozen = execution.freeze(plan, {{{"source", source}}}).take_value();
    ExecutionOptions options;
    options.maximum_result_window_bytes = 64;
    options.dependencies.maximum_stages = 100000;
    options.result_publication = [](ValueRef, const ResultRef&) {
      return Status::success();
    };
    auto producer_options = options;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    auto pause = [&] {
      std::unique_lock<std::mutex> lock(mutex);
      if (!entered) {
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
      }
    };
    producer_options.result_publication = [&](ValueRef ref, const ResultRef&) {
      if ((failure == 2 && ref.node_id == 1) ||
          (failure != 2 && ref.node_id == 5)) {
        pause();
        if (failure == 1)
          throw std::runtime_error("Result observer fixture");
        if (failure == 2)
          return Status{ErrorCode::OperationFailed, "observer fixture"};
      }
      return Status::success();
    };
    CancellationSource cancelled;
    auto a = std::async(std::launch::async, [&] {
      return execution.execute(frozen, cancelled.token(), producer_options);
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      PS_CHECK(changed.wait_for(lock, std::chrono::seconds(5),
                                [&] { return entered; }));
    }
    const auto started_forward = forward_starts.load();
    CancellationSource peer_cancel;
    auto b = std::async(std::launch::async, [&] {
      return execution.execute(frozen, peer_cancel.token(), options);
    });
    struct ReleaseOnExit {
      std::mutex& mutex;
      std::condition_variable& changed;
      bool& release;
      CancellationSource& producer;
      CancellationSource& peer;
      ~ReleaseOnExit() {
        producer.cancel();
        peer.cancel();
        {
          std::lock_guard<std::mutex> lock(mutex);
          release = true;
        }
        changed.notify_all();
      }
    } release_on_exit{mutex, changed, release, cancelled, peer_cancel};
    PS_CHECK(b.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    PS_CHECK(forward_starts > started_forward);
    if (failure == 0)
      cancelled.cancel();
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    changed.notify_all();
    auto a_ready = a.wait_for(std::chrono::seconds(5));
    if (a_ready != std::future_status::ready) {
      cancelled.cancel();
      peer_cancel.cancel();
    }
    auto failed = a.get();
    const auto ready = b.wait_for(std::chrono::seconds(5));
    if (ready != std::future_status::ready)
      peer_cancel.cancel();
    auto completed = b.get();
    if (!completed.ok())
      std::cerr << "prefix peer failure " << failure << ' '
                << static_cast<int>(completed.status().code) << ' '
                << completed.status().message << '\n';
    PS_CHECK(a_ready == std::future_status::ready);
    PS_CHECK(failed.status().code == (failure == 0 ? ErrorCode::Cancelled
                                      : failure == 3
                                          ? ErrorCode::Ok
                                          : ErrorCode::OperationFailed));
    PS_CHECK(ready == std::future_status::ready && completed.ok());
    PS_CHECK(scalar_result(completed.value().results.at("a_first")) == 1);
    PS_CHECK(completed.value().results.at("z_all").descriptor().value().rows(
                 0) == 12);
  }
  return 0;
}
struct ForeignState {
  Poll poll(const ResultProgramPhase& phase) {
    ResourceBudget other;
    auto builder = take(ResultBuilder::start(
        other, *phase.query.output.result_schema, phase.query.semantic_key));
    check(builder.publish(0, 0));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
int foreign_objects() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "foreign";
  operation.traits = traits(ids_schema(), 0, sizeof(ForeignState));
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ForeignState>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "foreign", {}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  ExecutionContext execution(registry);
  PS_CHECK(execution.execute(plan).status().code == ErrorCode::InvalidArgument);
  return 0;
}
struct ForeignBackingState {
  const Value* backing;
  explicit ForeignBackingState(const Value* value) : backing(value) {}
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
  auto scalar = Value::from_float64(42);
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
          .plan;
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
  auto backing = Value::from_float64(42);
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
  PS_CHECK(context.execute(compiled.value().plan).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(context
               .execute(compiled.value().plan,
                        {{{"unused", input}, {"unused", input}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(context.execute(compiled.value().plan, {{{"extra", input}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  PS_CHECK(context.execute(compiled.value().plan, {{{"unused", {}}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  ResourceBudget foreign;
  auto other_root = source_result(foreign, schema, backing).take_value();
  PS_CHECK(context.execute(compiled.value().plan, {{{"unused", other_root}}})
               .status()
               .code == ErrorCode::InvalidArgument);
  auto wrong_schema = schema;
  wrong_schema.id = "test.wrong_binding";
  auto wrong = source_result(root, wrong_schema, backing).take_value();
  PS_CHECK(context.execute(compiled.value().plan, {{{"unused", wrong}}})
               .status()
               .code == ErrorCode::TypeMismatch);
  auto result = context.execute(compiled.value().plan, {{{"unused", input}}});
  PS_CHECK(result.ok() &&
           scalar_result(result.value().results.at("value")) == 42);
  auto changed = document;
  changed.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(wrong_schema);
  GraphContext changed_graph(changed);
  auto changed_plan = compiler.compile(changed_graph);
  PS_CHECK(changed_plan.ok() && compiled.value().plan.digest().value !=
                                    changed_plan.value().plan.digest().value);
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
  bool waiting = false;
  Poll poll(const ResultProgramPhase& phase) {
    if (!waiting) {
      waiting = true;
      return Poll(ResultProgramNeed{{{0, true, false, {}}}});
    }
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
  effect.traits.outputs[0].region_rule = OperationRegionRule::Whole;
  effect.traits.deterministic = false;
  effect.traits.side_effect_free = false;
  effect.traits.cacheable = false;
  effect.start_result = [&](const ResultProgramQuery&,
                            const BufferAllocator& allocator) {
    return ResultContinuation::make<EffectState>(allocator, &calls, &mode,
                                                 &cancellation);
  };
  PS_CHECK(registry->register_operation(std::move(effect)).ok());
  OperationDefinition reader;
  reader.key = "effect_reader";
  reader.traits = traits(output, 1, sizeof(EffectReaderState));
  result_port(&reader.traits.input_schema[0], schema);
  reader.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
    return ResultContinuation::make<EffectReaderState>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(reader)).ok());
  auto scalar = Value::from_float64(7);
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
          .plan;
  auto frozen = context.freeze(plan, {}).take_value();
  for (unsigned run = 1; run <= 2; ++run) {
    auto result = context.execute(frozen);
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
  auto sparse = context.execute(frozen, {}, sparse_options);
  PS_CHECK(sparse.status().code == ErrorCode::InvalidArgument &&
           sparse.status().detail.origin == FailureOrigin::Protocol &&
           sparse.status().message ==
               "Whole publication omitted a tensor member" &&
           !sealed_sparse && calls == missing_slot_calls + 1);
  mode = 0;
  document.nodes[1] = {2, "effect_constant", {}, {}};
  GraphContext unrelated(document);
  auto independent = Compiler(registry).compile(unrelated).take_value().plan;
  auto captured = context.freeze(independent, {}).take_value();
  DemandQuery empty{{"value", TileRequest{}}};
  const auto before = calls;
  PS_CHECK(context.execute_fragments(captured, empty).ok() && calls == before);
  PS_CHECK(context.execute_fragments(captured, {}).ok() && calls == before);
  mode = 1;
  auto failure = context.execute(captured);
  PS_CHECK(failure.status().code == ErrorCode::OperationFailed &&
           failure.status().message == "unnamed effect failure" &&
           calls == before + 1);
  mode = 2;
  auto cancelled = context.execute(captured, cancellation.token());
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled &&
           calls == before + 2);
  return 0;
}
int foreign_scalar_validation() {
  auto scalar_schema =
      tensor_schema(ElementType::Float32, 1, "test.foreign_scalar");
  auto output_schema =
      tensor_schema(ElementType::Float64, 1, "test.scalar_guard");
  auto backing = Value::from_float64(7);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "scalar_guard";
  operation.traits = traits(output_schema, 1, sizeof(ForeignBackingState));
  auto& port = operation.traits.input_schema[0];
  result_port(&port, scalar_schema);
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  port.rank = 1;
  port.scalar_bounds = true;
  port.minimum = 0;
  port.maximum = 1;
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
          .plan;
  ExecutionContext context(registry);
  ResourceLimits limits;
  limits.maximum_work = 100000;
  ResourceBudget foreign(limits);
  const float number = .5F;
  auto numeric = MutableValue::allocate({ElementType::Float32, {1}},
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
  PS_CHECK(context.freeze(plan, {{{"scalar", input}}}).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(foreign.statistics().issued.work == before);
  return 0;
}
struct SparseNeedPeaks {
  std::uint64_t before = 0, supplied = 0;
};
struct ManyPartsState {
  const Footprint* requested;
  const Value* backing;
  bool waiting = false;
  std::atomic<unsigned>* needs;
  std::atomic<unsigned>* reads;
  SparseNeedPeaks* peaks;
  ManyPartsState(const Footprint* need, const Value* value,
                 std::atomic<unsigned>* need_count,
                 std::atomic<unsigned>* read_count, SparseNeedPeaks* observed)
      : requested(need),
        backing(value),
        needs(need_count),
        reads(read_count),
        peaks(observed) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (!waiting) {
      waiting = true;
      ++*needs;
      auto need = Poll(ResultProgramNeed{{take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, *requested))}});
      peaks->before = phase.resources.statistics().peak[ResourceKind::Metadata];
      return need;
    }
    peaks->supplied = phase.resources.statistics().peak[ResourceKind::Metadata];
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
  auto scalar = Value::from_float64(42);
  auto input_schema =
      tensor_schema(ElementType::UInt8, 256, "test.source_bytes");
  input_schema.tensors[0].descriptor.shape = {1, 256};
  input_schema.tensors[0].layout.spatial = true;
  std::atomic<unsigned> needs{0}, reads{0};
  SparseNeedPeaks peaks;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "many_parts";
  operation.traits =
      traits(tensor_schema(ElementType::Float64, 1, "test.sparse_output"), 1,
             sizeof(ManyPartsState));
  result_port(&operation.traits.input_schema[0], input_schema);
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ManyPartsState>(
        allocator, &requested, &scalar, &needs, &reads, &peaks);
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
          .plan;
  auto input =
      Value::create({ElementType::UInt8, {1, 256}}, Region::whole({1, 256}),
                    {0, {256, 1}}, std::vector<std::uint8_t>(256))
          .take_value();
  // Measure setup and sparse-Need admission on this platform first. Standard
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
    needs.store(0);
    reads.store(0);
    ExecutionOptions options;
    options.maximum_dependency_work = 100000000;
    options.dependencies.sets.maximum_work = 100000000;
    auto result =
        context.execute(plan, {{{"source", source.value()}}}, {}, options);
    if ((!constrained && !result.ok()) || needs.load() != 1)
      std::cerr << "sparse source Need fixture: "
                << static_cast<int>(result.status().code) << " "
                << result.status().message << "\n";
    PS_CHECK(needs.load() == 1);
    if (constrained) {
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(reads.load() == 0);
    } else {
      PS_CHECK(result.ok() &&
               scalar_result(result.value().results.at("value")) == 42);
      PS_CHECK(reads.load() == 128);
      PS_CHECK(peaks.supplied > peaks.before);
      need_limit = peaks.before + (peaks.supplied - peaks.before) / 2;
    }
  }
  return 0;
}
int workflow() {
  for (std::uint64_t count : {0U, 37U, 8192U}) {
    for (std::uint64_t page_bytes : {64U, 256U}) {
      const auto source_count = std::max<std::uint64_t>(1, count);
      const auto input_schema =
          tensor_schema(ElementType::UInt8, source_count, "test.source_bytes");
      auto registry = std::make_shared<OperationRegistry>();
      std::atomic<unsigned> builds{0}, reads{0}, forward_starts{0};
      std::function<Status(const ResultProgramQuery&)> start_control,
          select_control;
      unsigned failure_mode = 0;
      ResultRef failed_prefix;
      OperationDefinition select;
      select.key = "select_ids";
      select.traits = traits(ids_schema(), 1, sizeof(SelectState));
      result_port(&select.traits.input_schema[0], input_schema);
      select.traits.parameter_schema = {
          {"count", OperationParameterType::Int64, true, true, 0, 8192}};
      select.start_result = [&](const ResultProgramQuery& query,
                                const BufferAllocator& allocator) {
        if (select_control) {
          auto status = select_control(query);
          if (!status.ok())
            return Result<ResultContinuation>(status);
        }
        ++builds;
        return ResultContinuation::make<SelectState>(
            allocator,
            static_cast<std::uint64_t>(
                std::get<std::int64_t>(query.parameters.at("count"))),
            failure_mode, &reads, &failed_prefix);
      };
      PS_CHECK(registry->register_operation(std::move(select)).ok());
      OperationDefinition sum;
      sum.key = "sum_ids";
      sum.traits = traits(sum_schema(), 1, sizeof(SumState));
      sum.traits.input_schema[0].kind = OperationPortKind::Result;
      sum.traits.input_schema[0].result_schema_id = ids_schema().id;
      sum.traits.input_schema[0].result_schema_version = 1;
      sum.start_result = [&](const ResultProgramQuery& query,
                             const BufferAllocator& allocator) {
        if (start_control) {
          auto status = start_control(query);
          if (!status.ok())
            return Result<ResultContinuation>(status);
        }
        return ResultContinuation::make<SumState>(allocator);
      };
      PS_CHECK(registry->register_operation(std::move(sum)).ok());
      OperationDefinition forward;
      forward.key = "forward_ids";
      forward.traits = traits(ids_schema(), 1, sizeof(ForwardState));
      forward.traits.input_schema[0].kind = OperationPortKind::Result;
      forward.traits.input_schema[0].result_schema_id = ids_schema().id;
      forward.traits.input_schema[0].result_schema_version = 1;
      forward.start_result = [&](const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
        ++forward_starts;
        return ResultContinuation::make<ForwardState>(allocator);
      };
      PS_CHECK(registry->register_operation(std::move(forward)).ok());
      OperationDefinition first;
      first.key = "first_id";
      first.traits =
          traits(tensor_schema(ElementType::Float64, 1, "test.first_id"), 1,
                 sizeof(FirstState));
      first.traits.input_schema[0].kind = OperationPortKind::Result;
      first.traits.input_schema[0].result_schema_id = ids_schema().id;
      first.traits.input_schema[0].result_schema_version = 1;
      first.start_result = [](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
        return ResultContinuation::make<FirstState>(allocator);
      };
      PS_CHECK(registry->register_operation(std::move(first)).ok());
      PS_CHECK(registry->freeze().ok());
      WorkflowDocument document;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "source";
      declaration.result_schema =
          std::make_shared<const SchemaTemplate>(input_schema);
      document.inputs = {declaration};
      document.nodes = {{1,
                         "select_ids",
                         {WorkflowInputReference{1}},
                         {{"count", static_cast<std::int64_t>(count)}}},
                        {2, "sum_ids", {WorkflowNodeOutput{1, "value"}}, {}},
                        {3, "sum_ids", {WorkflowNodeOutput{1, "value"}}, {}}};
      document.outputs = {{"a", 2, "value"},
                          {"b", 3, "value"},
                          {"ids", 1, "value"}};
      GraphContext graph(document);
      auto compiled = Compiler(registry).compile(graph);
      PS_CHECK(compiled.ok());
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ResourceLimits{};
      // The large field retains its bytes and chunk directory in memory.
      const std::uint64_t field_capacity = count == 8192 ? 1048576 : 262144;
      config.managed_resources->capacity[ResourceKind::Host] = field_capacity;
      config.managed_resources->capacity[ResourceKind::Metadata] =
          field_capacity;
      auto context = std::make_unique<ExecutionContext>(registry, config);
      auto root = context->resource_budget().take_value();
      std::vector<std::uint8_t> data(source_count);
      for (std::uint64_t i = 0; i < source_count; ++i)
        data[i] = i % 3;
      auto input = Value::create({ElementType::UInt8, {source_count}},
                                 Region::whole({source_count}), {0, {1}},
                                 std::move(data))
                       .take_value();
      auto source = source_result(root, input_schema, input).take_value();
      ExecutionOptions options;
      options.maximum_result_window_bytes = page_bytes;
      options.dependencies.maximum_stages = 100000;
      std::uint64_t last_count = 0;
      std::set<std::uint64_t> notified_nodes;
      options.result_publication = [&](ValueRef ref, const ResultRef& result) {
        notified_nodes.insert(ref.node_id);
        if (ref.node_id == 1) {
          auto facts = result.descriptor(false);
          if (!facts.ok() || facts.value().rows(0) < last_count)
            return Status{ErrorCode::Internal, {}};
          last_count = facts.value().rows(0);
        }
        return Status::success();
      };
      auto executed = context->execute(compiled.value().plan,
                                       {{{"source", source}}}, {}, options);
      if (!executed.ok())
        std::cerr << "structured failure "
                  << static_cast<int>(executed.status().code) << ' '
                  << executed.status().message << " count=" << count
                  << " page=" << page_bytes << " reason="
                  << static_cast<unsigned>(executed.status().reason)
                  << " live=" << root.statistics().live[ResourceKind::Host]
                  << " peak=" << root.statistics().peak[ResourceKind::Host]
                  << '\n';
      PS_CHECK(executed.ok());
      PS_CHECK(notified_nodes == std::set<std::uint64_t>({1, 2, 3}));
      PS_CHECK(builds == 1 && reads == count);
      auto result = executed.take_value();
      PS_CHECK(result.results.at("a").object_id() !=
               result.results.at("b").object_id());
      std::int64_t expected = 0;
      std::uint64_t expected_count = 0;
      for (std::uint64_t i = 0; i < count; ++i)
        if (i % 3 == 1) {
          expected += static_cast<std::int64_t>(i);
          ++expected_count;
        }
      const auto& ids = result.results.at("ids");
      PS_CHECK(ids.descriptor().value().rows(0) == expected_count);
      PS_CHECK(count == 0 ? ids.association().empty()
                          : ids.association().size() == 1 &&
                                ids.association()[0] == source.object_id());
      bool selection_timing = false;
      std::uint64_t total_elements = 0;
      for (const auto& timing : result.diagnostics.operation_timings) {
        PS_CHECK(!timing.computed_elements_saturated);
        if (timing.output.node_id == 1) {
          selection_timing = true;
          PS_CHECK(timing.computed_elements == expected_count);
        } else if (timing.output.node_id == 2 || timing.output.node_id == 3) {
          total_elements += timing.computed_elements;
        }
      }
      PS_CHECK(selection_timing && total_elements == 2);
      const auto& total = result.results.at("a");
      PS_CHECK(total.association().size() == 1 &&
               total.association()[0] == ids.object_id());
      auto page = total.prepare_read(total.descriptor().value(), 0, 0, 1)
                      .value()
                      .load(8)
                      .take_value();
      std::int64_t actual = 0;
      std::memcpy(&actual, page->bytes().data(), 8);
      PS_CHECK(actual == expected &&
               root.statistics().peak[ResourceKind::Host] <=
                   config.managed_resources->capacity[ResourceKind::Host]);
      auto retained_diagnostics = std::move(result.diagnostics);
      result = {};
      page.reset();
      PS_CHECK(retained_diagnostics.operation_timings.capacity() > 0);
      if (count == 0 && page_bytes == 64)
        context.reset();
      const auto diagnostic_before = root.statistics().live[ResourceKind::Host];
      auto copied_diagnostics = retained_diagnostics;
      PS_CHECK(root.statistics().live[ResourceKind::Host] > diagnostic_before);
      retained_diagnostics = {};
      copied_diagnostics = {};
      if (count == 37 && page_bytes == 64) {
        auto subscriptions = document;
        subscriptions.nodes.push_back(
            {4,
             "select_ids",
             {WorkflowInputReference{1}},
             {{"count", static_cast<std::int64_t>(count)}}});
        subscriptions.nodes.push_back(
            {5, "first_id", {WorkflowNodeOutput{1, "value"}}, {}});
        subscriptions.nodes.push_back(
            {6, "first_id", {WorkflowNodeOutput{4, "value"}}, {}});
        // Each selected step publishes its complete Whole result once,
        // including equivalent producers with distinct computation units.
        subscriptions.outputs = {{"a_alias_ids", 4, "value"},
                                 {"b_first", 5, "value"},
                                 {"c_first", 6, "value"},
                                 {"d_all", 2, "value"}};
        GraphContext subscribed_graph(subscriptions);
        auto subscribed = Compiler(registry).compile(subscribed_graph);
        PS_CHECK(subscribed.ok());
        bool alias_prefix = false, alias_complete = false;
        std::uint64_t alias_revision = 0;
        auto subscribed_options = options;
        subscribed_options.result_publication = [&](ValueRef ref,
                                                    const ResultRef& object) {
          if (ref.node_id == 4) {
            const auto descriptor = object.descriptor(false).value();
            if (descriptor.revision() <= alias_revision)
              return Status{ErrorCode::Internal, "duplicate alias notice"};
            alias_revision = descriptor.revision();
            alias_prefix |= !descriptor.sealed();
            alias_complete |= descriptor.sealed();
          }
          return Status::success();
        };
        const auto subscribed_builds = builds.load();
        auto subscribed_result =
            context->execute(subscribed.value().plan, {{{"source", source}}},
                             {}, subscribed_options);
        if (!subscribed_result.ok() || alias_prefix || !alias_complete ||
            builds != subscribed_builds + 2) {
          std::cerr << "subscription Result failure code="
                    << static_cast<unsigned>(subscribed_result.status().code)
                    << " message=" << subscribed_result.status().message
                    << " prefix=" << alias_prefix
                    << " complete=" << alias_complete << " builds=" << builds
                    << " expected=" << subscribed_builds + 2 << " host_peak="
                    << root.statistics().peak[ResourceKind::Host]
                    << " metadata_peak="
                    << root.statistics().peak[ResourceKind::Metadata] << '\n';
        }
        PS_CHECK(subscribed_result.ok() && !alias_prefix && alias_complete &&
                 builds == subscribed_builds + 2);
        subscribed_result = Result<ExecutionResult>(ExecutionResult{});
        auto captured =
            context->freeze(compiled.value().plan, {{{"source", source}}});
        PS_CHECK(captured.ok());
        auto frozen = captured.take_value();
        options.result_publication = {};
        const auto before = builds.load();
        auto first = context->execute(frozen, {}, options);
        PS_CHECK(first.ok());
        options.maximum_result_window_bytes = 128;
        auto second = context->execute(frozen, {}, options);
        PS_CHECK(second.ok() && builds == before + 2);
        PS_CHECK(first.value().results.at("ids").object_id() !=
                 second.value().results.at("ids").object_id());
        first = Result<ExecutionResult>(ExecutionResult{});
        second = Result<ExecutionResult>(ExecutionResult{});
        unsigned delivered = 0;
        notified_nodes.clear();
        auto stream_options = options;
        stream_options.result_publication = [&](ValueRef ref,
                                                const ResultRef& object) {
          notified_nodes.insert(ref.node_id);
          if (!object.descriptor(false).ok())
            return Status{ErrorCode::Internal, {}};
          ++delivered;
          return Status::success();
        };
        auto streamed = context->execute(frozen, {}, stream_options);
        if (!streamed.ok())
          std::cerr << "stream failure: code="
                    << static_cast<unsigned>(streamed.status().code)
                    << " reason="
                    << static_cast<unsigned>(streamed.status().reason)
                    << " node=" << streamed.status().detail.node_id
                    << " host=" << root.statistics().live[ResourceKind::Host]
                    << " peak=" << root.statistics().peak[ResourceKind::Host]
                    << " message=" << streamed.status().message << '\n';
        PS_CHECK(streamed.ok() && delivered >= 2 && builds == before + 3);
        PS_CHECK(notified_nodes == std::set<std::uint64_t>({1, 2, 3}));
        first = Result<ExecutionResult>(ExecutionResult{});
        second = Result<ExecutionResult>(ExecutionResult{});
        streamed = Result<ExecutionResult>(ExecutionResult{});
        // With completed caching disabled each run computes a fresh object.
        auto rebuilt = context->execute(frozen, {}, options);
        PS_CHECK(rebuilt.ok() && builds == before + 4);
        rebuilt = Result<ExecutionResult>(ExecutionResult{});
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, resume = false;
        auto producer_options = options;
        producer_options.result_publication = [&](ValueRef ref,
                                                  const ResultRef&) {
          if (ref.node_id == 1) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!entered) {
              entered = true;
              changed.notify_all();
              changed.wait(lock, [&] { return resume; });
            }
          }
          return Status::success();
        };
        CancellationSource cancelled;
        auto producer = std::async(std::launch::async, [&] {
          return context->execute(frozen, cancelled.token(), producer_options);
        });
        {
          std::unique_lock<std::mutex> lock(mutex);
          PS_CHECK(changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return entered; }));
        }
        auto waiter = std::async(std::launch::async, [&] {
          return context->execute(frozen, {}, options);
        });
        const bool independent = waiter.wait_for(std::chrono::seconds(5)) ==
                                 std::future_status::ready;
        cancelled.cancel();
        {
          std::lock_guard<std::mutex> lock(mutex);
          resume = true;
        }
        changed.notify_all();
        auto cancelled_result = producer.get();
        auto surviving = waiter.get();
        if (!surviving.ok())
          std::cerr << "independent run failure "
                    << static_cast<int>(surviving.status().code) << " "
                    << surviving.status().message << '\n';
        PS_CHECK(cancelled_result.status().code == ErrorCode::Cancelled);
        PS_CHECK(independent && surviving.ok() && builds == before + 6);
        PS_CHECK(surviving.value().results.at("ids").descriptor().value().rows(
                     0) == expected_count);
        for (const auto& name : {"a", "b"}) {
          const auto& total = surviving.value().results.at(name);
          auto loaded = total.prepare_read(total.descriptor().value(), 0, 0, 1)
                            .value()
                            .load(8)
                            .take_value();
          std::int64_t value = 0;
          std::memcpy(&value, loaded->bytes().data(), 8);
          PS_CHECK(value == expected);
        }
        ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
        changes.push_back({PlanWorkflowInput{0}, 0,
                           take(TileSet::from_indices(root, {0})), false});
        auto dirty = surviving.value().dependencies.dirty_units(changes);
        PS_CHECK(dirty.ok() && dirty.value().size() == 3);
        surviving = Result<ExecutionResult>(ExecutionResult{});
        // Each run observes its own factory failure before continuation
        // creation.
        entered = false;
        resume = false;
        std::atomic<unsigned> start_attempts{0};
        start_control = [&](const ResultProgramQuery&) {
          ++start_attempts;
          std::unique_lock<std::mutex> lock(mutex);
          entered = true;
          changed.notify_all();
          changed.wait(lock, [&] { return resume; });
          return Status{ErrorCode::OperationFailed, "start fixture"};
        };
        auto failing = std::async(std::launch::async, [&] {
          return context->execute(frozen, {}, options);
        });
        {
          std::unique_lock<std::mutex> lock(mutex);
          PS_CHECK(changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return entered; }));
        }
        CancellationSource peer_cancel;
        auto peer = std::async(std::launch::async, [&] {
          return context->execute(frozen, peer_cancel.token(), options);
        });
        {
          std::lock_guard<std::mutex> lock(mutex);
          resume = true;
        }
        changed.notify_all();
        PS_CHECK(failing.get().status().code == ErrorCode::OperationFailed);
        const auto peer_ready = peer.wait_for(std::chrono::seconds(5));
        if (peer_ready != std::future_status::ready)
          peer_cancel.cancel();
        auto peer_result = peer.get();
        PS_CHECK(peer_ready == std::future_status::ready &&
                 peer_result.status().code == ErrorCode::OperationFailed &&
                 start_attempts == 2);
        // Run cancellation reaches a cooperative callback even when
        // that callback only polls its token and never calls consume_work.
        entered = false;
        std::atomic<bool> observed_cancel{false};
        start_control = [&](const ResultProgramQuery& query) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            entered = true;
          }
          changed.notify_all();
          const auto limit =
              std::chrono::steady_clock::now() + std::chrono::seconds(2);
          while (!query.cancellation.cancelled() &&
                 std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
          observed_cancel = query.cancellation.cancelled();
          return Status{ErrorCode::Cancelled, {}};
        };
        select_control = std::move(start_control);
        CancellationSource only_waiter;
        auto cooperative = std::async(std::launch::async, [&] {
          return context->execute(frozen, only_waiter.token(), options);
        });
        {
          std::unique_lock<std::mutex> lock(mutex);
          PS_CHECK(changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return entered; }));
        }
        only_waiter.cancel();
        PS_CHECK(cooperative.get().status().code == ErrorCode::Cancelled &&
                 observed_cancel);
        start_control = {};
        select_control = {};
        PS_CHECK(prefix_retirement(registry, config, document, input,
                                   forward_starts) == 0);
      }
      context.reset();
      if (count == 37 && page_bytes == 64) {
        options.result_publication = {};
        for (unsigned failure_mode : {1U, 2U, 4U}) {
          auto bounded_config = config;
          auto& limits = *bounded_config.managed_resources;
          if (failure_mode == 1)
            limits.maximum_work = 1;
          if (failure_mode == 2)
            limits.maximum_stages = 0;
          if (failure_mode == 4)
            bounded_config.maximum_live_bytes = 4;
          auto bounded =
              std::make_unique<ExecutionContext>(registry, bounded_config);
          auto ledger = bounded->resource_budget().take_value();
          auto admitted = source_result(ledger, input_schema, input);
          auto failed = admitted.ok()
                            ? bounded->execute(compiled.value().plan,
                                               {{{"source", admitted.value()}}},
                                               {}, options)
                            : Result<ExecutionResult>(admitted.status());
          admitted = Result<ResultRef>(ResultRef{});
          PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
          bounded.reset();
          for (auto live : ledger.statistics().live.values)
            PS_CHECK(live == 0);
        }
        failure_mode = 3;
        {
          ExecutionContext checked(registry, config);
          auto owned = source_result(checked.resource_budget().take_value(),
                                     input_schema, input)
                           .take_value();
          auto failed = checked.execute(compiled.value().plan,
                                        {{{"source", owned}}}, {}, options);
          PS_CHECK(failed_prefix.valid() &&
                   failed.status().reason == FailureReason::NotConverged);
          const auto retained = failed_prefix.production_status();
          PS_CHECK(retained.reason == failed.status().reason &&
                   retained.detail.node_id == failed.status().detail.node_id &&
                   retained.detail.scope == failed.status().detail.scope &&
                   retained.detail.node_id == 1 &&
                   retained.detail.scope == FailureScope::Group);
        }
        options.result_publication = {};
        failure_mode = 0;
        failed_prefix = {};
      }
      source = {};
      for (auto live : root.statistics().live.values)
        PS_CHECK(live == 0);
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(workflow() == 0);
  PS_CHECK(foreign_objects() == 0);
  PS_CHECK(foreign_backing() == 0);
  PS_CHECK(binding_contract() == 0);
  PS_CHECK(mandatory_effects() == 0);
  PS_CHECK(foreign_scalar_validation() == 0);
  PS_CHECK(sparse_source_need_budget() == 0);
  return 0;
}
