#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
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
  for (uint32_t slot = 0; slot < schema.images.size(); ++slot) {
    auto count = schema.images[slot].sample_count().value();
    std::vector<float> samples(count);
    for (uint64_t i = 0; i < count; ++i)
      samples[i] =
          (biased_slot == UINT32_MAX || slot == biased_slot ? bias : 0) + i;
    auto status = builder.publish_image(
        slot, Region::whole(schema.images[slot].sample_shape()),
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
  schema.images[0].facets = {{"test.large", 1, std::vector<uint8_t>(65536, 7)}};
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
    schema.images[0].frames = 3;
    schema.images[0].layers = 1;
    schema.images[0].descriptor.shape = {1, 1};
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
                .publish_image(0, Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}}),
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
        builder.publish_image(0, Region({{1, 2}, {0, 1}, {0, 1}, {0, 1}}),
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
    require(
        old.read_image(old.descriptor(false).value(), 0, {0, 0, 0, 0}, &read, 4)
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
        phase.query.image_outputs
            ? Result<Footprint>(*phase.query.image_outputs)
            : Footprint::all(
                  phase.query.output.result_schema->images[0].sample_shape());
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
      need.images = {{0, 0, upstream.value(), 1}, {1, 0, samples.value(), 1}};
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
            auto read = phase.read_image(0, 0, source, &first, 4);
            if (!read.ok())
              return read;
            read = phase.read_image(1, 0, at, &second, 4);
            if (!read.ok())
              return read;
            const float sum = first + second;
            std::memcpy(buffer.data() + 4 * position++, &sum, 4);
            auto index = unified_example::flat(at, samples.value().shape());
            rows.push_back(
                {index,
                 {0, 1, unified_example::flat(source, samples.value().shape()),
                  1, ResultSupportTarget::Image, 0}});
            rows.push_back(
                {index, {1, 1, index, 1, ResultSupportTarget::Image, 0}});
            return Status::success();
          },
          count);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
      auto relation = ResultRelation::sample_rows(
          phase.resources,
          phase.query.output.result_schema->images[0].sample_count().value(),
          rows.size(),
          [&](auto i) { return Result<ResultRelationRow>(rows[i]); });
      if (!relation.ok())
        return Result<ResultProgramPoll>(relation.status());
      status =
          output.publish_image(0, box, ByteView(buffer.data(), buffer.size()),
                               relation.take_value(), {true, true, true, true});
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
      require(owner.read_image(owner.descriptor().value(), 0, {1, 1, 1, 2},
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
struct Probe {
  unsigned stage = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto shape =
        phase.query.inputs[0].result_schema->images[1].sample_shape();
    const std::vector<uint64_t> at{0, 1, 0, 2};
    if (!stage++) {
      ResultProgramNeed need;
      need.results = {{0, 0, true, 0}};
      if (!phase.query.value_outputs->empty())
        need.images = {{0, 1,
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
    ResourceVector<Value> parts;
    Result<ResultRelation> relation =
        ResultRelation::cartesian(phase.resources, 1, {0, 1, 0, 0});
    if (!phase.query.value_outputs->empty()) {
      if (std::get<bool>(phase.query.parameters.at("bad"))) {
        float ignored = 0;
        static_cast<void>(
            phase.images->at({0, 1}).read({0, 1, 0, 0}, &ignored, 4));
      }
      float value = 0;
      auto read = phase.read_image(0, 1, at, &value, 4);
      if (!read.ok())
        return Result<ResultProgramPoll>(read);
      auto writer = MutableValue::allocate({ElementType::Float32, {1}},
                                           Region::whole({1}), phase.allocator)
                        .take_value();
      std::memcpy(writer.data(), &value, 4);
      parts.push_back(std::move(writer).publish().take_value());
      relation = ResultRelation::cartesian(
          phase.resources, 1, {0, 1, 5, 1, ResultSupportTarget::Image, 1});
    }
    if (!relation.ok())
      return Result<ResultProgramPoll>(relation.status());
    auto fragments = ValueFragments::create_view({ElementType::Float32, {1}},
                                                 {}, *phase.query.value_outputs,
                                                 parts.data(), parts.size());
    if (!fragments.ok())
      return Result<ResultProgramPoll>(fragments.status());
    return Result<ResultProgramPoll>(ResultValuePublication{
        fragments.take_value(), relation.take_value(), basis.take_value()});
  }
};
struct Escaped {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    float zero = 0;
    auto writer = MutableValue::allocate({ElementType::Float32, {1}},
                                         Region::whole({1}), phase.allocator)
                      .take_value();
    std::memcpy(writer.data(), &zero, 4);
    auto value = std::move(writer).publish().take_value();
    auto fragments =
        ValueFragments::create_view(value.descriptor(), {},
                                    *phase.query.value_outputs, &value, 1)
            .take_value();
    auto known =
        ResultRelation::cartesian(phase.resources, 1,
                                  {0, 1, 5, 1, ResultSupportTarget::Image, 1})
            .take_value();
    auto unresolved = ResultRelation::unknown(phase.resources, 1).take_value();
    auto relation = ResultRelation::unite(phase.resources, {known, unresolved})
                        .take_value();
    auto basis = ResultRelation::cartesian(phase.resources, 1, {0, 8, 0, 0})
                     .take_value();
    return Result<ResultProgramPoll>(ResultValuePublication{
        std::move(fragments), std::move(relation), std::move(basis)});
  }
};
void slots_and_guarantees() {
  auto schema = unified_example::image_schema();
  auto depth = schema.images[0];
  depth.key = "depth";
  depth.frames = 1;
  depth.layers = 2;
  depth.descriptor.shape = {1, 3};
  schema.images.push_back(depth);
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "test.image.probe";
  op.traits = unified_example::traits(1, sizeof(Probe));
  unified_example::result_port(&op.traits.input_schema[0], schema);
  op.traits.outputs[0].output_element_type = ElementType::Float32;
  op.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
  op.traits.outputs[0].fixed_output_shape = {1};
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
  escaped.traits.outputs[0].output_element_type = ElementType::Float32;
  escaped.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
  escaped.traits.outputs[0].fixed_output_shape = {1};
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
      ResultSupportTarget::Image, 1);
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
      changed.value().values.at("g1").read({0}, &value, 4).ok() && value == 105,
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
void tuple_metadata() {
  auto schema = unified_example::image_schema();
  auto& image = schema.images[0];
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
    auto& slot = schema.images[0];
    slot.frames = slot.layers = 1;
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
  require(survivor.read_image(survivor.descriptor().value(), 0, {0, 0, 0, 0, 3},
                              &read, 4)
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
