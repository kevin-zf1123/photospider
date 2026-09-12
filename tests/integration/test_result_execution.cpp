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
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
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
OperationTraits traits(SchemaTemplate schema, std::uint32_t inputs,
                       std::uint64_t state) {
  OperationTraits t;
  t.input_count = inputs;
  t.input_schema.resize(inputs);
  auto& output = t.outputs[0];
  output.dependency_version = 2;
  output.region_rule = OperationRegionRule::Dependency;
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
  std::uint64_t position = 0, count = 0, emitted = 0, batch = 0;
  unsigned stage = 0;
  ResultBuilder builder;
  ResultRelation relation;
  bool illegal_io = false;
  explicit SelectState(std::uint64_t n, bool forbidden = false)
      : count(n), illegal_io(forbidden) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (stage == 0) {
      if (illegal_io) {
        auto ignored = TemporaryStorage::create(phase.resources);
        static_cast<void>(ignored);
      }
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {count, count * 8});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto support =
          ResultRelation::cartesian(phase.resources, count, {0, 15, 0, count},
                                    DependencyGuarantee::Conservative);
      if (!support.ok())
        return Poll(support.status());
      relation = support.take_value();
      auto descriptor =
          ResultRelation::cartesian(phase.resources, 1, {0, 15, 0, count},
                                    DependencyGuarantee::Conservative);
      if (!descriptor.ok())
        return Poll(descriptor.status());
      auto bound = builder.bind_descriptor_relation(descriptor.take_value());
      if (!bound.ok())
        return Poll(bound);
      auto published =
          builder.publish(0, 0, relation, {true, true, true, true});
      if (!published.ok())
        return Poll(published);
      stage = 1;
      return Poll(ResultPublication{builder.reference(), false});
    }
    if (stage == 3) {
      auto status =
          builder.publish(0, emitted, relation, {true, true, true, true});
      if (!status.ok())
        return Poll(status);
      stage = 1;
      return Poll(ResultPublication{builder.reference(), false});
    }
    if (stage == 2) {
      auto workspace = phase.allocator.allocate(batch * 8);
      if (!workspace.ok())
        return Poll(workspace.status());
      auto bytes = workspace.take_value();
      std::uint64_t selected = 0;
      for (std::uint64_t i = 0; i < batch; ++i) {
        auto charged = phase.consume_work(1);
        if (!charged.ok())
          return Poll(charged);
        std::uint8_t value = 0;
        auto read = phase.read(0, {position + i}, &value, 1);
        if (!read.ok())
          return Poll(read);
        if (value % 3 == 1) {
          auto id = static_cast<std::int64_t>(position + i);
          std::memcpy(bytes.data() + selected * 8, &id, 8);
          ++selected;
        }
      }
      position += batch;
      if (selected) {
        auto exact = phase.allocator.allocate(selected * 8);
        if (!exact.ok())
          return Poll(exact.status());
        auto payload = exact.take_value();
        std::memcpy(payload.data(), bytes.data(), selected * 8);
        auto append =
            builder.prepare_append(0, selected, std::move(payload).freeze());
        if (!append.ok())
          return Poll(append.status());
        emitted += selected;
        stage = 3;
        return Poll(ResultProgramNeed{{}, {}, {append.take_value()}});
      }
      stage = 1;
    }
    if (position == count) {
      auto sealed = builder.seal();
      return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                         : Poll(sealed.status());
    }
    batch = std::min<std::uint64_t>(
        count - position,
        std::max<std::uint64_t>(1, phase.query.page_bytes / 16));
    stage = 2;
    auto samples = Footprint::from_regions(
        phase.query.inputs[0].descriptor.shape, {Region({{position, batch}})});
    if (!samples.ok())
      return Poll(samples.status());
    return Poll(ResultProgramNeed{{{0, samples.take_value()}}, {}, {}});
  }
};
struct SumState {
  unsigned stage = 0;
  ResultRef source;
  ResultDescriptor descriptor;
  ResultBuilder builder;
  std::uint64_t position = 0;
  std::int64_t sum = 0;
  Poll poll(const ResultProgramPhase& phase) {
    if (stage == 0) {
      stage = 1;
      return Poll(ResultProgramNeed{{}, {{0, 0, true, 0}}, {}});
    }
    if (stage == 1) {
      source = phase.results.at(0);
      auto facts = source.descriptor();
      if (!facts.ok())
        return Poll(facts.status());
      descriptor = facts.take_value();
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {}, {source.object_id()});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto witness = source.descriptor_relation();
      if (!witness.ok())
        return Poll(witness.status());
      auto bound = builder.bind_descriptor_relation(witness.value());
      if (!bound.ok())
        return Poll(bound);
      stage = 2;
    }
    if (stage == 3) {
      const auto& page =
          std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
      for (std::size_t offset = 0; offset < page->bytes().size(); offset += 8) {
        std::int64_t id = 0;
        std::memcpy(&id, page->bytes().data() + offset, 8);
        sum += id;
      }
      stage = 2;
    }
    if (stage == 2 && position < descriptor.rows(0)) {
      const auto count = std::min<std::uint64_t>(descriptor.rows(0) - position,
                                                 phase.query.page_bytes / 8);
      auto read = source.prepare_read(descriptor, 0, position, count);
      if (!read.ok())
        return Poll(read.status());
      position += count;
      stage = 3;
      return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
    }
    if (stage == 2) {
      auto buffer = phase.allocator.allocate(8);
      if (!buffer.ok())
        return Poll(buffer.status());
      auto bytes = buffer.take_value();
      std::memcpy(bytes.data(), &sum, 8);
      auto write = builder.prepare_append(0, 1, std::move(bytes).freeze());
      if (!write.ok())
        return Poll(write.status());
      stage = 4;
      return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
    }
    auto support = source.descriptor_relation();
    if (!support.ok())
      return Poll(support.status());
    auto published =
        builder.publish(0, 1, support.take_value(), {true, true, true, true});
    if (!published.ok())
      return Poll(published);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
// Identity copy with real prefix-to-prefix dependencies and mandatory paging.
struct ForwardState {
  unsigned stage = 0;
  std::uint64_t position = 0, batch = 0;
  ResultRef source;
  ResultBuilder builder;
  ResultRelation support;
  Poll poll(const ResultProgramPhase& phase) {
    if (stage == 0) {
      stage = 1;
      return Poll(ResultProgramNeed{{}, {{0, 0, false, position + 1}}, {}});
    }
    if (stage == 1) {
      source = phase.results.at(0);
      auto facts = source.descriptor(false);
      if (!facts.ok())
        return Poll(facts.status());
      if (!builder.reference().valid()) {
        auto made = ResultBuilder::start(
            phase.resources, *phase.query.output.result_schema,
            phase.query.semantic_key, {37, 37 * 8}, {source.object_id()});
        if (!made.ok())
          return Poll(made.status());
        builder = made.take_value();
        auto relation =
            ResultRelation::cartesian(phase.resources, 37, {0, 15, 0, 37},
                                      DependencyGuarantee::Conservative);
        if (!relation.ok())
          return Poll(relation.status());
        support = relation.take_value();
        auto descriptor =
            ResultRelation::cartesian(phase.resources, 1, {0, 15, 0, 37},
                                      DependencyGuarantee::Conservative);
        if (!descriptor.ok())
          return Poll(descriptor.status());
        auto bound = builder.bind_descriptor_relation(descriptor.take_value());
        if (!bound.ok())
          return Poll(bound);
      }
      if (position == facts.value().rows(0) && facts.value().sealed()) {
        auto sealed = builder.seal();
        return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                           : Poll(sealed.status());
      }
      batch = std::min(facts.value().rows(0) - position,
                       phase.query.page_bytes / 8);
      auto read = source.prepare_read(facts.value(), 0, position, batch);
      if (!read.ok())
        return Poll(read.status());
      stage = 2;
      return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
    }
    if (stage == 2) {
      auto append = builder.prepare_append(
          0, batch,
          std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0)));
      if (!append.ok())
        return Poll(append.status());
      stage = 3;
      return Poll(ResultProgramNeed{{}, {}, {append.take_value()}});
    }
    position += batch;
    auto published =
        builder.publish(0, position, support, {true, true, true, true});
    if (!published.ok())
      return Poll(published);
    stage = 0;
    return Poll(ResultPublication{builder.reference(), false});
  }
};
struct FirstState {
  unsigned stage = 0;
  Poll poll(const ResultProgramPhase& phase) {
    if (stage == 0) {
      stage = 1;
      return Poll(ResultProgramNeed{{}, {{0, 0, false, 1}}, {}});
    }
    if (stage == 1) {
      const auto& source = phase.results.at(0);
      auto facts = source.descriptor(false);
      if (!facts.ok())
        return Poll(facts.status());
      if (!facts.value().rows(0))
        return Poll(Status{ErrorCode::TypeMismatch, "nonempty test fixture"});
      auto read = source.prepare_read(facts.value(), 0, 0, 1);
      if (!read.ok())
        return Poll(read.status());
      stage = 2;
      return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
    }
    std::int64_t id = 0;
    auto page = std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
    std::memcpy(&id, page->bytes().data(), 8);
    const double value = static_cast<double>(id);
    auto allocated = MutableValue::allocate(
        {ElementType::Float64, {1}}, Region::whole({1}), phase.allocator);
    if (!allocated.ok())
      return Poll(allocated.status());
    auto output = allocated.take_value();
    std::memcpy(output.data(), &value, 8);
    auto published = std::move(output).publish();
    if (!published.ok())
      return Poll(published.status());
    auto fragments = ValueFragments::create({ElementType::Float64, {1}}, {},
                                            *phase.query.value_outputs,
                                            {published.take_value()});
    if (!fragments.ok())
      return Poll(fragments.status());
    auto relation = ResultRelation::cartesian(
        phase.resources, 1, {0, 15, 0, 1}, DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    return Poll(
        ResultValuePublication{fragments.take_value(), relation.take_value()});
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
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext execution(registry, config);
    auto frozen = execution.freeze(plan, {{{"source", input}}}).take_value();
    auto root = execution.resource_budget().take_value();
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
    if (failure == 2) {
      producer_options.result_publication = [&](ValueRef ref,
                                                const ResultRef&) {
        if (ref.node_id == 1) {
          pause();
          return Status{ErrorCode::OperationFailed, "observer fixture"};
        }
        return Status::success();
      };
    }
    CancellationSource cancelled;
    auto a = std::async(std::launch::async, [&] {
      return execution.execute_stream(
          frozen,
          [&](const std::string&, ValueView) {
            if (failure != 2)
              pause();
            if (failure == 1)
              throw std::runtime_error("sink fixture");
            return Status::success();
          },
          cancelled.token(), producer_options);
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      PS_CHECK(changed.wait_for(lock, std::chrono::seconds(5),
                                [&] { return entered; }));
    }
    const auto entries = root.statistics().live[ResourceKind::Entries];
    const auto started_forward = forward_starts.load();
    CancellationSource peer_cancel;
    auto b = std::async(std::launch::async, [&] {
      return execution.execute(frozen, peer_cancel.token(), options);
    });
    const auto limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (root.statistics().live[ResourceKind::Entries] <= entries &&
           std::chrono::steady_clock::now() < limit)
      std::this_thread::yield();
    PS_CHECK(b.wait_for(std::chrono::milliseconds(20)) ==
             std::future_status::timeout);
    if (failure == 3) {
      while (forward_starts == started_forward &&
             std::chrono::steady_clock::now() < limit)
        std::this_thread::yield();
      PS_CHECK(forward_starts > started_forward);
    }
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
    PS_CHECK(completed.value().values.at("a_first").as_float64().value() == 1);
    PS_CHECK(completed.value().results.at("z_all").descriptor().value().rows(
                 0) == 12);
  }
  return 0;
}
struct ForeignState {
  const ResultIoRequest* request;
  explicit ForeignState(const ResultIoRequest* value) : request(value) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (request)
      return Poll(ResultProgramNeed{{}, {}, {*request}});
    ResourceBudget other;
    auto builder =
        ResultBuilder::start(other, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto descriptor =
        ResultRelation::cartesian(other, 1, {0, 15, 0, 0},
                                  DependencyGuarantee::Conservative)
            .take_value();
    auto rows = ResultRelation::cartesian(other, 0, {0, 15, 0, 0},
                                          DependencyGuarantee::Conservative)
                    .take_value();
    auto bound = builder.bind_descriptor_relation(descriptor);
    if (!bound.ok())
      return Poll(bound);
    auto published = builder.publish(0, 0, rows, {true, true, true, true});
    if (!published.ok())
      return Poll(published);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
int foreign_objects() {
  auto registry = std::make_shared<OperationRegistry>();
  const ResultIoRequest* selected = nullptr;
  OperationDefinition operation;
  operation.key = "foreign";
  operation.traits = traits(ids_schema(), 0, sizeof(ForeignState));
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ForeignState>(allocator, selected);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok() &&
           registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "foreign", {}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  PS_CHECK(execution.execute(plan).status().code == ErrorCode::InvalidArgument);
  ResourceBudget other;
  auto temporary = TemporaryStorage::create(other).take_value();
  PS_CHECK(temporary.append_zeroed(8).ok());
  auto builder =
      ResultBuilder::start(other, sum_schema(), "foreign_ready").take_value();
  auto support = ResultRelation::cartesian(other, 1, {0, 15, 0, 0},
                                           DependencyGuarantee::Conservative)
                     .take_value();
  PS_CHECK(builder.bind_descriptor_relation(support).ok());
  std::int64_t scalar = 7;
  PS_CHECK(
      builder
          .append(0, 1,
                  ByteView(reinterpret_cast<const std::uint8_t*>(&scalar), 8))
          .ok());
  PS_CHECK(builder.publish(0, 1, support, {true, true, true, true}).ok());
  auto completed = builder.seal().take_value();
  auto read = completed.prepare_read(completed.descriptor().value(), 0, 0, 1)
                  .take_value();
  auto pending =
      ResultBuilder::start(other, sum_schema(), "foreign_pending").take_value();
  auto bytes = other.allocator().allocate(8).take_value();
  std::memcpy(bytes.data(), &scalar, 8);
  auto write =
      pending.prepare_append(0, 1, std::move(bytes).freeze()).take_value();
  std::vector<ResultIoRequest> actions{ResultReadTemporary{temporary, 0, 8},
                                       read, write};
  const auto issued = other.statistics().issued.io_requests;
  for (const auto& action : actions) {
    selected = &action;
    PS_CHECK(execution.execute(plan).status().code ==
             ErrorCode::InvalidArgument);
    PS_CHECK(other.statistics().issued.io_requests == issued);
  }
  ResourceBudget own;
  auto local = ResultBuilder::start(own, ids_schema(), "local").take_value();
  PS_CHECK(!local.bind_descriptor_relation(support).ok());
  PS_CHECK(!ResultRelation::unite(own, {support}).ok());
  PS_CHECK(!ResultRelation::compose(own, support, support, 100).ok());
  return 0;
}
struct ForeignValueState {
  const ValueFragments* output;
  explicit ForeignValueState(const ValueFragments* value) : output(value) {}
  Poll poll(const ResultProgramPhase& phase) {
    auto relation = ResultRelation::cartesian(
        phase.resources, 1, {0, 15, 0, 0}, DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    return Poll(ResultValuePublication{*output, relation.take_value()});
  }
};
int foreign_value() {
  auto scalar = Value::from_float64(42);
  auto coverage = Footprint::all({1}).take_value();
  auto fragments =
      ValueFragments::create(scalar.descriptor(), {}, coverage, {scalar})
          .take_value();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "foreign_value";
  operation.traits = traits(ids_schema(), 0, sizeof(ForeignValueState));
  auto& output = operation.traits.outputs[0];
  output.result_schema.reset();
  output.output_schema = {};
  output.output_element_type = ElementType::Float64;
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ForeignValueState>(allocator, &fragments);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "foreign_value", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  for (std::uint64_t limit : {0U, 8U}) {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Referenced] = limit;
    ExecutionContext context(registry, config);
    auto root = context.resource_budget().take_value();
    bool delivered = false;
    auto status = context.execute_stream(
        plan, {}, [&](const std::string&, ValueView view) {
          delivered = true;
          double value = 0;
          std::memcpy(&value, view.bytes().data(), 8);
          return value == 42 ? Status::success()
                             : Status{ErrorCode::Internal, {}};
        });
    PS_CHECK(status.ok() == (limit == 8));
    PS_CHECK(delivered == (limit == 8));
    if (!limit)
      PS_CHECK(status.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
  }
  return 0;
}
struct ManyPartsState {
  const Footprint* requested;
  const ValueFragments* output;
  bool waiting = false;
  ManyPartsState(const Footprint* need, const ValueFragments* value)
      : requested(need), output(value) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (!waiting) {
      waiting = true;
      return Poll(ResultProgramNeed{{{0, *requested}}, {}, {}});
    }
    return ForeignValueState(output).poll(phase);
  }
};
int source_parts_budget() {
  std::vector<Region> boxes;
  for (std::uint64_t i = 0; i < 128; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{2 * i, 1}});
  auto requested = Footprint::from_regions({256}, boxes).take_value();
  auto scalar = Value::from_float64(42);
  auto output =
      ValueFragments::create(scalar.descriptor(), {},
                             Footprint::all({1}).take_value(), {scalar})
          .take_value();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "many_parts";
  operation.traits = traits(ids_schema(), 1, sizeof(ManyPartsState));
  operation.traits.outputs[0].result_schema.reset();
  operation.traits.outputs[0].output_schema = {};
  operation.traits.outputs[0].output_element_type = ElementType::Float64;
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ManyPartsState>(allocator, &requested,
                                                    &output);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "source",
                      {ElementType::UInt8, {256}},
                      Region::whole({256}),
                      {0, {1}},
                      {}}};
  document.nodes = {{1, "many_parts", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto input =
      Value::create(document.inputs[0].descriptor, Region::whole({256}),
                    {0, {1}}, std::vector<std::uint8_t>(256))
          .take_value();
  for (std::uint64_t limit : {16384U, 1048576U}) {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = limit;
    config.managed_resources->capacity[ResourceKind::Metadata] = limit;
    ExecutionContext context(registry, config);
    ExecutionOptions options;
    options.maximum_dependency_work = 100000000;
    options.dependencies.sets.maximum_work = 100000000;
    auto result = context.execute(plan, {{{"source", input}}}, {}, options);
    if (limit != 16384 && !result.ok())
      std::cerr << "source parts fixture: "
                << static_cast<int>(result.status().code) << " "
                << result.status().message << "\n";
    if (limit == 16384)
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
    else
      PS_CHECK(result.ok() &&
               result.value().values.at("value").as_float64().value() == 42);
  }
  return 0;
}
int workflow() {
  for (std::uint64_t count : {0U, 37U, 8192U}) {
    for (std::uint64_t page_bytes : {64U, 256U}) {
      auto registry = std::make_shared<OperationRegistry>();
      std::atomic<unsigned> builds{0}, reads{0}, forward_starts{0};
      std::function<Status(const ResultProgramQuery&)> start_control,
          select_control;
      bool illegal_io = false;
      OperationDefinition select;
      select.key = "select_ids";
      select.traits = traits(ids_schema(), 1, sizeof(SelectState));
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
            illegal_io);
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
      first.traits = traits(ids_schema(), 1, sizeof(FirstState));
      first.traits.outputs[0].result_schema.reset();
      first.traits.outputs[0].output_schema = {};
      first.traits.outputs[0].output_element_type = ElementType::Float64;
      first.traits.input_schema[0].kind = OperationPortKind::Result;
      first.traits.input_schema[0].result_schema_id = ids_schema().id;
      first.traits.input_schema[0].result_schema_version = 1;
      first.start_result = [](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
        return ResultContinuation::make<FirstState>(allocator);
      };
      PS_CHECK(registry->register_operation(std::move(first)).ok());
      PS_CHECK(registry->freeze().ok());
      const auto source_count = std::max<std::uint64_t>(1, count);
      WorkflowDocument document;
      document.inputs = {{1,
                          "source",
                          {ElementType::UInt8, {source_count}},
                          Region::whole({source_count}),
                          {0, {1}},
                          {}}};
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
      PS_CHECK(compiled.value().plan.structured_network());
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ResourceLimits{};
      config.managed_resources->capacity[ResourceKind::Host] = 65536;
      config.managed_resources->capacity[ResourceKind::Metadata] = 65536;
      auto context = std::make_unique<ExecutionContext>(registry, config);
      auto root = context->resource_budget().take_value();
      auto source = std::make_shared<RegionalSource>();
      source->descriptor = document.inputs[0].descriptor;
      source->read = [&](const Region& region, std::uint8_t* bytes,
                         std::uint64_t size, const BufferAllocator&,
                         const CancellationToken&) {
        ++reads;
        for (std::uint64_t i = 0; i < size; ++i)
          bytes[i] = static_cast<std::uint8_t>(
              (region.dimensions()[0].offset + i) % 3);
        return Result<Region>(region);
      };
      ExecutionOptions options;
      options.maximum_result_window_bytes = page_bytes;
      options.dependencies.maximum_stages = 100000;
      std::uint64_t last_count = 0;
      options.result_publication = [&](ValueRef ref, const ResultRef& result) {
        if (ref.node_id == 1) {
          auto facts = result.descriptor(false);
          if (!facts.ok() || facts.value().rows(0) < last_count)
            return Status{ErrorCode::Internal, {}};
          last_count = facts.value().rows(0);
        }
        return Status::success();
      };
      auto executed = context->execute(compiled.value().plan,
                                       {{{"source", {}, source}}}, {}, options);
      if (!executed.ok())
        std::cerr << "structured failure "
                  << static_cast<int>(executed.status().code) << ' '
                  << executed.status().message << '\n';
      PS_CHECK(executed.ok());
      PS_CHECK(builds == 1 && (count != 0 || reads == 0));
      auto result = executed.take_value();
      PS_CHECK(result.results.at("a").object_id() ==
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
      const auto& total = result.results.at("a");
      auto page = total.prepare_read(total.descriptor().value(), 0, 0, 1)
                      .value()
                      .load(8)
                      .take_value();
      std::int64_t actual = 0;
      std::memcpy(&actual, page->bytes().data(), 8);
      PS_CHECK(actual == expected &&
               root.statistics().peak[ResourceKind::Host] <= 65536);
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
        std::vector<std::uint8_t> data(source_count);
        for (std::uint64_t i = 0; i < source_count; ++i)
          data[i] = i % 3;
        auto input = Value::create(document.inputs[0].descriptor,
                                   Region::whole({source_count}), {0, {1}},
                                   std::move(data))
                         .take_value();
        auto captured =
            context->freeze(compiled.value().plan, {{{"source", input}}});
        PS_CHECK(captured.ok());
        auto frozen = captured.take_value();
        options.result_publication = {};
        const auto before = builds.load();
        auto first = context->execute(frozen, {}, options);
        PS_CHECK(first.ok());
        options.maximum_result_window_bytes = 128;
        auto second = context->execute(frozen, {}, options);
        PS_CHECK(second.ok() && builds == before + 1);
        PS_CHECK(first.value().results.at("ids").object_id() ==
                 second.value().results.at("ids").object_id());
        unsigned delivered = 0;
        auto stream_options = options;
        stream_options.result_publication = [&](ValueRef,
                                                const ResultRef& object) {
          if (!object.descriptor().ok())
            return Status{ErrorCode::Internal, {}};
          ++delivered;
          return Status::success();
        };
        auto streamed = context->execute_stream(
            frozen,
            [](const std::string&, ValueView) { return Status::success(); }, {},
            stream_options);
        PS_CHECK(streamed.ok() && delivered >= 2 && builds == before + 1);
        first = Result<ExecutionResult>(ExecutionResult{});
        second = Result<ExecutionResult>(ExecutionResult{});
        // No optional cache owns completed data; expiry permits a new object.
        auto rebuilt = context->execute(frozen, {}, options);
        PS_CHECK(rebuilt.ok() && builds == before + 2);
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
        const auto entries = root.statistics().live[ResourceKind::Entries];
        auto waiter = std::async(std::launch::async, [&] {
          return context->execute(frozen, {}, options);
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (root.statistics().live[ResourceKind::Entries] < entries + 2 &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        PS_CHECK(root.statistics().live[ResourceKind::Entries] >= entries + 2);
        // The second request is blocked on the existing producer, never a
        // worker.
        PS_CHECK(waiter.wait_for(std::chrono::milliseconds(20)) ==
                 std::future_status::timeout);
        cancelled.cancel();
        {
          std::lock_guard<std::mutex> lock(mutex);
          resume = true;
        }
        changed.notify_all();
        auto cancelled_result = producer.get();
        auto surviving = waiter.get();
        if (!surviving.ok())
          std::cerr << "shared waiter failure "
                    << static_cast<int>(surviving.status().code) << " "
                    << surviving.status().message << '\n';
        PS_CHECK(cancelled_result.status().code == ErrorCode::Cancelled);
        PS_CHECK(surviving.ok() && builds == before + 3);
        surviving = Result<ExecutionResult>(ExecutionResult{});
        // A failure before the producer installs its continuation wakes peers.
        entered = false;
        resume = false;
        start_control = [&](const ResultProgramQuery&) {
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
        const auto failure_entries =
            root.statistics().live[ResourceKind::Entries];
        auto peer = std::async(std::launch::async, [&] {
          return context->execute(frozen, peer_cancel.token(), options);
        });
        const auto join_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (root.statistics().live[ResourceKind::Entries] <
                   failure_entries + 2 &&
               std::chrono::steady_clock::now() < join_deadline)
          std::this_thread::yield();
        PS_CHECK(root.statistics().live[ResourceKind::Entries] >=
                 failure_entries + 2);
        PS_CHECK(peer.wait_for(std::chrono::milliseconds(20)) ==
                 std::future_status::timeout);
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
                 peer_result.status().code == ErrorCode::OperationFailed);
        // Last-waiter cancellation reaches a cooperative callback even when
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
        for (unsigned failure_mode = 0; failure_mode < 5; ++failure_mode) {
          auto bounded_config = config;
          auto& limits = *bounded_config.managed_resources;
          if (failure_mode == 0)
            limits.capacity[ResourceKind::Disk] = 1;
          if (failure_mode == 1)
            limits.maximum_work = 1;
          if (failure_mode == 2)
            limits.maximum_stages = 0;
          if (failure_mode == 3)
            limits.maximum_io_bytes = 1;
          if (failure_mode == 4)
            bounded_config.maximum_live_bytes = 4;
          auto bounded =
              std::make_unique<ExecutionContext>(registry, bounded_config);
          auto ledger = bounded->resource_budget().take_value();
          auto failed = bounded->execute(
              compiled.value().plan, {{{"source", {}, source}}}, {}, options);
          PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
          bounded.reset();
          for (auto live : ledger.statistics().live.values)
            PS_CHECK(live == 0);
        }
        illegal_io = true;
        ExecutionContext checked(registry, config);
        const auto before_reads = reads.load();
        auto ignored = checked.execute(compiled.value().plan,
                                       {{{"source", {}, source}}}, {}, options);
        PS_CHECK(ignored.status().code == ErrorCode::InvalidArgument &&
                 reads == before_reads);
        illegal_io = false;
      }
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
  PS_CHECK(foreign_value() == 0);
  PS_CHECK(source_parts_budget() == 0);
  return 0;
}
