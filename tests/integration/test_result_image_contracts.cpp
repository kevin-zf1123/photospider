#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/icc_fixture.hpp"
#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
void zero(const ResourceBudget& root) {
  for (auto count : root.statistics().live.values)
    require(!count, "last owner capacity");
}
ResultRef image(const ResourceBudget& root, const SchemaTemplate& schema,
                float bias = 0, ResourceBindings resources = {},
                uint32_t biased_slot = UINT32_MAX) {
  auto made = ResultBuilder::start(root, schema, "source", {}, {}, 1, 1,
                                   std::move(resources));
  require(made.ok(), made.status().message);
  auto builder = made.take_value();
  require(builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "image basis");
  for (uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
    auto count = schema.tensors[slot].sample_count().value();
    std::vector<float> samples(count);
    for (uint64_t i = 0; i < count; ++i)
      samples[i] =
          (biased_slot == UINT32_MAX || slot == biased_slot ? bias : 0) + i;
    auto status = builder.publish_tensor(
        slot, Region::whole(schema.tensors[slot].sample_shape()),
        ByteView(reinterpret_cast<uint8_t*>(samples.data()),
                 samples.size() * 4),
        ResultRelation::cartesian(root, count, {0, 1, 0, 0}).take_value(),
        {true, true, true, true});
    require(status.ok(), status.message);
  }
  return builder.seal().take_value();
}
void captured_prefix() {
  ResourceBudget root;
  {
    SchemaTemplate schema;
    schema.id = "test.prefix.capture";
    schema.publication = PublishPolicy::StablePrefix;
    schema.fields = {
        {"rows", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}}};
    auto builder = ResultBuilder::start(root, schema, "scope").take_value();
    require(
        builder
            .bind_descriptor_relation(
                ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
            .ok(),
        "basis");
    const int64_t first = 1, second = 2;
    auto witness =
        ResultRelation::cartesian(root, 2, {0, 1, 0, 0}).take_value();
    require(
        builder
            .append(0, 1, ByteView(reinterpret_cast<const uint8_t*>(&first), 8))
            .ok(),
        "append first");
    require(builder.publish(0, 1, witness, {true, true, true, true}).ok(),
            "publish first");
    auto old = builder.reference().capture().take_value();
    auto weak = old.weak();
    auto facts = old.descriptor(false).take_value();
    require(builder
                .append(0, 1,
                        ByteView(reinterpret_cast<const uint8_t*>(&second), 8))
                .ok(),
            "append second");
    require(builder.publish(0, 2, witness, {true, true, true, true}).ok(),
            "publish second");
    auto complete = builder.seal().take_value();
    auto locked = weak.lock();
    require(locked.valid() && locked.descriptor(false).value().rows(0) == 1,
            "weak capture revision");
    require(!locked.descriptor().ok() && !locked.production_status().ok(),
            "captured prefix stays incomplete");
    require(!locked.prepare_read(complete.descriptor().value(), 0, 1, 1).ok(),
            "captured view refuses future descriptor");
    auto plan = locked.prepare_read(facts, 0, 0, 1).take_value();
    auto storage = plan.load(8).take_value();
    int64_t value = 0;
    std::memcpy(&value, storage->bytes().data(), 8);
    require(value == 1, "old prefix remains readable");
  }
  zero(root);
}
void resource_admission() {
  std::vector<Region> boxes;
  for (uint64_t i = 0; i < 128; ++i)
    boxes.emplace_back(std::vector<RegionDimension>{{i, 1}});
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 512;
  limits.capacity[ResourceKind::Metadata] = 512;
  ResourceBudget small(limits);
  {
    ErrorCode failure = ErrorCode::Ok;
    ResourceAllocationScope scope(small, &failure);
    auto footprint = Footprint::from_regions({128}, boxes);
    require(!footprint.ok() &&
                footprint.status().code == ErrorCode::ResourceExhausted,
            "transient boxes admission");
    require(failure == ErrorCode::ResourceExhausted,
            "sticky metadata admission");
  }
  zero(small);
  ResourceBudget roomy;
  {
    ResourceAllocationScope scope(roomy);
    auto footprint = Footprint::from_regions({128}, boxes).take_value();
    require(footprint.boxes().size() == 1, "adjacent boxes normalize");
    require(roomy.statistics().peak[ResourceKind::Metadata] >
                roomy.statistics().live[ResourceKind::Metadata],
            "transient peak accounted");
  }
  zero(roomy);
  auto schema = unified_example::image_schema();
  schema.tensors[0].facets = {
      {"test.large", 1, std::vector<uint8_t>(65536, 7)}};
  limits.capacity[ResourceKind::Host] = 16384;
  limits.capacity[ResourceKind::Metadata] = 16384;
  ResourceBudget low(limits);
  auto refused = ResultBuilder::start(low, schema, "large");
  require(refused.status().code == ErrorCode::ResourceExhausted,
          "large typed image metadata refused");
  zero(low);
  TensorDescription tensor;
  tensor.channel_axis = 0;
  tensor.channels.resize(64);
  auto facet = encode_tensor_description(tensor).take_value();
  {
    ResourceAllocationScope scope(low);
    auto decoded = decode_tensor_description(facet);
    require(decoded.status().code == ErrorCode::ResourceExhausted,
            "decoder expansion admitted before allocation");
  }
  zero(low);
  ValueFacet truncated{
      "photospider.tensor-description",
      4,
      {'T', 'D', 'M', '4', 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0}};
  {
    ResourceAllocationScope scope(roomy);
    require(!decode_tensor_description(truncated).ok(),
            "truncated group table rejected");
  }
  zero(roomy);
  // Two new frame pages cannot be partially committed when the second fails.
  limits = ResourceLimits{};
  limits.capacity[ResourceKind::Payload] = 32768;
  ResourceBudget pages(limits);
  {
    schema = unified_example::image_schema();
    schema.tensors[0].batch_axes[0] = 3;
    schema.tensors[0].batch_axes[1] = 1;
    schema.tensors[0].descriptor.shape = {1, 1};
    schema.publication = PublishPolicy::StablePrefix;
    auto builder =
        ResultBuilder::start(pages, schema, "transaction", {}, {}, 1, 1)
            .take_value();
    require(
        builder
            .bind_descriptor_relation(
                ResultRelation::cartesian(pages, 1, {0, 8, 0, 0}).take_value())
            .ok(),
        "transaction basis");
    auto relation =
        ResultRelation::cartesian(pages, 3, {0, 1, 0, 0}).take_value();
    float value = 4;
    require(builder
                .publish_tensor(0, Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}}),
                                ByteView(reinterpret_cast<uint8_t*>(&value), 4),
                                relation, {true, true, true, true})
                .ok(),
            "old frame");
    auto old = builder.reference().capture().take_value();
    const auto baseline = pages.statistics().live[ResourceKind::Payload];
    ResourceCapacity occupied;
    occupied[ResourceKind::Payload] = 32768 - 2 * baseline;
    auto blocker = pages.reserve(occupied).take_value();
    float next[] = {5, 6};
    auto failed =
        builder.publish_tensor(0, Region({{1, 2}, {0, 1}, {0, 1}, {0, 1}}),
                               ByteView(reinterpret_cast<uint8_t*>(next), 8),
                               relation, {true, true, true, true});
    require(failed.code == ErrorCode::ResourceExhausted,
            "second frame preparation refused");
    require(pages.statistics().live[ResourceKind::Payload] == 32768 - baseline,
            "uncommitted frame pages retired");
    require(builder.reference().descriptor(false).value().revision() ==
                old.descriptor(false).value().revision(),
            "failed transaction does not advance revision");
    float read = 0;
    require(old.read_tensor(old.descriptor(false).value(), 0, {0, 0, 0, 0},
                            &read, 4)
                    .ok() &&
                read == 4,
            "old captured frame survives failure");
  }
  zero(pages);
}
struct Join {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto samples =
        phase.query.tensor_outputs
            ? Result<Footprint>(*phase.query.tensor_outputs)
            : Footprint::all(
                  phase.query.output.result_schema->tensors[0].sample_shape());
    if (!samples.ok())
      return Result<ResultProgramPoll>(samples.status());
    const bool temporal = phase.query.parameters.count("temporal") &&
                          std::get<bool>(phase.query.parameters.at("temporal"));
    auto upstream = samples;
    if (temporal) {
      auto admitted = phase.resources.reserve(ResourceCapacity::host(
          samples.value().boxes().size() *
              (sizeof(Region) + 5 * sizeof(RegionDimension)),
          samples.value().boxes().size() *
              (sizeof(Region) + 5 * sizeof(RegionDimension))));
      if (!admitted.ok())
        return Result<ResultProgramPoll>(admitted.status());
      std::vector<Region> boxes;
      boxes.reserve(samples.value().boxes().size());
      for (const auto& box : samples.value().boxes()) {
        auto dimensions = box.dimensions();
        if (dimensions[0].extent != samples.value().shape()[0])
          dimensions[0].offset =
              (dimensions[0].offset + 1) % samples.value().shape()[0];
        boxes.emplace_back(std::move(dimensions));
      }
      upstream = Footprint::from_regions(samples.value().shape(), boxes);
      if (!upstream.ok())
        return Result<ResultProgramPoll>(upstream.status());
    }
    if (!stage++) {
      ResultProgramNeed need;
      need.tensors = {{0, 0, upstream.value(), 1}, {1, 0, samples.value(), 1}};
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, {}, phase.query.tile_height,
        phase.query.tile_width, phase.query.resources);
    if (!builder.ok())
      return Result<ResultProgramPoll>(builder.status());
    auto output = builder.take_value();
    auto basis = ResultRelation::cartesian(phase.resources, 1, {0, 8, 0, 0});
    if (!basis.ok())
      return Result<ResultProgramPoll>(basis.status());
    auto status = output.bind_descriptor_relation(basis.take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    for (const auto& box : samples.value().boxes()) {
      auto count = box.element_count().value();
      auto bytes = phase.allocator.allocate(count * 4);
      if (!bytes.ok())
        return Result<ResultProgramPoll>(bytes.status());
      auto buffer = bytes.take_value();
      ResourceVector<ResultRelationRow> rows;
      uint64_t position = 0;
      auto selected = Footprint::from_regions(samples.value().shape(), {box});
      if (!selected.ok())
        return Result<ResultProgramPoll>(selected.status());
      status = selected.value().visit(
          [&](const auto& at) {
            float first = 0, second = 0;
            auto source = at;
            if (temporal)
              source[0] = (source[0] + 1) % samples.value().shape()[0];
            auto read = phase.read_tensor(0, 0, source, &first, 4);
            if (!read.ok())
              return read;
            read = phase.read_tensor(1, 0, at, &second, 4);
            if (!read.ok())
              return read;
            const float sum = first + second;
            std::memcpy(buffer.data() + 4 * position++, &sum, 4);
            auto index = unified_example::flat(at, samples.value().shape());
            rows.push_back(
                {index,
                 {0, 1, unified_example::flat(source, samples.value().shape()),
                  1, ResultSupportTarget::Tensor, 0}});
            rows.push_back(
                {index, {1, 1, index, 1, ResultSupportTarget::Tensor, 0}});
            return Status::success();
          },
          count);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      auto relation = ResultRelation::sample_rows(
          phase.resources,
          phase.query.output.result_schema->tensors[0].sample_count().value(),
          rows.size(),
          [&](auto i) { return Result<ResultRelationRow>(rows[i]); });
      if (!relation.ok())
        return Result<ResultProgramPoll>(relation.status());
      status = output.publish_tensor(
          0, box, ByteView(buffer.data(), buffer.size()), relation.take_value(),
          {true, true, true, true});
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto sealed = output.seal();
    return sealed.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{sealed.take_value(), true})
                       : Result<ResultProgramPoll>(sealed.status());
  }
};
Status join(OperationRegistry* registry, const SchemaTemplate& schema) {
  OperationDefinition op;
  op.key = "test.image.join";
  op.traits = unified_example::traits(2, sizeof(Join));
  op.traits.parameter_schema = {
      {"temporal", OperationParameterType::Bool, false, false, 0, 0}};
  unified_example::result_output(&op.traits.outputs[0], schema);
  for (auto& port : op.traits.input_schema)
    unified_example::result_port(&port, schema);
  op.start_result = [](const ResultProgramQuery&,
                       const BufferAllocator& allocator) {
    return ResultContinuation::make<Join>(allocator);
  };
  return registry->register_operation(std::move(op));
}
void alias_diamond() {
  auto schema = unified_example::image_schema();
  auto registry = std::make_shared<OperationRegistry>();
  require(join(registry.get(), schema).ok(), "join registration");
  require(registry->freeze().ok(), "freeze joins");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ExecutionBinding binding;
  binding.name = "source";
  binding.result = image(root, schema);
  WorkflowDocument doc;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  doc.inputs = {declaration};
  doc.nodes = {
      {1,
       "test.image.join",
       {WorkflowInputReference{1}, WorkflowInputReference{1}},
       {}},
      {2,
       "test.image.join",
       {WorkflowInputReference{1}, WorkflowInputReference{1}},
       {}},
      {3,
       "test.image.join",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
       {}},
      {4,
       "test.image.join",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}}};
  doc.outputs = {{"left", 3, "value"}, {"right", 4, "value"}};
  GraphContext graph(doc);
  PlanningOptions geometry;
  geometry.tile_height = geometry.tile_width = 2;
  auto plan = Compiler(registry).compile(graph, geometry).take_value().plan;
  auto frozen = context.freeze(plan, {{binding}}).take_value();
  auto q = Footprint::from_regions({2, 2, 2, 4},
                                   {Region({{1, 1}, {1, 1}, {1, 1}, {2, 1}})})
               .take_value();
  for (unsigned iteration = 0; iteration < 2; ++iteration) {
    auto result =
        context.execute_fragments(frozen, {{"left", q}, {"right", q}});
    require(result.ok(), result.status().message);
    {
      const auto available = root.available_capacity();
      const auto bytes = std::min(available[ResourceKind::Host],
                                  available[ResourceKind::Metadata]) -
                         ResourceBudget::lease_metadata_bytes();
      auto blocker =
          root.reserve(ResourceCapacity::host(bytes, bytes)).take_value();
      auto denied = result.value().dependencies.source_support();
      require(
          !denied.ok() && denied.status().code == ErrorCode::ResourceExhausted,
          "returned source map admission failure is a typed result");
    }
    const auto before_maps = root.statistics().live[ResourceKind::Metadata];
    {
      auto coverage = result.value().dependencies.coverage();
      auto guarantees = result.value().dependencies.guarantees();
      require(root.statistics().live[ResourceKind::Metadata] > before_maps,
              "returned evidence maps own root capacity");
    }
    require(root.statistics().live[ResourceKind::Metadata] == before_maps,
            "returned map capacity retires");
    auto observations = result.value().dependencies.source_observations();
    require(observations.ok() && !observations.value().empty(),
            "cold/warm diamond ancestry");
    auto dirty = result.value().dependencies.potential_dirty("source", q);
    require(dirty.ok() && dirty.value().at("left") == q &&
                dirty.value().at("right") == q,
            "rebound alias dirty");
    for (const auto& key : {"left", "right"}) {
      auto owner = result.value().results.at(key);
      float read = 0;
      require(owner.read_tensor(owner.descriptor().value(), 0, {1, 1, 1, 2},
                                &read, 4)
                      .ok() &&
                  read == 120,
              "nonzero origin/frame/layer diamond result");
    }
  }

  doc.nodes.resize(1);
  doc.nodes[0].parameters = {{"temporal", true}};
  doc.outputs = {{"out", 1, "value"}};
  GraphContext temporal_graph(doc);
  auto temporal_plan =
      Compiler(registry).compile(temporal_graph, geometry).take_value().plan;
  auto near = Footprint::from_regions(
                  {2, 2, 2, 4}, {Region({{0, 1}, {0, 1}, {1, 1}, {2, 1}})})
                  .take_value();
  auto next = Footprint::from_regions(
                  {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}})})
                  .take_value();
  auto result = context.execute_fragments(
      context.freeze(temporal_plan, {{binding}}).take_value(), {{"out", near}});
  require(result.ok(), result.status().message);
  auto dirty = result.value().dependencies.potential_dirty("source", next);
  require(dirty.ok() && dirty.value().at("out") == near,
          "cross-frame support transpose");
  auto unrelated = Footprint::from_regions(
                       {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {3, 1}})})
                       .take_value();
  dirty = result.value().dependencies.potential_dirty("source", unrelated);
  require(dirty.ok() && dirty.value().at("out").empty(),
          "unrelated sample in same tile stays clean");
}
SchemaTemplate chunk_schema(uint64_t count, ElementType type, const char* id) {
  SchemaTemplate schema;
  schema.id = id;
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {type, {count}};
  schema.tensors.push_back(std::move(member));
  return schema;
}
struct Probe {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto shape =
        phase.query.inputs[0].result_schema->tensors[1].sample_shape();
    const std::vector<uint64_t> at{0, 1, 0, 2};
    if (!stage++) {
      ResultProgramNeed need;
      need.results = {{0, 0, true, 0}};
      if (!phase.query.tensor_outputs->empty())
        need.tensors = {{0, 1,
                         Footprint::from_regions(
                             shape, {Region({{0, 1}, {1, 1}, {0, 1}, {2, 1}})})
                             .take_value(),
                         1}};
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto guarantee = static_cast<DependencyGuarantee>(
        std::get<int64_t>(phase.query.parameters.at("guarantee")));
    auto basis =
        guarantee == DependencyGuarantee::Unknown
            ? ResultRelation::unknown(phase.resources, 1)
            : ResultRelation::cartesian(
                  phase.resources, 1,
                  {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0}, guarantee);
    if (!basis.ok())
      return Result<ResultProgramPoll>(basis.status());
    Result<ResultRelation> relation =
        ResultRelation::cartesian(phase.resources, 1, {0, 1, 0, 0});
    float value = 0;
    if (!phase.query.tensor_outputs->empty()) {
      if (std::get<bool>(phase.query.parameters.at("bad"))) {
        float ignored = 0;
        static_cast<void>(
            phase.tensors->at({0, 1}).read({0, 1, 0, 0}, &ignored, 4));
      }
      auto read = phase.read_tensor(0, 1, at, &value, 4);
      if (!read.ok())
        return Result<ResultProgramPoll>(read);
      relation = ResultRelation::cartesian(
          phase.resources, 1, {0, 1, 5, 1, ResultSupportTarget::Tensor, 1});
    }
    if (!relation.ok())
      return Result<ResultProgramPoll>(relation.status());
    auto started =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!started.ok())
      return Result<ResultProgramPoll>(started.status());
    auto builder = started.take_value();
    auto status = builder.bind_descriptor_relation(basis.take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    for (const auto& box : phase.query.tensor_outputs->boxes()) {
      status = builder.publish_tensor(
          0, box, ByteView(reinterpret_cast<const uint8_t*>(&value), 4),
          relation.value(), {true, true, true, true});
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{sealed.take_value(), true})
                       : Result<ResultProgramPoll>(sealed.status());
  }
};
struct Escaped {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    float zero = 0;
    auto known =
        ResultRelation::cartesian(phase.resources, 1,
                                  {0, 1, 5, 1, ResultSupportTarget::Tensor, 1})
            .take_value();
    auto unresolved = ResultRelation::unknown(phase.resources, 1).take_value();
    auto relation = ResultRelation::unite(phase.resources, {known, unresolved})
                        .take_value();
    auto basis = ResultRelation::cartesian(phase.resources, 1, {0, 8, 0, 0})
                     .take_value();
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto status = builder.bind_descriptor_relation(std::move(basis));
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    status = builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const uint8_t*>(&zero), 4),
        std::move(relation), {true, true, true, true});
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{builder.seal().take_value(), true});
  }
};
void slots_and_guarantees() {
  auto schema = unified_example::image_schema();
  auto depth = schema.tensors[0];
  depth.key = "depth";
  depth.batch_axes[0] = 1;
  depth.batch_axes[1] = 2;
  depth.descriptor.shape = {1, 3};
  schema.tensors.push_back(depth);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "test.image.probe";
  op.traits = unified_example::traits(1, sizeof(Probe));
  unified_example::result_port(&op.traits.input_schema[0], schema);
  unified_example::result_output(
      &op.traits.outputs[0],
      chunk_schema(1, ElementType::Float32, "test.probe.result"));
  op.traits.parameter_schema = {
      {"guarantee", OperationParameterType::Int64, true, true, 1, 3},
      {"bad", OperationParameterType::Bool, true, false, 0, 0}};
  op.start_result = [](const ResultProgramQuery&,
                       const BufferAllocator& allocator) {
    return ResultContinuation::make<Probe>(allocator);
  };
  require(registry->register_operation(std::move(op)).ok(),
          "probe registration");
  OperationDefinition escaped;
  escaped.key = "test.image.escaped";
  escaped.traits = unified_example::traits(1, sizeof(Escaped));
  unified_example::result_port(&escaped.traits.input_schema[0], schema);
  unified_example::result_output(
      &escaped.traits.outputs[0],
      chunk_schema(1, ElementType::Float32, "test.probe.result"));
  escaped.traits.outputs[0].input_indices = std::vector<uint32_t>{};
  escaped.start_result = [](const ResultProgramQuery&,
                            const BufferAllocator& allocator) {
    return ResultContinuation::make<Escaped>(allocator);
  };
  require(registry->register_operation(std::move(escaped)).ok(),
          "escaped registration");
  require(registry->freeze().ok(), "probe freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = context.resource_budget().take_value();
  ExecutionBinding binding;
  binding.name = "source";
  binding.result = image(root, schema);
  WorkflowDocument doc;
  WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "source";
  declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
  doc.inputs = {declaration};
  {
    auto escaped = doc;
    escaped.nodes = {
        {1, "test.image.escaped", {WorkflowInputReference{1}}, {}}};
    escaped.outputs = {{"out", 1, "value"}};
    GraphContext graph(escaped);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto refused = context.execute(plan, {{binding}});
    require(
        !refused.ok() && refused.status().code == ErrorCode::InvalidArgument,
        "Unknown union declared spans obey projection");
  }
  for (int64_t g = 1; g <= 3; ++g) {
    doc.nodes.push_back({static_cast<uint64_t>(g),
                         "test.image.probe",
                         {WorkflowInputReference{1}},
                         {{"guarantee", g}, {"bad", false}}});
    doc.outputs.push_back(
        {"g" + std::to_string(g), static_cast<uint64_t>(g), "value"});
  }
  GraphContext graph(doc);
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto frozen = context.freeze(plan, {{binding}}).take_value();
  auto q = Footprint::all({1}).take_value();
  auto result =
      context.execute_fragments(frozen, {{"g1", q}, {"g2", q}, {"g3", q}});
  require(result.ok(), result.status().message);
  auto guarantees = result.value().dependencies.guarantees();
  require(guarantees.at("g1") == DependencyGuarantee::Exact &&
              guarantees.at("g2") == DependencyGuarantee::Conservative &&
              guarantees.at("g3") == DependencyGuarantee::Unknown,
          "independent root guarantees");
  auto unknown = result.value().dependencies.potential_dirty(
      "source", Footprint::all(depth.sample_shape()).value(), 1, {},
      ResultSupportTarget::Tensor, 1);
  require(!unknown.ok() && unknown.status().code == ErrorCode::NotFound,
          "Unknown descriptor remains unresolved");
  auto empty =
      context.execute_fragments(frozen, {{"g2", Footprint::none({1}).value()}});
  require(empty.ok(), empty.status().message);
  require(empty.value().dependencies.guarantees().at("g2") ==
              DependencyGuarantee::Conservative,
          "Empty retains descriptor guarantee");
  auto obligations = empty.value().dependencies.source_observations();
  require(obligations.ok() && !obligations.value().empty() &&
              obligations.value()[0].target == ResultSupportTarget::Descriptor,
          "Empty retains descriptor ancestry");
  auto handle = context.open_demand(plan, {{binding}}).take_value();
  require(handle.request({{"g1", q}}).ok(), "slot demand");
  binding.result = image(root, schema, 50, {}, 0);
  auto update = handle.replace_bindings({{binding}}, {2, {}});
  require(update.ok(), update.status().message);
  require(update.value().potential_dirty.at("g1").empty(),
          "unrelated slot is not read or dirty");
  binding.result = image(root, schema, 100, {}, 1);
  update = handle.replace_bindings({{binding}}, {2, {}});
  require(update.ok(), update.status().message);
  require(update.value().potential_dirty.at("g1") == q, "slot 1 data dirty");
  auto changed = handle.request({{"g1", q}});
  require(changed.ok(), changed.status().message);
  float value = 0;
  require(
      changed.value()
              .results.at("g1")
              .read_tensor(
                  changed.value().results.at("g1").descriptor().take_value(), 0,
                  {0}, &value, 4)
              .ok() &&
          value == 105,
      "slot 1 replacement value");
  for (auto target : {ResultSupportTarget::Value, ResultSupportTarget::Field})
    require(!changed.value()
                 .dependencies
                 .potential_dirty("source", Footprint::all({1}).value(), 1, {},
                                  target, 99)
                 .ok(),
            "invalid typed dirty target rejected");
  doc.outputs = {{"bad", 1, "value"}};
  doc.nodes[0].parameters["bad"] = true;
  GraphContext invalid(doc);
  auto bad_plan = Compiler(registry).compile(invalid).take_value().plan;
  auto bad = context.execute(bad_plan, {{binding}});
  require(!bad.ok() && bad.status().code == ErrorCode::InvalidArgument,
          "ignored direct capability failure is sticky");
}

struct ChunkObserver {
  std::vector<WeakResultRef> produced;
  uint64_t maximum_live = 0;
  bool keep_first = false;
  bool foreign_association = false;
  uint64_t fail_at = UINT64_MAX;
};
struct ChunkCopy {
  unsigned stage = 0;
  std::shared_ptr<ChunkObserver> observer;
  explicit ChunkCopy(std::shared_ptr<ChunkObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto& samples = *phase.query.tensor_outputs;
    if (!stage++) {
      ResultProgramNeed need;
      need.tensors = {{0, 0, samples, 1}};
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto region = samples.boxes()[0];
    const auto at = region.dimensions()[0].offset;
    if (at == observer->fail_at)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "deliberate chunk failure"});
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto basis = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0});
    if (!basis.ok())
      return Result<ResultProgramPoll>(basis.status());
    auto status = builder.bind_descriptor_relation(basis.take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    double value = 0;
    status = phase.read_tensor(0, 0, {at}, &value, 8);
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto relation = ResultRelation::mapped(
        phase.resources, samples.shape(), region, samples.shape(),
        {{0, 0, 1, 1}}, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0});
    if (!relation.ok())
      return Result<ResultProgramPoll>(relation.status());
    status = builder.publish_tensor(
        0, region, ByteView(reinterpret_cast<const uint8_t*>(&value), 8),
        relation.take_value(), {true, true, true, true});
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto result = builder.seal().take_value();
    observer->produced.push_back(result.weak());
    return Result<ResultProgramPoll>(
        ResultPublication{std::move(result), true});
  }
};
struct ChunkSum {
  uint64_t cursor = 0;
  double sum = 0;
  bool waiting = false;
  ResultTensorReadWindow retained;
  std::shared_ptr<ChunkObserver> observer;
  explicit ChunkSum(std::shared_ptr<ChunkObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto count =
        phase.query.inputs[0].result_schema->tensors[0].sample_count().value();
    if (waiting) {
      double value = 0;
      auto status = phase.read_tensor(0, 0, {cursor}, &value, 8);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      if (!cursor && observer->keep_first) {
        auto window = phase.tensors->at({0, 0}).acquire(Region({{0, 1}}));
        if (!window.ok())
          return Result<ResultProgramPoll>(window.status());
        retained = window.take_value();
      }
      if (retained.valid()) {
        auto first = retained.row_run({0});
        if (!first.ok())
          return Result<ResultProgramPoll>(first.status());
        double first_value = 0;
        std::memcpy(&first_value, first.value().data, 8);
        require(first_value == 1,
                "explicit window retains old chunk bytes across Need polls");
      }
      uint64_t live = 0;
      for (const auto& weak : observer->produced)
        if (weak.lock().valid())
          ++live;
      observer->maximum_live = std::max(observer->maximum_live, live);
      require(live <= (observer->keep_first ? 2U : 1U),
              "consumed history does not keep old chunk payload owners alive");
      sum += value;
      ++cursor;
      waiting = false;
    }
    if (cursor < count) {
      auto samples = Footprint::from_regions({count}, {Region({{cursor, 1}})});
      if (!samples.ok())
        return Result<ResultProgramPoll>(samples.status());
      ResultProgramNeed need;
      need.tensors = {{0, 0, samples.take_value(), 1}};
      waiting = true;
      return Result<ResultProgramPoll>(std::move(need));
    }
    require(
        phase.association && phase.association->size() == count,
        "all successful chunk object associations survive payload retirement");
    std::vector<uint64_t> association(phase.association->begin(),
                                      phase.association->end());
    if (observer->keep_first) {
      std::reverse(association.begin(), association.end());
      association.push_back(association.front());
    }
    if (observer->foreign_association)
      association.push_back(UINT64_MAX);
    auto builder = ResultBuilder::start(
                       phase.resources, *phase.query.output.result_schema,
                       phase.query.semantic_key, {}, std::move(association))
                       .take_value();
    auto basis = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0});
    if (!basis.ok())
      return Result<ResultProgramPoll>(basis.status());
    auto status = builder.bind_descriptor_relation(basis.take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto relation = ResultRelation::cartesian(
        phase.resources, 1, {0, 1, 0, count, ResultSupportTarget::Tensor, 0});
    if (!relation.ok())
      return Result<ResultProgramPoll>(relation.status());
    status = builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const uint8_t*>(&sum), 8),
        relation.take_value(), {true, true, true, true});
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{builder.seal().take_value(), true});
  }
};
void staged_input_retirement() {
  for (const auto mode : {0U, 1U, 2U, 3U}) {
    const uint64_t count = mode == 0 ? 1030 : 32;
    auto observer = std::make_shared<ChunkObserver>();
    observer->keep_first = mode == 1;
    observer->foreign_association = mode == 3;
    if (mode == 2)
      observer->fail_at = 17;
    auto schema =
        chunk_schema(count, ElementType::Float64, "test.chunk.samples");
    auto output_schema =
        chunk_schema(1, ElementType::Float64, "test.chunk.sum");
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition copy;
    copy.key = "test.chunk.copy";
    copy.traits = unified_example::traits(1, sizeof(ChunkCopy));
    unified_example::result_port(&copy.traits.input_schema[0], schema);
    unified_example::result_output(&copy.traits.outputs[0], schema);
    copy.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkCopy>(allocator, observer);
    };
    require(registry->register_operation(std::move(copy)).ok(),
            "chunk copy registration");
    OperationDefinition reduction;
    reduction.key = "test.chunk.sum";
    reduction.traits = unified_example::traits(1, sizeof(ChunkSum));
    reduction.traits.outputs[0].maximum_dependency_stages = 4096;
    unified_example::result_port(&reduction.traits.input_schema[0], schema);
    unified_example::result_output(&reduction.traits.outputs[0], output_schema);
    reduction.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkSum>(allocator, observer);
    };
    require(registry->register_operation(std::move(reduction)).ok() &&
                registry->freeze().ok(),
            "chunk sum registration and freeze");
    ExecutionContextConfig config;
    config.maximum_live_bytes = 4096;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Payload] = 4096;
    config.managed_resources->capacity[ResourceKind::Metadata] = 64 * 1048576;
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    {
      auto buffer = root.allocator().allocate(8).take_value();
      const double one = 1;
      std::memcpy(buffer.data(), &one, 8);
      auto builder =
          ResultBuilder::start(root, schema, "chunk.input").take_value();
      require(
          builder
              .bind_descriptor_relation(
                  ResultRelation::cartesian(root, 1, {0, 8, 0, 0}).take_value())
              .ok(),
          "broadcast chunk source basis");
      require(builder
                  .publish_tensor(
                      0, Region::whole({count}), {0, {0}},
                      std::move(buffer).freeze(),
                      ResultRelation::cartesian(root, count, {0, 1, 0, 0})
                          .take_value(),
                      {true, true, true, true})
                  .ok(),
              "broadcast chunk source");
      ExecutionBinding binding;
      binding.name = "source";
      binding.result = builder.seal().take_value();
      WorkflowDocument doc;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "source";
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      doc.inputs = {declaration};
      doc.nodes = {{1, "test.chunk.copy", {WorkflowInputReference{1}}, {}},
                   {2, "test.chunk.sum", {WorkflowNodeOutput{1, "value"}}, {}}};
      doc.outputs = {{"out", 2, "value"}};
      GraphContext graph(doc);
      auto plan = Compiler(registry).compile(graph).take_value().plan;
      ExecutionOptions options;
      options.maximum_dependency_work = UINT64_C(1) << 30;
      options.dependencies.maximum_work = UINT64_C(1) << 30;
      options.dependencies.sets.maximum_work = UINT64_C(1) << 30;
      auto result = context->execute(plan, {{binding}}, {}, options);
      if (mode == 2) {
        require(!result.ok() &&
                    result.status().code == ErrorCode::OperationFailed &&
                    result.status().detail.node_id == 1,
                "failed chunk preserves producer provenance");
      } else if (mode == 3) {
        require(!result.ok() && result.status().code == ErrorCode::TypeMismatch,
                "unobserved association is rejected after membership check");
      } else {
        require(result.ok(), result.status().message);
        auto output = result.value().results.at("out");
        double sum = 0;
        require(output.read_tensor(output.descriptor().take_value(), 0, {0},
                                   &sum, 8)
                        .ok() &&
                    sum == count && output.association().size() == count,
                "public workflow completes chunk sum with full historical "
                "association");
        for (uint64_t edited : {uint64_t{0}, count / 2, count - 1}) {
          auto changed =
              Footprint::from_regions({count}, {Region({{edited, 1}})})
                  .take_value();
          auto dirty =
              result.value().dependencies.potential_dirty("source", changed);
          require(dirty.ok() && dirty.value().at("out") ==
                                    Footprint::all({1}).take_value(),
                  "payload-free ancestry preserves early, middle and late "
                  "source dirty mapping");
        }
        FootprintLimits evidence_limits;
        evidence_limits.maximum_work = UINT64_C(1) << 30;
        auto support =
            result.value().dependencies.source_support(evidence_limits);
        require(support.ok(), support.status().message);
        require(
            support.value().at("source") ==
                Footprint::all({count}).take_value(),
            "complete payload-free source support survives chunk retirement");
        require(observer->maximum_live == (observer->keep_first ? 2U : 1U),
                "only current and explicitly retained chunks remain live");
      }
      for (const auto& weak : observer->produced)
        require(!weak.lock().valid(),
                "chunk payload owners retire when execution actors finish");
      require(root.statistics().peak[ResourceKind::Payload] <= 4096,
              "chunk count does not increase live payload capacity");
    }
    context.reset();
    zero(root);
  }
}
struct BlockObserver {
  unsigned mode = 0, computations = 0;
  ResultRef foreign;
  WeakResultRef previous;
  ResultRef previous_owner;
  ResourceBindings resources;
  ColorProfileIdentity profile;
  CancellationSource cancellation;
};
struct BlockProgram {
  unsigned stage = 0;
  std::shared_ptr<BlockObserver> observer;
  explicit BlockProgram(std::shared_ptr<BlockObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    if (!stage++) {
      ResultProgramNeed need;
      need.tensors = {
          {0, 0, Footprint::from_regions({2}, {Region({{0, 1}})}).take_value(),
           5}};
      return Answer(std::move(need));
    }
    const auto state = [&](double number) {
      auto schema = chunk_schema(1, ElementType::Float64, "test.block.state");
      if (observer->mode == 10) {
        schema.tensors[0].descriptor.shape = {1, 4};
        ColorArrayDescriptor description;
        description.model = ColorModel::Cmyk;
        description.reference = ColorReference::ProfileRelative;
        description.white.reset();
        description.profile = observer->profile;
        schema.tensors[0].facets = {
            encode_color_array(description).take_value()};
      }
      auto builder =
          ResultBuilder::start(phase.resources, schema, "block.state", {}, {},
                               128, 128, observer->resources)
              .take_value();
      require(builder
                  .bind_descriptor_relation(
                      ResultRelation::cartesian(phase.resources, 1, {})
                          .take_value())
                  .ok(),
              "block state basis");
      const double colors[] = {number / 10, 0, 0, 0};
      require(
          builder
              .publish_tensor(
                  0, Region::whole(schema.tensors[0].sample_shape()),
                  observer->mode == 10
                      ? ByteView(reinterpret_cast<const uint8_t*>(colors), 32)
                      : ByteView(reinterpret_cast<const uint8_t*>(&number), 8),
                  ResultRelation::cartesian(
                      phase.resources,
                      schema.tensors[0].sample_count().take_value(), {})
                      .take_value(),
                  {true, true, true, true})
              .ok(),
          "block state payload");
      return builder.seal().take_value();
    };
    auto incoming = state(1);
    if (observer->mode == 11)
      incoming = incoming.capture().take_value();
    auto prepared = state(3);
    ResourceLease blocker;
    if (observer->mode == 7) {
      auto available = phase.resources.available_capacity();
      auto bytes = std::min(available[ResourceKind::Host],
                            available[ResourceKind::Metadata]) -
                   ResourceBudget::lease_metadata_bytes();
      blocker = phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                    .take_value();
    }
    if (observer->mode == 11) {
      // Vary only optional headroom after all mandatory state is prepared.
      // This covers canonical-key and input-read window admissions separately.
      for (uint64_t headroom = 256; headroom <= 8192; headroom += 16) {
        const auto available = phase.resources.available_capacity();
        const auto bytes = std::min(available[ResourceKind::Host],
                                    available[ResourceKind::Metadata]) -
                           ResourceBudget::lease_metadata_bytes() - headroom;
        auto quota =
            phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                .take_value();
        auto trial = phase.block(1, 0, 1, headroom, incoming,
                                 [&] { return Result<ResultRef>(prepared); });
        require(trial.ok(),
                "optional block key and authorized hash read admission");
      }
    }
    auto result = phase.block(
        1, 0, observer->mode == 5 ? 0 : 1, 1,
        observer->mode == 6 ? observer->foreign : incoming,
        [&]() -> Result<ResultRef> {
          ++observer->computations;
          if (observer->mode == 2)
            throw std::runtime_error("block compute failure");
          if (observer->mode == 3) {
            double ignored = 0;
            static_cast<void>(phase.read_tensor(0, 0, {1}, &ignored, 8));
          }
          if (observer->mode == 4)
            observer->cancellation.cancel();
          if (observer->mode == 1)
            return Result<ResultRef>(observer->foreign);
          if (observer->mode == 8) {
            auto available = phase.resources.available_capacity();
            auto bytes = std::min(available[ResourceKind::Host],
                                  available[ResourceKind::Metadata]) -
                         ResourceBudget::lease_metadata_bytes();
            blocker =
                phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                    .take_value();
            return Result<ResultRef>(prepared);
          }
          if (observer->mode == 7)
            return Result<ResultRef>(prepared);
          double value = 0;
          auto status = phase.read_tensor(0, 0, {0}, &value, 8);
          return status.ok() ? Result<ResultRef>(state(1 + value))
                             : Result<ResultRef>(status);
        });
    blocker = {};
    double number = 3;
    if (result.ok()) {
      auto facts = result.value().descriptor().take_value();
      require(
          result.value()
              .read_tensor(facts, 0,
                           observer->mode == 10 ? std::vector<uint64_t>{0, 0}
                                                : std::vector<uint64_t>{0},
                           &number, 8)
              .ok(),
          "Result block read");
      if (observer->previous.lock().valid())
        require(
            observer->previous.lock().object_id() != result.value().object_id(),
            "block hit creates independent current state");
      observer->previous = result.value().weak();
      observer->previous_owner = result.value();
    }
    // Ignore any service error deliberately; the enclosing publication must
    // still report its original sticky cause.
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    require(builder
                .bind_descriptor_relation(
                    ResultRelation::cartesian(
                        phase.resources, 1,
                        {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})
                        .take_value())
                .ok(),
            "block output basis");
    require(builder
                .publish_tensor(
                    0, phase.query.tensor_outputs->boxes()[0],
                    ByteView(reinterpret_cast<const uint8_t*>(&number), 8),
                    ResultRelation::cartesian(
                        phase.resources, 2,
                        {0, 5, 0, 1, ResultSupportTarget::Tensor, 0})
                        .take_value(),
                    {true, true, true, true})
                .ok(),
            "block output payload");
    return Answer(ResultPublication{builder.seal().take_value(), true});
  }
};
void result_blocks() {
  for (unsigned mode = 0; mode < 13; ++mode) {
    auto observer = std::make_shared<BlockObserver>();
    observer->mode = mode;
    auto schema = chunk_schema(2, ElementType::Float64, "test.block.input");
    if (mode == 12) {
      for (unsigned i = 0; i < 32; ++i)
        schema.tensors[0].facets.push_back(
            {"vendor.proof" + std::to_string(i), 1, {42}});
      std::sort(schema.tensors[0].facets.begin(),
                schema.tensors[0].facets.end(),
                [](const auto& a, const auto& b) { return a.key < b.key; });
    }
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition operation;
    operation.key = "test.block";
    operation.traits = unified_example::traits(1, sizeof(BlockProgram));
    if (mode == 9)
      operation.traits.workspace_bytes = 0;
    unified_example::result_port(&operation.traits.input_schema[0], schema);
    unified_example::result_output(&operation.traits.outputs[0], schema);
    operation.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<BlockProgram>(allocator, observer);
    };
    auto registered = registry->register_operation(std::move(operation));
    require(registered.ok(), "block registration mode=" + std::to_string(mode) +
                                 " message=" + registered.message);
    require(registry->freeze().ok(), "block registry freeze");
    ExecutionContextConfig config;
    config.result_cache_bytes = 4096;
    config.maximum_dependency_cache_metadata = 1;
    config.managed_resources = ResourceLimits{};
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    ResourceBudget foreign;
    if (mode == 10) {
      auto bytes = numeric_fixture::fixture();
      auto profile =
          IccProfile::import(ByteView(bytes.data(), bytes.size()), root)
              .take_value();
      observer->profile = profile.identity();
      observer->resources =
          ResourceBindings::create({profile}, root).take_value();
    }
    {
      auto make_source = [&](const ResourceBudget& budget) {
        auto builder =
            ResultBuilder::start(budget, schema, "block.input").take_value();
        require(builder
                    .bind_descriptor_relation(
                        ResultRelation::cartesian(budget, 1, {}).take_value())
                    .ok(),
                "block source basis");
        double numbers[] = {2, 4};
        require(builder
                    .publish_tensor(
                        0, Region::whole({2}),
                        ByteView(reinterpret_cast<const uint8_t*>(numbers), 16),
                        ResultRelation::cartesian(budget, 2, {}).take_value(),
                        {true, true, true, true})
                    .ok(),
                "block source payload");
        return builder.seal().take_value();
      };
      auto input = make_source(root);
      observer->foreign = make_source(foreign);
      WorkflowDocument document;
      WorkflowInputDeclaration declaration;
      declaration.id = 1;
      declaration.name = "x";
      declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
      document.inputs = {declaration};
      WorkflowNode node;
      node.id = 2;
      node.operation = "test.block";
      node.inputs = {WorkflowInputReference{1}};
      document.nodes = {node};
      document.outputs = {{"out", 2, "value"}};
      GraphContext graph(document);
      auto plan = Compiler(registry).compile(graph).take_value().plan;
      auto frozen = context->freeze(plan, {{{"x", input}}}).take_value();
      auto point = [&](uint64_t at) {
        return Footprint::from_regions({2}, {Region({{at, 1}})}).take_value();
      };
      ExecutionOptions options;
      if (mode == 12)
        options.maximum_dependency_cache_work = 16;
      auto first = context->execute_fragments(
          frozen, {{"out", point(0)}}, observer->cancellation.token(), options);
      if (mode == 0 || mode >= 7) {
        require(first.ok(),
                "valid Result block execution mode=" + std::to_string(mode) +
                    " code=" +
                    std::to_string(static_cast<unsigned>(first.status().code)) +
                    " message=" + first.status().message);
        if (mode == 0 || mode == 9) {
          auto second = context->execute_fragments(frozen, {{"out", point(1)}});
          require(second.ok() && observer->computations == 1 &&
                      second.value().diagnostics.block_cache_hits == 1,
                  "completed block reuse excludes requested output footprint");
          observer->previous_owner = {};
          require(!observer->previous.lock().valid(),
                  "block cache retains no prior Result state owner");
          auto support =
              second.value().dependencies.source_support().take_value();
          require(support.at("x") == point(0),
                  "block hit keeps current Need support");
        } else if (mode == 12) {
          require(observer->computations == 1 &&
                      first.value().diagnostics.block_cache_hits == 0 &&
                      first.value().diagnostics.dependency_cache_work <= 16,
                  "many input facets respect finite optional metadata fuel");
        } else if (mode == 11) {
          require(observer->computations == 1,
                  "optional metadata read refusals do not poison later "
                  "mandatory computation");
        } else if (mode == 10) {
          auto second = context->execute_fragments(frozen, {{"out", point(1)}});
          require(second.ok() && observer->computations == 2 &&
                      second.value().diagnostics.block_cache_hits == 0,
                  "resource-bearing block states compute without lossy cache "
                  "reuse");
          require(observer->previous_owner.resources()
                      .icc_profile(observer->profile)
                      .ok(),
                  "block state retains required profile owner");
        } else {
          require(
              observer->computations == 1 &&
                  first.value().diagnostics.block_cache_hits == 0 &&
                  context->cache_statistics().entries == 0,
              "optional allocation refusal preserves successful computation");
        }
      } else {
        const auto expected = mode == 2   ? ErrorCode::OperationFailed
                              : mode == 4 ? ErrorCode::Cancelled
                                          : ErrorCode::InvalidArgument;
        require(!first.ok() && first.status().code == expected,
                "ignored Result block failure remains sticky");
      }
      observer->foreign = {};
      observer->previous_owner = {};
    }
    context.reset();
    observer->resources = {};
    zero(root);
    zero(foreign);
  }
}

struct CheckpointObserver {
  std::vector<uint64_t> starts, restored;
  std::vector<uint64_t> targets;
  ResultCheckpoint retained;
  ResultRef foreign;
  bool bad_read = false, bad_state = false, block_retention = false;
  CancellationSource* cancel = nullptr;
};
struct CheckpointSum {
  std::shared_ptr<CheckpointObserver> observer;
  uint64_t cursor = 0, start = 0;
  double carry = 0;
  bool started = false, waiting = false;
  explicit CheckpointSum(std::shared_ptr<CheckpointObserver> value)
      : observer(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto& requested = *phase.query.tensor_outputs;
    const auto region = requested.boxes()[0];
    const auto target = region.dimensions()[0].offset;
    const auto count = requested.shape()[0];
    if (!started) {
      started = true;
      auto absent = phase.checkpoint_before(99, target);
      if (!absent.ok())
        return Answer(absent.status());
      require(!absent.value(), "checkpoint phase isolation");
      auto prior = phase.checkpoint_before(1, target);
      if (!prior.ok())
        return Answer(prior.status());
      if (prior.value()) {
        require(phase.association &&
                    phase.association->size() == prior.value()->sequence() + 1,
                "checkpoint restoration refreshes same-poll association");
        observer->retained = *prior.value();
        auto facts = prior.value()->state().descriptor();
        if (!facts.ok())
          return Answer(facts.status());
        auto status = prior.value()->state().read_tensor(facts.value(), 0, {0},
                                                         &carry, 8);
        if (!status.ok())
          return Answer(status);
        cursor = prior.value()->sequence() + 1;
        observer->restored.push_back(prior.value()->sequence());
        if (observer->bad_read) {
          double ignored = 0;
          static_cast<void>(phase.read_tensor(0, 0, {0}, &ignored, 8));
        }
        if (observer->cancel) {
          observer->cancel->cancel();
          static_cast<void>(phase.checkpoint_publish(
              1, prior.value()->sequence(), prior.value()->state()));
        }
      }
      start = cursor;
      observer->starts.push_back(cursor);
      observer->targets.push_back(target);
    }
    if (waiting) {
      double value = 0;
      auto status = phase.read_tensor(0, 0, {cursor}, &value, 8);
      if (!status.ok())
        return Answer(status);
      carry += value;
      auto schema = chunk_schema(1, ElementType::Float64, "test.carry");
      auto builder =
          ResultBuilder::start(phase.resources, schema, "carry").take_value();
      status = builder.bind_descriptor_relation(
          ResultRelation::cartesian(phase.resources, 1, {0, 8, 0, 0})
              .take_value());
      if (!status.ok())
        return Answer(status);
      status = builder.publish_tensor(
          0, Region::whole({1}),
          ByteView(reinterpret_cast<const uint8_t*>(&carry), 8),
          ResultRelation::cartesian(phase.resources, 1, {0, 1, 0, 0})
              .take_value(),
          {true, true, true, true});
      if (!status.ok())
        return Answer(status);
      auto state = builder.seal().take_value();
      if (observer->bad_state) {
        static_cast<void>(
            phase.checkpoint_publish(1, cursor, observer->foreign));
      } else {
        ResourceLease blocker;
        if (observer->block_retention) {
          const auto available = phase.resources.available_capacity();
          const auto bytes = std::min(available[ResourceKind::Host],
                                      available[ResourceKind::Metadata]) -
                             ResourceBudget::lease_metadata_bytes();
          blocker =
              phase.resources.reserve(ResourceCapacity::host(bytes, bytes))
                  .take_value();
        }
        status = phase.checkpoint_publish(1, cursor, state);
        if (!status.ok())
          return Answer(status);
      }
      ++cursor;
      waiting = false;
    }
    if (cursor <= target) {
      ResultProgramNeed need;
      auto samples = Footprint::from_regions({count}, {Region({{cursor, 1}})})
                         .take_value();
      need.tensors = {{0, 0, samples, 5}};
      waiting = true;
      return Answer(std::move(need));
    }
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto status = builder.bind_descriptor_relation(
        ResultRelation::cartesian(
            phase.resources, 1,
            {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0})
            .take_value());
    if (!status.ok())
      return Answer(status);
    auto relation = start <= target
                        ? ResultRelation::mapped(
                              phase.resources, {count}, region, {count},
                              {{-1, start, 0, target - start + 1}},
                              {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})
                        : ResultRelation::cartesian(
                              phase.resources, count,
                              {0, 1, 0, 0, ResultSupportTarget::Tensor, 0});
    if (!relation.ok())
      return Answer(relation.status());
    status = builder.publish_tensor(
        0, region, ByteView(reinterpret_cast<const uint8_t*>(&carry), 8),
        relation.take_value(), {true, true, true, true});
    if (!status.ok())
      return Answer(status);
    return Answer(ResultPublication{builder.seal().take_value(), true});
  }
};
struct DynamicCheckpointSource {
  std::shared_ptr<std::atomic<unsigned>> calls;
  explicit DynamicCheckpointSource(
      std::shared_ptr<std::atomic<unsigned>> counter)
      : calls(std::move(counter)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto status = builder.bind_descriptor_relation(
        ResultRelation::cartesian(phase.resources, 1, {}).take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    const auto base = ++*calls;
    std::array<double, 8> values{};
    for (uint64_t i = 0; i < values.size(); ++i)
      values[i] = i + base;
    status = builder.publish_tensor(
        0, Region::whole({8}),
        ByteView(reinterpret_cast<const uint8_t*>(values.data()), 64),
        ResultRelation::cartesian(phase.resources, 8, {}).take_value(),
        {true, true, true, true});
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{builder.seal().take_value(), true});
  }
};
void impure_whole_results() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto chunks = std::make_shared<ChunkObserver>();
    auto schema = chunk_schema(8, ElementType::Float64, "test.impure.whole");
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition dynamic;
    dynamic.key = "test.impure.whole";
    dynamic.traits =
        unified_example::traits(0, sizeof(DynamicCheckpointSource));
    unified_example::result_output(&dynamic.traits.outputs[0], schema);
    dynamic.traits.deterministic = mode == 1;
    dynamic.traits.side_effect_free = mode == 0;
    dynamic.traits.cacheable = false;
    dynamic.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    dynamic.start_result = [calls](const auto&, const auto& allocator) {
      return ResultContinuation::make<DynamicCheckpointSource>(allocator,
                                                               calls);
    };
    for (unsigned invalid_mode = 0; invalid_mode < 4; ++invalid_mode) {
      auto invalid = dynamic;
      invalid.key += ".invalid" + std::to_string(invalid_mode);
      if (invalid_mode == 0)
        invalid.traits.outputs[0].region_rule = OperationRegionRule::Dependency;
      else if (invalid_mode == 1)
        invalid.traits.outputs[0].observation_kind =
            ObservationKind::RequestRecord;
      else if (invalid_mode == 2)
        invalid.traits.outputs[0].failure_delivery =
            FailureDelivery::PerAtomOutcome;
      else
        invalid.traits.cacheable = true;
      require(registry->register_operation(std::move(invalid)).code ==
                  ErrorCode::InvalidArgument,
              "non-pure execution requires Whole Atomic uncached behavior");
    }
    require(registry->register_operation(std::move(dynamic)).ok(),
            "non-pure Whole Result registration");
    OperationDefinition copy;
    copy.key = "test.impure.copy";
    copy.traits = unified_example::traits(1, sizeof(ChunkCopy));
    unified_example::result_port(&copy.traits.input_schema[0], schema);
    unified_example::result_output(&copy.traits.outputs[0], schema);
    copy.start_result = [chunks](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkCopy>(allocator, chunks);
    };
    require(registry->register_operation(std::move(copy)).ok() &&
                registry->freeze().ok(),
            "pure consumer of non-pure Whole Result registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    {
      WorkflowDocument doc;
      doc.nodes = {
          {1, "test.impure.whole", {}, {}},
          {2, "test.impure.whole", {}, {}},
          {3, "test.impure.copy", {WorkflowNodeOutput{1, "value"}}, {}},
          {4, "test.impure.copy", {WorkflowNodeOutput{2, "value"}}, {}}};
      doc.outputs = {{"a", 1, "value"},
                     {"b", 2, "value"},
                     {"c", 3, "value"},
                     {"d", 4, "value"}};
      GraphContext graph(doc);
      auto plan = Compiler(registry).compile(graph).take_value().plan;
      auto frozen = context->freeze(plan, {}).take_value();
      auto point =
          Footprint::from_regions({8}, {Region({{0, 1}})}).take_value();
      ExecutionOptions options;
      options.maximum_parallelism = 1;
      options.maximum_dependency_cache_work = 0;
      const DemandQuery demand{{"a", point},
                               {"b", point},
                               {"c", point},
                               {"d", point}};
      auto first = context->execute_fragments(frozen, demand, {}, options);
      require(first.ok(), first.status().message);
      require(*calls == 2,
              "identical non-pure nodes each execute once within a Run");
      auto second = context->execute_fragments(frozen, demand, {}, options);
      require(second.ok(), second.status().message);
      require(*calls == 4,
              "non-pure closure recomputes while prior Result owners survive");
      const auto read = [&](const DemandResult& execution, const char* name) {
        const auto& output = execution.results.at(name);
        double value = 0;
        require(output
                    .read_tensor(output.descriptor().take_value(), 0, {0},
                                 &value, sizeof(value))
                    .ok(),
                "non-pure Whole Result payload read");
        return value;
      };
      for (const auto* execution : {&first.value(), &second.value()}) {
        const auto a = read(*execution, "a"), b = read(*execution, "b");
        const auto minimum = execution == &first.value() ? 1.0 : 3.0;
        require((a == minimum && b == minimum + 1) ||
                    (b == minimum && a == minimum + 1),
                "independent non-pure producers publish distinct Run values");
        require(read(*execution, "c") == a && read(*execution, "d") == b,
                "pure consumers retain their own non-pure producer results");
      }
    }
    context.reset();
    zero(root);
  }
}
void result_checkpoints() {
  for (auto mode : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}) {
    auto observer = std::make_shared<CheckpointObserver>();
    auto chunks = std::make_shared<ChunkObserver>();
    auto schema =
        chunk_schema(8, ElementType::Float64, "test.checkpoint.input");
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition copy;
    copy.key = "checkpoint.copy";
    copy.traits = unified_example::traits(1, sizeof(ChunkCopy));
    unified_example::result_port(&copy.traits.input_schema[0], schema);
    unified_example::result_output(&copy.traits.outputs[0], schema);
    copy.start_result = [chunks](const auto&, const auto& allocator) {
      return ResultContinuation::make<ChunkCopy>(allocator, chunks);
    };
    require(registry->register_operation(std::move(copy)).ok(),
            "checkpoint copy registration");
    auto dynamic_calls = std::make_shared<std::atomic<unsigned>>(0);
    if (mode == 9) {
      OperationDefinition dynamic;
      dynamic.key = "checkpoint.dynamic";
      dynamic.traits =
          unified_example::traits(0, sizeof(DynamicCheckpointSource));
      unified_example::result_output(&dynamic.traits.outputs[0], schema);
      dynamic.traits.deterministic = dynamic.traits.side_effect_free =
          dynamic.traits.cacheable = false;
      dynamic.traits.outputs[0].region_rule = OperationRegionRule::Whole;
      dynamic.start_result = [dynamic_calls](const auto&,
                                             const auto& allocator) {
        return ResultContinuation::make<DynamicCheckpointSource>(allocator,
                                                                 dynamic_calls);
      };
      require(registry->register_operation(std::move(dynamic)).ok(),
              "dynamic Result producer registration");
    }
    OperationDefinition sum;
    sum.key = "checkpoint.sum";
    sum.traits = unified_example::traits(1, sizeof(CheckpointSum));
    unified_example::result_port(&sum.traits.input_schema[0], schema);
    unified_example::result_output(&sum.traits.outputs[0], schema);
    sum.start_result = [observer](const auto&, const auto& allocator) {
      return ResultContinuation::make<CheckpointSum>(allocator, observer);
    };
    require(registry->register_operation(std::move(sum)).ok() &&
                registry->freeze().ok(),
            "checkpoint sum registration");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    auto context = std::make_unique<ExecutionContext>(registry, config);
    auto root = context->resource_budget().take_value();
    {
      auto make_source = [&](const ResourceBudget& budget, double bias = 0) {
        auto builder =
            ResultBuilder::start(budget, schema, "input").take_value();
        require(builder
                    .bind_descriptor_relation(
                        ResultRelation::cartesian(budget, 1, {0, 8, 0, 0})
                            .take_value())
                    .ok(),
                "checkpoint input basis");
        double values[8];
        for (uint64_t i = 0; i < 8; ++i)
          values[i] = i + 1 + bias;
        require(builder
                    .publish_tensor(
                        0, Region::whole({8}),
                        ByteView(reinterpret_cast<const uint8_t*>(values), 64),
                        ResultRelation::cartesian(budget, 8, {0, 1, 0, 0})
                            .take_value(),
                        {true, true, true, true})
                    .ok(),
                "checkpoint input samples");
        return builder.seal().take_value();
      };
      ExecutionBinding binding;
      binding.name = "source";
      binding.result = make_source(root);
      observer->bad_read = mode == 1;
      observer->bad_state = mode == 2;
      observer->block_retention = mode == 5;
      if (observer->bad_state)
        observer->foreign = make_source(ResourceBudget{});
      CancellationSource cancelled;
      if (mode == 3)
        observer->cancel = &cancelled;
      WorkflowDocument doc;
      WorkflowInputDeclaration input;
      input.id = 1;
      input.name = "source";
      input.result_schema = std::make_shared<SchemaTemplate>(schema);
      doc.inputs = {input};
      doc.nodes = {{1, "checkpoint.copy", {WorkflowInputReference{1}}, {}},
                   {2, "checkpoint.sum", {WorkflowNodeOutput{1, "value"}}, {}},
                   {3, "checkpoint.sum", {WorkflowNodeOutput{1, "value"}}, {}},
                   {4, "checkpoint.sum", {WorkflowNodeOutput{1, "value"}}, {}}};
      if (mode == 9)
        doc.nodes[0] = {1, "checkpoint.dynamic", {}, {}};
      if (mode == 8) {
        doc.nodes.clear();
        for (uint64_t i = 1; i <= 12; ++i)
          doc.nodes.push_back(
              {i,
               "checkpoint.copy",
               {i == 1 ? WorkflowInput{WorkflowInputReference{1}}
                       : WorkflowInput{WorkflowNodeOutput{i - 1, "value"}}},
               {}});
        for (uint64_t i = 2; i <= 4; ++i)
          doc.nodes.push_back({i + 20,
                               "checkpoint.sum",
                               {WorkflowNodeOutput{12, "value"}},
                               {}});
      }
      doc.outputs = {{"a", 2, "value"}, {"b", 3, "value"}, {"c", 4, "value"}};
      if (mode == 8)
        doc.outputs = {{"a", 22, "value"},
                       {"b", 23, "value"},
                       {"c", 24, "value"}};
      GraphContext graph(doc);
      auto plan = Compiler(registry).compile(graph).take_value().plan;
      auto frozen = context->freeze(plan, {{binding}}).take_value();
      const auto point = [](uint64_t at) {
        return Footprint::from_regions({8}, {Region({{at, 1}})}).take_value();
      };
      ExecutionOptions options;
      options.maximum_parallelism = 1;
      if (mode == 4)
        options.maximum_dependency_cache_work = 0;
      if (mode == 8)
        options.maximum_dependency_cache_work = 1200;
      if (mode == 1 || mode == 3 || mode == 6 || mode == 7 || mode == 9) {
        ExecutionBinding alternate = binding;
        if (mode == 6)
          alternate.result = make_source(root, 100);
        auto other = mode == 6
                         ? context->freeze(plan, {{alternate}}).take_value()
                         : frozen;
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false;
        auto held_options = options;
        held_options.result_publication = [&](ValueRef ref, const ResultRef&) {
          if (ref.node_id != 2)
            return Status::success();
          std::unique_lock<std::mutex> lock(mutex);
          entered = true;
          changed.notify_all();
          if (!changed.wait_for(lock, std::chrono::seconds(5),
                                [&] { return release; }))
            return Status{ErrorCode::Cancelled, "checkpoint barrier timed out"};
          return Status::success();
        };
        auto first = std::async(std::launch::async, [&] {
          return context->execute_fragments(frozen, {{"a", point(2)}}, {},
                                            held_options);
        });
        {
          std::unique_lock<std::mutex> lock(mutex);
          require(changed.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return entered; }),
                  "completed checkpoint scope remains active at barrier");
        }
        auto second = context->execute_fragments(
            other, {{mode == 6 ? "a" : "b", point(mode == 6 ? 2 : 5)}},
            cancelled.token());
        {
          std::lock_guard<std::mutex> lock(mutex);
          release = true;
        }
        changed.notify_all();
        auto completed = first.get();
        require(completed.ok(), completed.status().message);
        if (mode == 1 || mode == 3) {
          require(
              !second.ok() && second.status().code ==
                                  (mode == 3 ? ErrorCode::Cancelled
                                             : ErrorCode::InvalidArgument),
              "ignored checkpoint failures after actual restore are sticky");
          require(observer->restored == std::vector<uint64_t>{2},
                  "negative checkpoint probe restores the completed state");
        } else {
          require(second.ok(), second.status().message);
          const auto& output = second.value().results.at(mode == 6 ? "a" : "b");
          double actual = 0;
          require(
              output.read_tensor(output.descriptor().take_value(), 0,
                                 {mode == 6 ? 2U : 5U}, &actual, 8)
                      .ok() &&
                  actual == (mode == 6   ? 306.0
                             : mode == 9 ? 27.0
                                         : 21.0),
              "active frozen runs isolate bindings and share only completed "
              "state");
          require(observer->starts == (mode == 6 || mode == 9
                                           ? std::vector<uint64_t>{0, 0}
                                           : std::vector<uint64_t>{0, 3}),
                  "checkpoint sharing uses the full frozen input basis");
          auto dirty = second.value().dependencies.potential_dirty("source",
                                                                   point(0), 1);
          require(
              dirty.ok() && (mode == 9 ||
                             !dirty.value().at(mode == 6 ? "a" : "b").empty()),
              "peer checkpoint lookup imports payload-free source ancestry");
          if (mode == 7)
            require(chunks->produced.size() == 6,
                    "completed peer checkpoint avoids earlier source "
                    "recomputation");
          if (mode == 9)
            require(*dynamic_calls == 2 && observer->restored.empty(),
                    "checkpoint sharing excludes an impure upstream closure");
        }
      } else {
        const DemandQuery requested = mode == 5 ? DemandQuery{{"a", point(2)}}
                                                : DemandQuery{{"a", point(2)},
                                                              {"b", point(5)},
                                                              {"c", point(0)}};
        auto result = context->execute_fragments(frozen, requested,
                                                 cancelled.token(), options);
        if (mode == 2) {
          require(!result.ok() && result.status().code ==
                                      (mode == 3 ? ErrorCode::Cancelled
                                                 : ErrorCode::InvalidArgument),
                  "ignored checkpoint failures are sticky");
        } else {
          require(result.ok(), result.status().message);
          const auto queries = mode == 5 ? 1U : 3U;
          require(observer->starts.size() == queries,
                  "each requested checkpoint query starts once");
          require(observer->targets.size() == queries,
                  "checkpoint cursors retain their requested targets");
          std::vector<uint64_t> restored;
          for (size_t i = 0; i < queries; ++i) {
            require(observer->starts[i] <= observer->targets[i] + 1,
                    "checkpoint cursor never restores a future sequence");
            if (observer->starts[i])
              restored.push_back(observer->starts[i] - 1);
          }
          require(observer->restored == restored,
                  "restored phase and sequence agree with computation cursors");
          if (mode >= 4)
            require(observer->starts == std::vector<uint64_t>(queries, 0) &&
                        observer->restored.empty(),
                    "disabled or unretained checkpoints never restore state");
          for (const auto& entry :
               {std::make_pair("a", 6.0), std::make_pair("b", 21.0),
                std::make_pair("c", 1.0)}) {
            if (mode == 5 && entry.first != std::string("a"))
              continue;
            const auto& output = result.value().results.at(entry.first);
            const auto at = entry.first == std::string("a")   ? 2U
                            : entry.first == std::string("b") ? 5U
                                                              : 0U;
            double actual = 0;
            require(output.read_tensor(output.descriptor().take_value(), 0,
                                       {at}, &actual, 8)
                            .ok() &&
                        actual == entry.second,
                    "checkpoint result values");
          }
          for (uint64_t edited : {0U, 2U, 4U, 7U}) {
            auto dirty = result.value().dependencies.potential_dirty(
                "source", point(edited), 1);
            require(dirty.ok(), dirty.status().message);
            require(dirty.value().at("a").empty() == (edited > 2) &&
                        (mode == 5 ||
                         (dirty.value().at("b").empty() == (edited > 5) &&
                          dirty.value().at("c").empty() == (edited > 0))),
                    "restored checkpoint carries complete prefix ancestry");
          }
          const auto depth = mode == 8 ? 12U : 1U;
          require(chunks->produced.size() >= (mode == 5 ? 3U : 6U) * depth &&
                      chunks->produced.size() <= (mode == 5 ? 3U : 10U) * depth,
                  "parallel queries compute only requested source prefixes");
          require(result.value().diagnostics.dependency_cache_work <=
                      options.maximum_dependency_cache_work,
                  "checkpoint ancestor copies remain in optional work budget");
        }
      }
      for (const auto& weak : chunks->produced)
        require(!weak.lock().valid(),
                "checkpoint history retains no chunk payload");
    }
    context.reset();
    if (observer->retained.valid()) {
      double value = 0;
      auto state = observer->retained.state();
      require(
          state.read_tensor(state.descriptor().take_value(), 0, {0}, &value, 8)
                  .ok() &&
              (value == 1 || value == 6),
          "owning checkpoint state survives execution context retirement");
    }
    observer->retained = {};
    observer->foreign = {};
    zero(root);
  }
}
void tuple_metadata() {
  auto schema = unified_example::image_schema();
  auto& image = schema.tensors[0];
  image.descriptor.shape = {2, 4, 4};
  image.layout.channel_axis = 2;
  image.facets = {encode_semantic(rgba_semantics()).take_value()};
  require(schema.validate(true).ok(), "RGBA image slot");
  auto q =
      Footprint::from_regions(
          {2, 2, 2, 4, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}, {1, 1}})})
          .take_value();
  auto closed = image.close_samples(q).take_value();
  require(closed.element_count().value() == 4 &&
              closed.boxes()[0].dimensions()[4].offset == 0,
          "RGBA full tuple closure");
  image.descriptor = {ElementType::Float64, {2, 4, 3}};
  image.layout.channel_axis = 2;
  image.facets = {encode_color_array(ColorArrayDescriptor{}).take_value()};
  require(schema.validate(true).ok(), "XYZ ColorArray image slot");
  auto color =
      Footprint::from_regions(
          {2, 2, 2, 4, 3}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}, {1, 1}})})
          .take_value();
  require(image.close_samples(color).value().element_count().value() == 3,
          "ColorArray tuple closure");
  TensorDescription tensor;
  tensor.channel_axis = 0;
  tensor.channels.resize(6);
  TensorColorGroup group;
  group.name = "rgb";
  group.indices = {0, 2, 4};
  group.components = {{"R", "red", "relative"},
                      {"G", "green", "relative"},
                      {"B", "blue", "relative"}};
  group.alpha = 5;
  group.interpretation.model = "rgb";
  group.interpretation.primaries = "srgb";
  group.interpretation.transfer = "linear";
  tensor.sampling = TensorSampling{"image-grid", {1, 1}, {0, 0}};
  tensor.groups = {group};
  tensor.channels[5].sampling = TensorSampling{"image-grid", {1, 1}, {0, 0}};
  image.descriptor = {ElementType::Float32, {6, 2, 4}};
  image.layout.channel_axis = 0;
  image.layout.height_axis = 1;
  image.layout.width_axis = 2;
  image.facets = {encode_tensor_description(tensor).take_value()};
  require(schema.validate(true).ok(), "TDM CHW slot");
  auto sparse =
      Footprint::from_regions(
          {2, 2, 6, 2, 4}, {Region({{1, 1}, {0, 1}, {2, 1}, {1, 1}, {2, 1}})})
          .take_value();
  require(image.close_samples(sparse).value() == sparse,
          "TDM channel has no implicit peers or alpha");
  tensor.channels[5].sampling->grid = "other-grid";
  require(!encode_tensor_description(tensor).ok(),
          "alpha distinct grid rejected");
}
void profile_owner() {
  ResourceBudget root;
  ResultRef survivor;
  ColorProfileIdentity identity;
  {
    auto bytes = numeric_fixture::fixture();
    auto profile =
        IccProfile::import(ByteView(bytes.data(), bytes.size()), root)
            .take_value();
    identity = profile.identity();
    auto resources = ResourceBindings::create({profile}, root).take_value();
    auto schema = unified_example::image_schema();
    schema.id = "test.profile";
    auto& slot = schema.tensors[0];
    slot.batch_axes[0] = slot.batch_axes[1] = 1;
    slot.descriptor.shape = {1, 1, 4};
    slot.layout.channel_axis = 2;
    TensorDescription tensor;
    tensor.channel_axis = 2;
    tensor.model = "cmyk";
    tensor.profile = identity;
    tensor.channels = {{"C", "cyan", "relative"},
                       {"M", "magenta", "relative"},
                       {"Y", "yellow", "relative"},
                       {"K", "black", "relative"}};
    slot.facets = {encode_tensor_description(tensor).take_value()};
    auto registry = std::make_shared<OperationRegistry>();
    require(join(registry.get(), schema).ok(), "profile join");
    require(registry->freeze().ok(), "profile freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto destination = context.resource_budget().take_value();
    auto input = image(destination, schema, 0, resources);
    WorkflowDocument doc;
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "source";
    declaration.result_schema = std::make_shared<SchemaTemplate>(schema);
    doc.inputs = {declaration};
    doc.nodes = {{1,
                  "test.image.join",
                  {WorkflowInputReference{1}, WorkflowInputReference{1}},
                  {}}};
    doc.outputs = {{"out", 1, "value"}};
    GraphContext graph(doc);
    auto plan = Compiler(registry).compile(graph, {}, resources);
    require(plan.ok(), plan.status().message);
    ExecutionBinding binding;
    binding.name = "source";
    binding.result = input;
    auto result = context.execute(plan.value().plan, {{binding}});
    require(result.ok(), result.status().message);
    survivor = result.value().results.at("out");
    require(survivor.resources().icc_profile(identity).ok(),
            "profile resource inherited through Result");
  }
  require(survivor.resources().icc_profile(identity).ok(),
          "profile outlives context and original owner");
  float read = 0;
  require(survivor.read_tensor(survivor.descriptor().value(), 0,
                               {0, 0, 0, 0, 3}, &read, 4)
                  .ok() &&
              read == 6,
          "image backing outlives context");
  survivor = {};
  zero(root);
}
}  // namespace
int main() {
  try {
    captured_prefix();
    resource_admission();
    staged_input_retirement();
    result_blocks();
    result_checkpoints();
    impure_whole_results();
    tuple_metadata();
    alias_diamond();
    slots_and_guarantees();
    profile_owner();
    std::cout << "Result image contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
