#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
const char* selected_fixture = nullptr;
std::uint64_t root_capacity = 524288;
std::uint64_t executed_fixtures = 0;
enum class WireBacking { Dense, Reversed, Split, BroadcastZero };
const char* backing_name(WireBacking backing) {
  switch (backing) {
    case WireBacking::Reversed:
      return "reversed";
    case WireBacking::Split:
      return "split";
    case WireBacking::BroadcastZero:
      return "broadcast_zero";
    default:
      return "dense";
  }
}
void check(bool valid, const char* detail) {
  if (!valid)
    throw std::runtime_error(detail);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
struct PackState {
  explicit PackState(std::shared_ptr<std::atomic<std::uint64_t>> counter)
      : reads(std::move(counter)) {}
  std::shared_ptr<std::atomic<std::uint64_t>> reads;
  ResultBuilder builder;
  ResultTensorReadWindow input;
  std::array<std::uint64_t, 16> rows{};
  unsigned stage = 0, field = 0;
  std::uint64_t position = 0, offset = 128, batch = 0, width = 0;
  Poll poll(const ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    const auto shape =
        phase.query.inputs[0].result_schema->tensors[0].sample_shape();
    if (stage == 0) {
      stage = 1;
      auto requested = Footprint::from_regions(shape, {Region::whole(shape)});
      return requested.ok()
                 ? Poll(ResultProgramNeed{{},
                                          {},
                                          {{0, 0, requested.take_value(), 15}}})
                 : Poll(requested.status());
    }
    if (stage == 1) {
      auto acquired = phase.tensors->at({0, 0}).acquire(
          Region::whole(shape), phase.query.cancellation);
      if (!acquired.ok())
        return Poll(acquired.status());
      input = acquired.take_value();
      std::array<std::uint8_t, 128> header{};
      auto copied = read_wire(phase, 0, header.size(), header.data());
      if (!copied.ok())
        return Poll(copied);
      std::uint64_t total = 128;
      for (unsigned i = 0; i < 16; ++i) {
        for (unsigned byte = 0; byte < 8; ++byte) {
          const auto value = header[8 * i + byte];
          rows[i] |= static_cast<std::uint64_t>(value) << (8 * byte);
        }
        if (i >= schema.fields.size()) {
          if (rows[i])
            return Poll(Status{ErrorCode::TypeMismatch, "unused wire count"});
          continue;
        }
        const auto bytes = schema.row_bytes(i).value();
        if (rows[i] > 1048576 || rows[i] > (UINT64_MAX - total) / bytes)
          return Poll(Status{ErrorCode::TypeMismatch, "wire count overflow"});
        total += rows[i] * bytes;
      }
      if (total != shape[0])
        return Poll(Status{ErrorCode::TypeMismatch, "wire length mismatch"});
      auto made = ResultBuilder::start(
          phase.resources, schema, phase.query.semantic_key, {},
          phase.association
              ? std::vector<std::uint64_t>(phase.association->begin(),
                                           phase.association->end())
              : std::vector<std::uint64_t>{});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto relation = ResultRelation::cartesian(
          phase.resources, 1, {0, 15, 0, total, ResultSupportTarget::Tensor, 0},
          DependencyGuarantee::Conservative);
      if (!relation.ok())
        return Poll(relation.status());
      auto bound = builder.bind_descriptor_relation(relation.take_value());
      if (!bound.ok())
        return Poll(bound);
      stage = 2;
    }
    while (field < schema.fields.size()) {
      width = schema.row_bytes(field).value();
      if (position == rows[field]) {
        auto relation = ResultRelation::cartesian(
            phase.resources, rows[field],
            {0, 15, 0, shape[0], ResultSupportTarget::Tensor, 0},
            DependencyGuarantee::Conservative);
        if (!relation.ok())
          return Poll(relation.status());
        auto published =
            builder.publish(field, rows[field], relation.take_value(),
                            {true, true, true, true});
        if (!published.ok())
          return Poll(published);
        ++field;
        position = 0;
        continue;
      }
      if (phase.query.page_bytes < width)
        return Poll(
            Status{ErrorCode::ResourceExhausted, "record window too small"});
      batch = std::min(rows[field] - position, phase.query.page_bytes / width);
      return append_chunk(phase);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
  Status read_wire(const ResultProgramPhase& phase, std::uint64_t begin,
                   std::uint64_t count, std::uint8_t* output) {
    for (std::uint64_t copied = 0; copied < count;) {
      auto queried = input.row_run({begin + copied});
      if (!queried.ok())
        return queried.status();
      const auto& run = queried.value();
      const auto length = std::min(count - copied, run.samples);
      if (!length)
        return Status{ErrorCode::InvalidArgument, "empty wire read run"};
      auto charged = phase.consume_work(length);
      if (!charged.ok())
        return charged;
      for (std::uint64_t i = 0; i < length; ++i)
        output[copied + i] =
            run.data[static_cast<std::int64_t>(i) * run.sample_stride_bytes];
      *reads += length;
      copied += length;
    }
    return Status::success();
  }
  Poll append_chunk(const ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    auto allocation = phase.allocator.allocate(batch * width);
    if (!allocation.ok())
      return Poll(allocation.status());
    auto bytes = allocation.take_value();
    auto copied = read_wire(phase, offset, batch * width, bytes.data());
    if (!copied.ok())
      return Poll(copied);
    if (schema.id == "photospider.path_set" && field == 5) {
      // The wire's placeholder is rebound to the new immutable object
      // identity.
      const auto id = builder.reference().object_id();
      for (std::uint64_t i = 0; i < batch; ++i)
        std::memcpy(bytes.data() + i * width + 24, &id, 8);
    }
    auto write =
        builder.prepare_append(field, batch, std::move(bytes).freeze());
    if (!write.ok())
      return Poll(write.status());
    position += batch;
    offset += batch * width;
    stage = 2;
    return Poll(ResultProgramNeed{{}, {write.take_value()}});
  }
};
struct InspectState {
  bool waiting = false;
  Poll poll(const ResultProgramPhase& phase) {
    if (!waiting) {
      waiting = true;
      return Poll(ResultProgramNeed{{{0, 0, true, 0}}, {}});
    }
    const auto& source = phase.results.at(0);
    const auto descriptor = source.descriptor().value();
    double total = 0;
    for (unsigned i = 0; i < descriptor.field_count(); ++i)
      total += descriptor.rows(i);
    auto builder = ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, {source.object_id()});
    if (!builder.ok())
      return Poll(builder.status());
    auto result = builder.take_value();
    auto relation = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0},
        DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    auto status = result.bind_descriptor_relation(relation.value());
    if (!status.ok())
      return Poll(status);
    status = result.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&total), 8),
        relation.take_value(), {true, true, true, true});
    if (!status.ok())
      return Poll(status);
    auto sealed = result.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
struct RefineState {
  ResultRef source;
  ResultDescriptor descriptor;
  ResultBuilder builder;
  unsigned stage = 0;
  std::uint64_t position = 0, batch = 0, count = 0;
  std::int64_t generation = 0, iteration = 0;
  double residual = 0;
  Result<ResultRelation> source_support(const ResultProgramPhase& phase,
                                        std::uint64_t rows) const {
    std::vector<ResultRelation> parts;
    auto facts = ResultRelation::cartesian(
        phase.resources, rows, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0},
        DependencyGuarantee::Conservative);
    if (!facts.ok())
      return facts;
    parts.push_back(facts.take_value());
    for (std::uint32_t field = 0; field < descriptor.field_count(); ++field) {
      auto support = ResultRelation::cartesian(
          phase.resources, rows,
          {0, 15, 0, descriptor.rows(field), ResultSupportTarget::Field, field},
          DependencyGuarantee::Conservative);
      if (!support.ok())
        return support;
      parts.push_back(support.take_value());
    }
    return ResultRelation::unite(phase.resources, parts);
  }
  Poll poll(const ResultProgramPhase& phase) {
    if (stage == 0) {
      stage = 1;
      return Poll(ResultProgramNeed{{{0, 0, true, 0}}, {}});
    }
    if (stage == 1) {
      source = phase.results.at(0);
      descriptor = source.descriptor().value();
      auto input_spec = iterative_spec(source.schema());
      auto output_spec = iterative_spec(*phase.query.output.result_schema);
      if (!input_spec.ok() || !output_spec.ok() ||
          input_spec.value().size != output_spec.value().size ||
          input_spec.value().system_snapshot !=
              output_spec.value().system_snapshot ||
          input_spec.value().initialization !=
              output_spec.value().initialization)
        return Poll(Status{ErrorCode::TypeMismatch,
                           "iteration system association mismatch"});
      count = output_spec.value().size;
      auto read = source.prepare_read(descriptor, 0, 0, 1);
      if (!read.ok())
        return Poll(read.status());
      stage = 2;
      return Poll(ResultProgramNeed{{}, {read.take_value()}});
    }
    if (stage == 2) {
      const auto& page =
          std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
      std::memcpy(&generation, page->bytes().data(), 8);
      std::memcpy(&iteration, page->bytes().data() + 8, 8);
      const auto spec =
          iterative_spec(*phase.query.output.result_schema).value();
      if (generation <= 0 || generation == INT64_MAX || iteration < 0 ||
          static_cast<std::uint64_t>(iteration) >= spec.maximum_iterations)
        return Poll(Status{ErrorCode::TypeMismatch,
                           "iteration generation or count exhausted"});
      ++generation;
      ++iteration;
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {}, {source.object_id()});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto relation = source_support(phase, 1);
      if (!relation.ok())
        return Poll(relation.status());
      auto bound = builder.bind_descriptor_relation(relation.take_value());
      if (!bound.ok())
        return Poll(bound);
      stage = 3;
    }
    if (stage == 4) {
      auto diagonal =
          std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
      auto rhs = std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(1));
      auto made = phase.allocator.allocate(batch * 8);
      if (!made.ok())
        return Poll(made.status());
      auto estimate = made.take_value();
      for (std::uint64_t i = 0; i < batch; ++i) {
        auto charged = phase.consume_work(1);
        if (!charged.ok())
          return Poll(charged);
        double a = 0, b = 0;
        std::memcpy(&a, diagonal->bytes().data() + 8 * i, 8);
        std::memcpy(&b, rhs->bytes().data() + 8 * i, 8);
        if (a == 0)
          return Poll(Status{ErrorCode::TypeMismatch, "singular diagonal"});
        const auto x = b / a;
        volatile double product = a * x;
        const auto r = b - product;
        if (!std::isfinite(x) || !std::isfinite(r))
          return Poll(Status{ErrorCode::OperationFailed,
                             "nonfinite diagonal iteration"});
        residual = std::max(residual, std::abs(r));
        std::memcpy(estimate.data() + i * 8, &x, 8);
      }
      auto x = builder.prepare_append(1, batch, std::move(estimate).freeze());
      auto a = builder.prepare_append(2, batch, diagonal);
      auto b = builder.prepare_append(3, batch, rhs);
      if (!x.ok() || !a.ok() || !b.ok())
        return Poll(!x.ok() ? x.status() : !a.ok() ? a.status() : b.status());
      position += batch;
      stage = 3;
      return Poll(
          ResultProgramNeed{{},
                            {x.take_value(), a.take_value(), b.take_value()}});
    }
    if (stage == 3 && position < count) {
      batch = std::min(count - position, phase.query.page_bytes / 8);
      if (!batch)
        return Poll(
            Status{ErrorCode::ResourceExhausted, "iteration window too small"});
      auto a = source.prepare_read(descriptor, 2, position, batch),
           b = source.prepare_read(descriptor, 3, position, batch);
      if (!a.ok() || !b.ok())
        return Poll(!a.ok() ? a.status() : b.status());
      stage = 4;
      return Poll(ResultProgramNeed{{}, {a.take_value(), b.take_value()}});
    }
    if (stage == 3) {
      const auto spec =
          iterative_spec(*phase.query.output.result_schema).value();
      if (residual > spec.target_residual)
        return Poll(Status{ErrorCode::OperationFailed,
                           "diagonal step did not converge"});
      auto state = phase.allocator.allocate(24),
           error = phase.allocator.allocate(8);
      if (!state.ok() || !error.ok())
        return Poll(!state.ok() ? state.status() : error.status());
      auto state_bytes = state.take_value(), error_bytes = error.take_value();
      const std::int64_t values[] = {generation, iteration, 0};
      std::memcpy(state_bytes.data(), values, 24);
      std::memcpy(error_bytes.data(), &residual, 8);
      auto a = builder.prepare_append(0, 1, std::move(state_bytes).freeze()),
           b = builder.prepare_append(4, 1, std::move(error_bytes).freeze());
      if (!a.ok() || !b.ok())
        return Poll(!a.ok() ? a.status() : b.status());
      stage = 5;
      return Poll(ResultProgramNeed{{}, {a.take_value(), b.take_value()}});
    }
    for (unsigned field = 0; field < 5; ++field) {
      auto relation =
          source_support(phase, field == 0 || field == 4 ? 1 : count);
      if (!relation.ok())
        return Poll(relation.status());
      auto status =
          builder.publish(field, field == 0 || field == 4 ? 1 : count,
                          relation.take_value(), {true, true, true, true});
      if (!status.ok())
        return Poll(status);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
struct Fixture {
  std::string name;
  SchemaTemplate schema;
  std::array<std::vector<std::uint8_t>, 16> fields;
  template <class T>
  void set(unsigned field, const std::vector<T>& values) {
    fields[field].resize(values.size() * sizeof(T));
    if (!values.empty())
      std::memcpy(fields[field].data(), values.data(), fields[field].size());
  }
  std::vector<std::uint8_t> wire() const {
    std::vector<std::uint8_t> result(128);
    for (unsigned i = 0; i < schema.fields.size(); ++i) {
      auto width = schema.row_bytes(i).value();
      check(fields[i].size() % width == 0, "fixture row alignment");
      auto rows = fields[i].size() / width;
      for (unsigned byte = 0; byte < 8; ++byte)
        result[i * 8 + byte] = static_cast<std::uint8_t>(rows >> (8 * byte));
      result.insert(result.end(), fields[i].begin(), fields[i].end());
    }
    return result;
  }
};
SchemaTemplate tensor_schema(const char* id, ElementType type,
                             std::uint64_t count) {
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
OperationTraits staged(std::uint64_t bytes) {
  OperationTraits t;
  t.input_count = 1;
  t.input_schema.resize(1);
  t.outputs[0].region_rule = OperationRegionRule::Dependency;
  t.outputs[0].continuation_bytes = bytes;
  t.outputs[0].maximum_dependency_stages = 100000;
  t.workspace_bytes = 4096;
  return t;
}
void run(const Fixture& fixture, bool expected = true,
         std::uint64_t window = 256, bool refine = false,
         bool domain_limit = false, WireBacking backing = WireBacking::Dense) {
  if (selected_fixture && selected_fixture != fixture.name)
    return;
  ++executed_fixtures;
  const auto wire = fixture.wire();
  const auto input_schema = tensor_schema("example.representation_wire",
                                          ElementType::UInt8, wire.size());
  const auto summary_schema =
      tensor_schema("example.representation_summary", ElementType::Float64, 1);
  auto reads = std::make_shared<std::atomic<std::uint64_t>>(0);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition pack;
  pack.key = "example.pack";
  pack.traits = staged(sizeof(PackState));
  result_port(&pack.traits.input_schema[0], input_schema);
  pack.traits.outputs[0].result_schema = fixture.schema;
  pack.traits.outputs[0].output_schema.kind = OperationPortKind::Result;
  pack.traits.outputs[0].output_schema.result_schema_id = fixture.schema.id;
  pack.traits.outputs[0].output_schema.result_schema_version = 1;
  pack.start_result = [reads](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
    return ResultContinuation::make<PackState>(allocator, reads);
  };
  check(registry->register_operation(std::move(pack)).ok(), "register pack");
  OperationDefinition inspect;
  inspect.key = "example.inspect";
  inspect.traits = staged(sizeof(InspectState));
  inspect.traits.input_schema[0].kind = OperationPortKind::Result;
  inspect.traits.input_schema[0].result_schema_id = fixture.schema.id;
  inspect.traits.input_schema[0].result_schema_version = 1;
  inspect.traits.outputs[0].result_schema = summary_schema;
  result_port(&inspect.traits.outputs[0].output_schema, summary_schema);
  inspect.start_result = [](const ResultProgramQuery&,
                            const BufferAllocator& allocator) {
    return ResultContinuation::make<InspectState>(allocator);
  };
  check(registry->register_operation(std::move(inspect)).ok(),
        "register inspect");
  if (refine) {
    auto spec = take(iterative_spec(fixture.schema));
    spec.maximum_iterations = 1;
    spec.require_converged = true;
    OperationDefinition next;
    next.key = "example.diagonal_step";
    next.traits = staged(sizeof(RefineState));
    next.traits.input_schema[0].kind = OperationPortKind::Result;
    next.traits.input_schema[0].result_schema_id = fixture.schema.id;
    next.traits.input_schema[0].result_schema_version = 1;
    auto& out = next.traits.outputs[0];
    out.result_schema = take(iterative_schema(spec));
    out.output_schema.kind = OperationPortKind::Result;
    out.output_schema.result_schema_id = fixture.schema.id;
    out.output_schema.result_schema_version = 1;
    next.start_result = [](const ResultProgramQuery&,
                           const BufferAllocator& allocator) {
      return ResultContinuation::make<RefineState>(allocator);
    };
    check(registry->register_operation(std::move(next)).ok(),
          "register diagonal step");
  }
  check(registry->freeze().ok(), "freeze registry");
  WorkflowDocument document;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema = std::make_shared<const SchemaTemplate>(input_schema);
  document.inputs = {input};
  document.nodes = {
      {1, "example.pack", {WorkflowInputReference{1}}, {}},
      {2, "example.inspect", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"data", 1, "value"}, {"summary", 2, "value"}};
  if (refine) {
    document.nodes.push_back(
        {3, "example.diagonal_step", {WorkflowNodeOutput{1, "value"}}, {}});
    document.nodes.push_back(
        {4, "example.inspect", {WorkflowNodeOutput{3, "value"}}, {}});
    document.outputs.push_back({"next", 3, "value"});
    document.outputs.push_back({"next_summary", 4, "value"});
  }
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Host] = root_capacity;
  config.managed_resources->capacity[ResourceKind::Metadata] = root_capacity;
  ResourceBudget root;
  ResultRef held, next;
  {
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    auto source_builder =
        take(ResultBuilder::start(root, input_schema, "example.wire"));
    check(source_builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "wire descriptor");
    const auto publish = [&](const Region& region, ByteView bytes) {
      check(source_builder
                .publish_tensor(
                    0, region, bytes,
                    take(ResultRelation::cartesian(root, wire.size(), {})),
                    {true, true, true, true})
                .ok(),
            "wire publication");
    };
    if (backing == WireBacking::Split) {
      const auto split = std::uint64_t{64};
      publish(Region({{0, split}}), ByteView(wire.data(), split));
      publish(Region({{split, wire.size() - split}}),
              ByteView(wire.data() + split, wire.size() - split));
    } else if (backing == WireBacking::Reversed ||
               backing == WireBacking::BroadcastZero) {
      const bool broadcast = backing == WireBacking::BroadcastZero;
      if (broadcast)
        check(std::all_of(wire.begin(), wire.end(),
                          [](auto byte) { return byte == 0; }),
              "broadcast wire must be zero");
      auto storage =
          take(root.allocator().allocate(broadcast ? 1 : wire.size()));
      if (broadcast)
        storage.data()[0] = 0;
      else
        std::reverse_copy(wire.begin(), wire.end(), storage.data());
      StridedLayout layout{broadcast ? 0 : wire.size() - 1,
                           {broadcast ? 0 : -1}};
      check(source_builder
                .publish_tensor(
                    0, Region::whole({wire.size()}), layout,
                    std::move(storage).freeze(),
                    take(ResultRelation::cartesian(root, wire.size(), {})),
                    {true, true, true, true})
                .ok(),
            "strided wire publication");
    } else {
      publish(Region::whole({wire.size()}), ByteView(wire.data(), wire.size()));
    }
    auto source = take(source_builder.seal());
    ExecutionOptions options;
    options.maximum_result_window_bytes = window;
    options.maximum_dependency_work = 10000000;
    bool observed = false;
    options.result_publication = [&](ValueRef, const ResultRef&) {
      observed = true;
      return Status::success();
    };
    auto result =
        context.execute(compiled.plan, {{{"source", source}}}, {}, options);
    if (result.ok() != expected) {
      const auto stats = root.statistics();
      std::cerr << "Root host peak=" << stats.peak[ResourceKind::Host]
                << " metadata peak=" << stats.peak[ResourceKind::Metadata]
                << " payload peak=" << stats.peak[ResourceKind::Payload]
                << " wire bytes=" << wire.size() << '\n';
    }
    if (result.ok() != expected)
      throw std::runtime_error(
          fixture.name +
          (result.ok() ? ": unexpected success"
                       : ": rejected: code=" +
                             std::to_string(
                                 static_cast<unsigned>(result.status().code)) +
                             " reason=" +
                             std::to_string(static_cast<unsigned>(
                                 result.status().reason)) +
                             " " + result.status().message));
    check(observed == expected, "validation must precede observation");
    if (!expected) {
      if (domain_limit)
        check(result.status().code == ErrorCode::OperationFailed &&
                  result.status().reason == FailureReason::InvalidDomain &&
                  result.status().detail.origin == FailureOrigin::Domain &&
                  result.status().detail.scope == FailureScope::Association &&
                  result.status().detail.association != 0,
              "semantic count failure must preserve Domain association");
      std::cout << fixture.name << " rejected before publication\n";
      return;
    }
    check(reads->load() == wire.size(),
          "wire bytes are read once across all I/O phases");
    held = result.value().results.at("data");
    if (refine)
      next = result.value().results.at("next");
    std::uint64_t total = 0;
    for (unsigned i = 0; i < fixture.schema.fields.size(); ++i)
      total += fixture.fields[i].size() / fixture.schema.row_bytes(i).value();
    const auto& summary = result.value().results.at("summary");
    double actual = 0;
    check(summary.read_tensor(take(summary.descriptor()), 0, {0}, &actual, 8)
                  .ok() &&
              actual == total,
          "active consumer row count");
    check(held.association().size() == 1 &&
              held.association()[0] == source.object_id(),
          "packed Result retains wire source identity");
    check(summary.association().size() == 1 &&
              summary.association()[0] == held.object_id(),
          "summary retains its Result source identity");
    if (refine) {
      const auto& next_summary = result.value().results.at("next_summary");
      actual = 0;
      check(next_summary
                    .read_tensor(take(next_summary.descriptor()), 0, {0},
                                 &actual, 8)
                    .ok() &&
                actual == total,
            "iterated Result summary row count");
      check(next_summary.association().size() == 1 &&
                next_summary.association()[0] == next.object_id(),
            "iterated summary retains its source identity");
    }
  }
  const auto descriptor = take(held.descriptor());
  // The reference is byte-for-byte fixture data, independently of the
  // producer's page partition. Runtime ObjectId is checked separately for
  // PrimitiveRefs.
  for (unsigned field = 0; field < descriptor.field_count(); ++field) {
    const auto width = fixture.schema.row_bytes(field).value();
    for (std::uint64_t row = 0; row < descriptor.rows(field);) {
      const auto count =
          std::min<std::uint64_t>(descriptor.rows(field) - row, window / width);
      auto page = take(
          take(held.prepare_read(descriptor, field, row, count)).load(window));
      auto reference = fixture.fields[field];
      if (fixture.schema.id == "photospider.path_set" && field == 5) {
        auto id = held.object_id();
        for (std::size_t offset = 24; offset < reference.size(); offset += 32)
          std::memcpy(reference.data() + offset, &id, 8);
      }
      check(std::memcmp(page->bytes().data(), reference.data() + row * width,
                        count * width) == 0,
            "independent wire reference mismatch");
      row += count;
    }
  }
  if (refine) {
    check(next.association().size() == 1 &&
              next.association()[0] == held.object_id(),
          "iteration predecessor association");
    const auto facts = take(next.descriptor());
    auto state = take(take(next.prepare_read(facts, 0, 0, 1)).load(window));
    std::array<std::int64_t, 3> actual{};
    std::memcpy(actual.data(), state->bytes().data(), 24);
    check(actual == std::array<std::int64_t, 3>{2, 1, 0},
          "new iteration generation/count/stop reason");
    auto estimate = take(take(next.prepare_read(facts, 1, 0, 2)).load(window));
    std::array<double, 2> values{};
    std::memcpy(values.data(), estimate->bytes().data(), 16);
    check(values == std::array<double, 2>{3, 4},
          "independent diagonal solution");
  }
  check(root.statistics().peak[ResourceKind::Host] <= root_capacity &&
            root.statistics().peak[ResourceKind::Metadata] <= root_capacity,
        "root capacity exceeded");
  auto parent = held.weak();
  held = {};
  check(!parent.lock().valid(),
        "materialized output association does not retain predecessor payload");
  if (refine) {
    const auto facts = take(next.descriptor());
    auto estimate = take(take(next.prepare_read(facts, 1, 0, 2)).load(window));
    std::array<double, 2> values{};
    std::memcpy(values.data(), estimate->bytes().data(), 16);
    check(values == std::array<double, 2>{3, 4} &&
              root.statistics().live[ResourceKind::Disk] > 0,
          "materialized child remains readable after predecessor retirement");
  }
  next = {};
  check(root.statistics().live[ResourceKind::Disk] == 0,
        "last result did not release backing");
  std::cout << fixture.name << " passed; backing=" << backing_name(backing)
            << " wire_bytes_read=" << reads->load() << " window=" << window
            << " root_host_peak=" << root.statistics().peak[ResourceKind::Host]
            << " root_metadata_peak="
            << root.statistics().peak[ResourceKind::Metadata] << '\n';
}
void fixtures() {
  SpectrumSpec spectrum;
  spectrum.original_shape = {3, 4};
  spectrum.transformed_axes = {0, 1};
  spectrum.axis_order = {0, 1};
  spectrum.shifts = {0, 0};
  spectrum.sample_origin = {0, 0};
  spectrum.sample_step = {1, 1};
  spectrum.atol = 0;
  spectrum.real_policy = SpectrumRealPolicy::ExactHermitian;
  Fixture frequency{"spectrum-nyquist-column",
                    take(spectrum_schema(spectrum)),
                    {}};
  std::vector<double> samples(18);
  samples[10] = 1;
  samples[11] = 2;
  samples[16] = 1;
  samples[17] = -2;
  frequency.set(0, samples);
  run(frequency, true, 128);
  run(frequency, true, 128, false, false, WireBacking::Reversed);
  run(frequency, true, 128, false, false, WireBacking::Split);
  samples[17] = 2;
  frequency.set(0, samples);
  run(frequency, false);
  auto wrong_packing = spectrum;
  wrong_packing.shifts[0] = 1;
  check(!spectrum_schema(wrong_packing).ok(),
        "packed shift must reject before source I/O");

  auto large_spectrum = spectrum;
  large_spectrum.original_shape = {3};
  large_spectrum.transformed_axes = {0};
  large_spectrum.axis_order = {0};
  large_spectrum.shifts = {0};
  large_spectrum.sample_origin = {0};
  large_spectrum.sample_step = {1};
  large_spectrum.packed_axis = 0;
  large_spectrum.packing = SpectrumPacking::Full;
  Fixture large{"spectrum-large-exact",
                take(spectrum_schema(large_spectrum)),
                {}};
  const auto maximum = std::numeric_limits<double>::max();
  large.set<double>(0, {0, 0, maximum, maximum, maximum, -maximum});
  run(large);

  auto singleton_spec = large_spectrum;
  singleton_spec.original_shape = {1};
  singleton_spec.packing = SpectrumPacking::R2CHalf;
  Fixture singleton{"spectrum-singleton",
                    take(spectrum_schema(singleton_spec)),
                    {}};
  singleton.set<double>(0, {7, 0});
  run(singleton);

  auto measured_spec = large_spectrum;
  measured_spec.real_policy = SpectrumRealPolicy::RealProjectionMeasured;
  measured_spec.atol = 0x1p-102;
  measured_spec.rtol = 0;
  Fixture measured{"spectrum-measured-underflow",
                   take(spectrum_schema(measured_spec)),
                   {}};
  measured.set<double>(0, {0, 0, 0x1p1000, 0x1p-100, 0x1p1000, 0x1p-100});
  run(measured, false);

  measured_spec.atol = 0;
  measured_spec.rtol = std::numeric_limits<double>::denorm_min();
  Fixture relative{"spectrum-relative-subnormal",
                   take(spectrum_schema(measured_spec)),
                   {}};
  relative.set<double>(0, {0, 0, 0x1p1000, 0x1.2p-75, 0x1p1000, 0x1.2p-75});
  run(relative, false);

  BandsSpec bands;
  bands.original_shape = {5};
  bands.axes = {0};
  bands.levels = 1;
  bands.bands = {{1, 0, {3}, {5}, {0}, {2}, {0}, false},
                 {1, 1, {3}, {5}, {0}, {2}, {0}, false}};
  Fixture wavelet{"haar-odd-membership", take(bands_schema(bands)), {}};
  wavelet.set<double>(0, {2, 6, 9, -1, -1, 0});
  run(wavelet);
  const std::array<double, 5> reference{1, 3, 5, 7, 9};
  for (unsigned i = 0; i < 5; ++i) {
    const double low[] = {2, 6, 9}, high[] = {-1, -1, 0};
    check(low[i / 2] + (i % 2 ? -high[i / 2] : high[i / 2]) == reference[i],
          "independent odd Haar crop reference");
  }
  auto missing = bands;
  missing.bands.pop_back();
  check(!bands_schema(missing).ok(), "missing band cannot imply zero");
  bands.bands[1].explicit_zero = true;
  Fixture zero_band{"haar-explicit-zero", take(bands_schema(bands)), {}};
  zero_band.set<double>(0, {2, 6, 9});
  run(zero_band);

  Fixture path{"path-cubic-closed", take(path_set_schema({})), {}};
  path.set<std::uint8_t>(0, {1, 4, 2, 5});
  path.set<std::int64_t>(1, {0, 1, 4, 5, 5});
  path.set<double>(2, {0, 0, 1, 2, 2, 2, 3, 0, 4, 0});
  path.set<std::int64_t>(3, {0, 4});
  path.set<std::uint8_t>(4, {1});
  path.set<std::int64_t>(12, {3, 1, 1, 4, 0, 0});
  path.set<double>(13, {1, 2, 3, 4});
  run(path, true, 128);
  path.set<std::int64_t>(1, {0, 1, 3, 5, 5});
  run(path, false);
  Fixture empty_path{"path-empty", take(path_set_schema({})), {}};
  empty_path.set<std::int64_t>(1, {0});
  empty_path.set<std::int64_t>(3, {0});
  run(empty_path);
  PathSetSpec primitive_spec;
  primitive_spec.authority = PathAuthority::Primitives;
  Fixture arc{"path-affine-full-turn",
              take(path_set_schema(primitive_spec)),
              {}};
  arc.set<std::int64_t>(1, {0});
  arc.set<std::int64_t>(3, {0, 1});
  arc.set<std::uint8_t>(4, {1});
  arc.set<std::int64_t>(5, {5, 0, 7, 0});
  arc.set<double>(7, {0, 0, 2, 1, 1, 3, 0, 1});
  run(arc);
  Fixture spline{"path-rational-spline",
                 take(path_set_schema(primitive_spec)),
                 {}};
  spline.set<std::int64_t>(1, {0});
  spline.set<double>(2, {0, 0, 1, 2, 3, 0});
  spline.set<std::int64_t>(3, {0, 1});
  spline.set<std::uint8_t>(4, {0});
  spline.set<std::int64_t>(5, {7, 0, 9, 0});
  spline.set<std::int64_t>(9, {2, 0, 3, 0, 0});
  spline.set<double>(10, {0, 0, 0, 1, 1, 1});
  spline.set<double>(11, {1, 2, 1});
  run(spline);
  spline.set<double>(11, {1, -2, 1});
  run(spline, false);

  PointSetSpec point_spec;
  point_spec.basis_count = 8192;
  for (std::uint64_t count : {0U, 257U}) {
    Fixture points{count ? "points-reordered" : "points-empty",
                   take(point_set_schema(point_spec)),
                   {}};
    std::vector<std::int64_t> ids, mapping;
    std::vector<double> positions, attributes;
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto id = 3 * (count - 1 - i) + 1;
      ids.push_back(id);
      positions.push_back(id);
      positions.push_back(-static_cast<double>(id));
      attributes.push_back(id * 0.5);
      mapping.push_back(3 * i + 1);
      mapping.push_back(count - 1 - i);
    }
    points.set(0, ids);
    points.set(1, positions);
    points.set(2, attributes);
    points.set(3, mapping);
    run(points, true, 128);
    if (!count)
      run(points, true, 128, false, false, WireBacking::BroadcastZero);
    if (count) {
      auto limited = points;
      auto limited_spec = point_spec;
      limited_spec.maximum_count = count - 1;
      limited.schema = take(point_set_schema(limited_spec));
      limited.name = "points-semantic-limit";
      run(limited, false, 128, false, true);
      mapping[1] = 0;
      points.set(3, mapping);
      run(points, false);
    }
  }
  Fixture components{"components-min-pixel",
                     take(components_schema({2, 5, 100})),
                     {}};
  components.set<std::int64_t>(0, {1, 1, 0, 4, 0, 0, 1, 0, 4, 4});
  components.set<std::int64_t>(1, {1, 3, 0, 4, 3, 3});
  run(components);
  auto limited_components = components;
  limited_components.schema = take(components_schema({2, 5, 1}));
  limited_components.name = "components-semantic-limit";
  run(limited_components, false, 256, false, true);
  components.set<std::int64_t>(1, {1, 3, 0, 4, 2, 3});
  run(components, false);
  Fixture empty_components{"components-empty",
                           take(components_schema({1, 1, 0})),
                           {}};
  empty_components.set<std::int64_t>(0, {0});
  run(empty_components);

  Fixture planes{"ycbcr420-odd-support", take(ycbcr420_schema({3, 5})), {}};
  planes.set(0, std::vector<float>(15, 0.5f));
  planes.set(1, std::vector<float>(6, 0));
  planes.set(2, std::vector<float>(6, 0));
  run(planes);
  check(planes.schema.fields[1].rows.value == 6, "3x5 must have 2x3 chroma");
  // Last nominal center (2.5,4.5) corresponds to the clipped source singleton
  // (2,4). This arithmetic reference never treats that nominal center as
  // support.
  check(std::min<std::uint64_t>(2, 3 - 2) * std::min<std::uint64_t>(2, 5 - 4) ==
            1,
        "odd chroma actual support");

  BrushSpec brush;
  const BrushEvent events[] = {{0, 0}, {1, 0.5}, {2, 2.25}, {3, 4}};
  ResourceBudget budget;
  auto whole = take(advance_causal_brush(brush, {}, events, 4, true, budget));
  auto a = take(advance_causal_brush(brush, {}, events, 2, false, budget));
  auto b =
      take(advance_causal_brush(brush, a.state, events + 2, 2, true, budget));
  std::vector<double> joined(a.dabs.begin(), a.dabs.end());
  joined.insert(joined.end(), b.dabs.begin(), b.dabs.end());
  check(joined == std::vector<double>({0, 1, 2, 3, 4}) &&
            std::equal(joined.begin(), joined.end(), whole.dabs.begin(),
                       whole.dabs.end()),
        "independent spacing and batch partition reference");
  Fixture stroke{"brush-generation", take(brush_schema(brush)), {}};
  stroke.set<std::int64_t>(0, {9, 4, 4, 5, 1, 5, 2, 7, 5, 1});
  stroke.set<double>(1, {4, 1, 0.25, 0.5, -0.125, 0.5, 1, 0, 2});
  stroke.set(4, joined);
  run(stroke);
  auto reversed = stroke;
  reversed.name = "brush-reversed-dabs";
  reversed.set<double>(4, {4, 3, 2, 1, 0});
  run(reversed, false);
  brush.lookahead = 2;
  Fixture pending{"brush-pending-prefix", take(brush_schema(brush)), {}};
  pending.set<std::int64_t>(0, {9, 4, 2, 1, 1, 2, 1, 7, 1, 0});
  pending.set<double>(1, {0.5, 0.5, 0, 0, 0, 0, 0, 0, 0});
  pending.set<std::int64_t>(2, {2, 3});
  pending.set<double>(3, {2.25, 4});
  pending.set<double>(4, {0});
  run(pending);
  pending.set<std::int64_t>(0, {9, 4, 2, 2, 1, 2, 1, 7, 2, 1});
  run(pending, false);

  pending.set<std::int64_t>(0, {9, INT64_MAX, INT64_MIN, 0, 0, 0, 1, 0, 0, 0});
  pending.set<std::int64_t>(2, {});
  pending.set<double>(3, {});
  pending.set<double>(4, {});
  run(pending, false);

  IterativeSpec iteration;
  iteration.size = 2;
  iteration.maximum_iterations = 2;
  iteration.system_snapshot = "diagonal-fixture-v1";
  Fixture converged{"iteration-converged",
                    take(iterative_schema(iteration)),
                    {}};
  converged.set<std::int64_t>(0, {1, 1, 0});
  converged.set<double>(1, {3, 4});
  converged.set<double>(2, {1, 2});
  converged.set<double>(3, {3, 8});
  converged.set<double>(4, {0});
  run(converged);
  IterativeSpec zero_spec;
  zero_spec.size = 1;
  zero_spec.maximum_iterations = 1;
  zero_spec.target_residual = 0;
  zero_spec.system_snapshot = "diagonal-one";
  Fixture zero_initial{"iteration-zero-initialization",
                       take(iterative_schema(zero_spec)),
                       {}};
  zero_initial.set<std::int64_t>(0, {1, 0, 0});
  zero_initial.set<double>(1, {1});
  zero_initial.set<double>(2, {1});
  zero_initial.set<double>(3, {1});
  zero_initial.set<double>(4, {0});
  run(zero_initial, false);
  iteration.require_converged = false;
  Fixture approximate{"iteration-measured-approximate",
                      take(iterative_schema(iteration)),
                      {}};
  approximate.set<std::int64_t>(0, {2, 2, 1});
  approximate.set<double>(1, {0, 0});
  approximate.set<double>(2, {1, 2});
  approximate.set<double>(3, {3, 8});
  approximate.set<double>(4, {8});
  run(approximate);
  auto initial_spec = iteration;
  initial_spec.maximum_iterations = 0;
  Fixture seed{"iteration-resume-generation",
               take(iterative_schema(initial_spec)),
               {}};
  seed.set<std::int64_t>(0, {1, 0, 1});
  seed.set<double>(1, {0, 0});
  seed.set<double>(2, {1, 2});
  seed.set<double>(3, {3, 8});
  seed.set<double>(4, {8});
  run(seed, true, 256, true);
  approximate.set<double>(4, {0});
  run(approximate, false);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc > 3)
      throw std::runtime_error(
          "usage: representations_workflow [fixture [root_bytes]]");
    if (argc >= 2)
      selected_fixture = argv[1];
    if (argc == 3) {
      const std::string capacity(argv[2]);
      if (capacity.empty() ||
          !std::all_of(capacity.begin(), capacity.end(),
                       [](char byte) { return byte >= '0' && byte <= '9'; }))
        throw std::runtime_error(
            "root_bytes must be an unsigned decimal integer");
      root_capacity = std::stoull(capacity);
    }
    fixtures();
    if (!executed_fixtures)
      throw std::runtime_error("unknown representation fixture");
    std::cout << "representation workflows PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
