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
  auto descriptor = ResultRelation::cartesian(root, 1, {});
  if (!descriptor.ok())
    return Result<ResultRef>(descriptor.status());
  auto status = builder.bind_descriptor_relation(descriptor.take_value());
  if (!status.ok())
    return Result<ResultRef>(status);
  auto relation = ResultRelation::cartesian(
      root, schema.tensors[0].sample_count().value(), {});
  if (!relation.ok())
    return Result<ResultRef>(relation.status());
  status =
      builder.publish_tensor(0, backing.region(), backing.bytes(),
                             relation.take_value(), {true, true, true, true});
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
  unsigned illegal_io = 0;
  std::atomic<unsigned>* reads;
  SelectState(std::uint64_t n, unsigned forbidden,
              std::atomic<unsigned>* counter)
      : count(n), illegal_io(forbidden), reads(counter) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (stage == 0) {
      if (illegal_io == 1 || illegal_io == 2 || illegal_io == 4) {
        auto ignored = TemporaryStorage::create(phase.resources);
        static_cast<void>(ignored);
        if (illegal_io == 2)
          throw std::runtime_error("throw after protocol violation");
        if (illegal_io == 4)
          static_cast<void>(phase.consume_work(UINT64_MAX));
      }
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {count, count * 8});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto support = ResultRelation::cartesian(
          phase.resources, count,
          {0, 15, 0, count, ResultSupportTarget::Tensor, 0},
          DependencyGuarantee::Conservative);
      if (!support.ok())
        return Poll(support.status());
      relation = support.take_value();
      auto descriptor = ResultRelation::cartesian(
          phase.resources, 1, {0, 15, 0, count, ResultSupportTarget::Tensor, 0},
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
    if (stage == 1 && illegal_io == 3) {
      Status failure{ErrorCode::OperationFailed,
                     "producer stopped after prefix",
                     FailureReason::NotConverged};
      builder.fail(failure);
      return Poll(failure);
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
        auto read = phase.read_tensor(0, 0, {position + i}, &value, 1);
        if (!read.ok())
          return Poll(read);
        ++*reads;
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
        return Poll(ResultProgramNeed{{}, {append.take_value()}});
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
        phase.query.inputs[0].result_schema->tensors[0].sample_shape(),
        {Region({{position, batch}})});
    if (!samples.ok())
      return Poll(samples.status());
    return Poll(ResultProgramNeed{{}, {}, {{0, 0, samples.take_value(), 15}}});
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
      return Poll(ResultProgramNeed{{{0, 0, true, 0}}, {}});
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
      auto witness = ResultRelation::cartesian(
          phase.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0},
          DependencyGuarantee::Conservative);
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
      return Poll(ResultProgramNeed{{}, {read.take_value()}});
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
      return Poll(ResultProgramNeed{{}, {write.take_value()}});
    }
    auto support = ResultRelation::cartesian(
        phase.resources, 1,
        {0, 1, 0, descriptor.rows(0), ResultSupportTarget::Field, 0},
        DependencyGuarantee::Conservative);
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
      return Poll(ResultProgramNeed{{{0, 0, false, position + 1}}, {}});
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
        auto relation = ResultRelation::identity(phase.resources, 37, 0, 1,
                                                 ResultSupportTarget::Field, 0);
        if (!relation.ok())
          return Poll(relation.status());
        support = relation.take_value();
        auto descriptor = ResultRelation::cartesian(
            phase.resources, 1,
            {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0},
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
      return Poll(ResultProgramNeed{{}, {read.take_value()}});
    }
    if (stage == 2) {
      auto append = builder.prepare_append(
          0, batch,
          std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0)));
      if (!append.ok())
        return Poll(append.status());
      stage = 3;
      return Poll(ResultProgramNeed{{}, {append.take_value()}});
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
      return Poll(ResultProgramNeed{{{0, 0, false, 1}}, {}});
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
      return Poll(ResultProgramNeed{{}, {read.take_value()}});
    }
    std::int64_t id = 0;
    auto page = std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
    std::memcpy(&id, page->bytes().data(), 8);
    const double value = static_cast<double>(id);
    auto started =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!started.ok())
      return Poll(started.status());
    auto builder = started.take_value();
    auto basis = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0},
        DependencyGuarantee::Conservative);
    if (!basis.ok())
      return Poll(basis.status());
    auto status = builder.bind_descriptor_relation(basis.take_value());
    if (!status.ok())
      return Poll(status);
    auto relation = ResultRelation::cartesian(
        phase.resources, 1, {0, 1, 0, 1, ResultSupportTarget::Field, 0},
        DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    status = builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8),
        relation.take_value(), {true, true, true, true});
    if (!status.ok())
      return Poll(status);
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
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
    const auto entries = root.statistics().live[ResourceKind::Entries];
    const auto started_forward = forward_starts.load();
    const auto shared_before = execution.cache_statistics().shared_computations;
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
    const auto limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (root.statistics().live[ResourceKind::Entries] <= entries &&
           std::chrono::steady_clock::now() < limit)
      std::this_thread::yield();
    PS_CHECK(b.wait_for(std::chrono::milliseconds(20)) ==
             std::future_status::timeout);
    if (failure == 3) {
      while (forward_starts == started_forward &&
             execution.cache_statistics().shared_computations ==
                 shared_before &&
             std::chrono::steady_clock::now() < limit)
        std::this_thread::yield();
      PS_CHECK(forward_starts > started_forward ||
               execution.cache_statistics().shared_computations >
                   shared_before);
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
    PS_CHECK(scalar_result(completed.value().results.at("a_first")) == 1);
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
      return Poll(ResultProgramNeed{{}, {*request}});
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
    auto basis = ResultRelation::cartesian(phase.resources, 1, {});
    if (!basis.ok())
      return Poll(basis.status());
    auto status = builder.bind_descriptor_relation(basis.take_value());
    if (!status.ok())
      return Poll(status);
    auto relation = ResultRelation::cartesian(phase.resources, 1, {});
    if (!relation.ok())
      return Poll(relation.status());
    status = builder.publish_tensor(0, backing->region(), backing->layout(),
                                    backing->storage(), relation.take_value(),
                                    {true, true, true, true});
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
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
    auto descriptor = ResultRelation::cartesian(phase.resources, 1, {});
    if (!descriptor.ok())
      return Poll(descriptor.status());
    auto bound = builder.bind_descriptor_relation(descriptor.take_value());
    if (!bound.ok())
      return Poll(bound);
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
      const double numbers[] = {42.0 + slot, 43.0 + slot};
      const auto count = slot + 1;
      const auto published_count = *mode == 3 && slot == 1 ? 1U : count;
      auto support = ResultRelation::cartesian(phase.resources, count, {});
      if (!support.ok())
        return Poll(support.status());
      auto status = builder.publish_tensor(
          slot, Region({{0, published_count}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(numbers),
                   published_count * 8),
          support.take_value(), {true, true, true, true});
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
      auto first = Footprint::all({1}).take_value();
      auto second = Footprint::all({2}).take_value();
      return Poll(
          ResultProgramNeed{{}, {}, {{0, 0, first, 15}, {0, 1, second, 15}}});
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
    auto descriptor = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0});
    if (!descriptor.ok())
      return Poll(descriptor.status());
    auto status_bound =
        builder.bind_descriptor_relation(descriptor.take_value());
    if (!status_bound.ok())
      return Poll(status_bound);
    auto left = ResultRelation::cartesian(
        phase.resources, 1, {0, 5, 0, 1, ResultSupportTarget::Tensor, 0});
    auto right = ResultRelation::cartesian(
        phase.resources, 1, {0, 5, 0, 2, ResultSupportTarget::Tensor, 1});
    if (!left.ok() || !right.ok())
      return Poll(left.ok() ? right.status() : left.status());
    auto support = ResultRelation::unite(
        phase.resources, {left.take_value(), right.take_value()});
    if (!support.ok())
      return Poll(support.status());
    const double value = first + second;
    status = builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8),
        support.take_value(), {true, true, true, true});
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
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
               "tensor Need is outside published coverage" &&
           sealed_sparse && calls == missing_slot_calls + 1);
  mode = 0;
  document.nodes[1] = {2, "effect_constant", {}, {}};
  GraphContext unrelated(document);
  auto independent = Compiler(registry).compile(unrelated).take_value().plan;
  auto captured = context.freeze(independent, {}).take_value();
  DemandQuery empty{{"value", Footprint::none({1}).take_value()}};
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
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
struct ManyPartsState {
  const Footprint* requested;
  const Value* backing;
  bool waiting = false;
  std::atomic<unsigned>* needs;
  std::atomic<unsigned>* reads;
  ManyPartsState(const Footprint* need, const Value* value,
                 std::atomic<unsigned>* need_count,
                 std::atomic<unsigned>* read_count)
      : requested(need), backing(value), needs(need_count), reads(read_count) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (!waiting) {
      waiting = true;
      ++*needs;
      return Poll(ResultProgramNeed{{}, {}, {{0, 0, *requested, 1}}});
    }
    for (const auto& region : requested->boxes()) {
      std::uint8_t value = 1;
      auto status =
          phase.read_tensor(0, 0, {region.dimensions()[0].offset}, &value, 1);
      if (!status.ok())
        return Poll(status);
      ++*reads;
    }
    return ForeignBackingState(backing).poll(phase);
  }
};
int source_parts_budget() {
  std::vector<Region> boxes;
  for (std::uint64_t i = 0; i < 128; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{2 * i, 1}});
  auto requested = Footprint::from_regions({256}, boxes).take_value();
  auto scalar = Value::from_float64(42);
  const auto input_schema =
      tensor_schema(ElementType::UInt8, 256, "test.source_bytes");
  std::atomic<unsigned> needs{0}, reads{0};
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "many_parts";
  operation.traits =
      traits(tensor_schema(ElementType::Float64, 1, "test.sparse_output"), 1,
             sizeof(ManyPartsState));
  result_port(&operation.traits.input_schema[0], input_schema);
  operation.start_result = [&](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<ManyPartsState>(allocator, &requested,
                                                    &scalar, &needs, &reads);
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto input = Value::create({ElementType::UInt8, {256}}, Region::whole({256}),
                             {0, {1}}, std::vector<std::uint8_t>(256))
                   .take_value();
  // Source setup peaks below 17 KiB; 32 KiB admits setup and the callback,
  // then rejects the 128-part Need. The larger budget supplies every part.
  constexpr std::uint64_t low_limit = 32768;
  for (std::uint64_t limit : {low_limit, std::uint64_t{1048576}}) {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = limit;
    config.managed_resources->capacity[ResourceKind::Metadata] = limit;
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
    if (limit != low_limit && !result.ok())
      std::cerr << "source parts fixture: "
                << static_cast<int>(result.status().code) << " "
                << result.status().message << "\n";
    PS_CHECK(needs.load() == 1);
    if (limit == low_limit) {
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(reads.load() == 0);
    } else {
      PS_CHECK(result.ok() &&
               scalar_result(result.value().results.at("value")) == 42);
      PS_CHECK(reads.load() == 128);
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
      unsigned illegal_io = 0;
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
            illegal_io, &reads);
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
      config.managed_resources->capacity[ResourceKind::Host] = 262144;
      config.managed_resources->capacity[ResourceKind::Metadata] = 262144;
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
      PS_CHECK(selection_timing && total_elements == 1);
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
        // Explicit output interest exercises producer alias notices even when
        // the two identical scalar Result consumers share their continuation.
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
        if (!subscribed_result.ok() || !alias_prefix || !alias_complete ||
            builds != subscribed_builds + 1) {
          std::cerr << "subscription Result failure code="
                    << static_cast<unsigned>(subscribed_result.status().code)
                    << " message=" << subscribed_result.status().message
                    << " prefix=" << alias_prefix
                    << " complete=" << alias_complete << " builds=" << builds
                    << " expected=" << subscribed_builds + 1 << " host_peak="
                    << root.statistics().peak[ResourceKind::Host]
                    << " metadata_peak="
                    << root.statistics().peak[ResourceKind::Metadata] << '\n';
        }
        PS_CHECK(subscribed_result.ok() && alias_prefix && alias_complete &&
                 builds == subscribed_builds + 1);
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
        PS_CHECK(second.ok() && builds == before + 1);
        PS_CHECK(first.value().results.at("ids").object_id() ==
                 second.value().results.at("ids").object_id());
        unsigned delivered = 0;
        notified_nodes.clear();
        auto stream_options = options;
        stream_options.result_publication = [&](ValueRef ref,
                                                const ResultRef& object) {
          notified_nodes.insert(ref.node_id);
          if (!object.descriptor().ok())
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
        PS_CHECK(streamed.ok() && delivered >= 2 && builds == before + 1);
        PS_CHECK(notified_nodes == std::set<std::uint64_t>({1, 2, 3}));
        first = Result<ExecutionResult>(ExecutionResult{});
        second = Result<ExecutionResult>(ExecutionResult{});
        streamed = Result<ExecutionResult>(ExecutionResult{});
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
        PS_CHECK(builds == before + 3);
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
        // The shared sum holds dependency facts, not the selected-id payload.
        // After producer retirement the separately named ids output may
        // rebuild an expired weak entry; active sharing was checked above.
        PS_CHECK(surviving.ok() && builds >= before + 3 &&
                 builds <= before + 4);
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
        auto changed_source = Footprint::all({source_count}).take_value();
        auto dirty = surviving.value().dependencies.potential_dirty(
            "source", changed_source, 1);
        PS_CHECK(dirty.ok());
        for (const auto& name : {"a", "b", "ids"})
          PS_CHECK(!dirty.value().at(name).empty());
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
        for (unsigned violation : {1u, 2u, 4u}) {
          illegal_io = violation;
          ExecutionContext checked(registry, config);
          auto owned = source_result(checked.resource_budget().take_value(),
                                     input_schema, input)
                           .take_value();
          const auto before_reads = reads.load();
          auto ignored = checked.execute(compiled.value().plan,
                                         {{{"source", owned}}}, {}, options);
          PS_CHECK(ignored.status().code == ErrorCode::InvalidArgument &&
                   ignored.status().reason == FailureReason::UnauthorizedRead &&
                   ignored.status().detail.origin == FailureOrigin::Protocol &&
                   ignored.status().detail.scope == FailureScope::Group &&
                   reads == before_reads);
        }
        illegal_io = 3;
        {
          ExecutionContext checked(registry, config);
          auto owned = source_result(checked.resource_budget().take_value(),
                                     input_schema, input)
                           .take_value();
          ResultRef prefix;
          options.result_publication = [&](ValueRef ref,
                                           const ResultRef& result) {
            if (ref.node_id == 1)
              prefix = result;
            return Status::success();
          };
          auto failed = checked.execute(compiled.value().plan,
                                        {{{"source", owned}}}, {}, options);
          PS_CHECK(prefix.valid() &&
                   failed.status().reason == FailureReason::NotConverged);
          const auto retained = prefix.production_status();
          PS_CHECK(retained.reason == failed.status().reason &&
                   retained.detail.node_id == failed.status().detail.node_id &&
                   retained.detail.scope == failed.status().detail.scope &&
                   retained.detail.node_id == 1 &&
                   retained.detail.scope == FailureScope::Group);
        }
        options.result_publication = {};
        illegal_io = 0;
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
  PS_CHECK(source_parts_budget() == 0);
  return 0;
}
