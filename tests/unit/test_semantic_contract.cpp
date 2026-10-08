#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/data/semantic.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
SemanticDescriptor signal() {
  SemanticDescriptor s;
  s.kind = SemanticKind::SampledSignal;
  s.channels = {{"value", "value", "dimensionless"}};
  s.sample_origin = 0;
  s.sample_step = .5;
  s.sample_axis_unit = "seconds";
  return s;
}
int codec() {
  const auto original = encode_semantic(signal());
  PS_CHECK(original.ok());
  PS_CHECK(original.value().key == "photospider.semantic");
  PS_CHECK(decode_semantic(original.value()).ok());
  auto parameter = semantic_parameter(signal()).take_value();
  auto decoded = semantic_from_parameter(parameter);
  PS_CHECK(decoded.ok() && decoded.value().sample_axis_unit == "seconds");
  PS_CHECK(encode_semantic(decoded.value()).value().payload ==
           original.value().payload);
  PS_CHECK(!semantic_from_parameter(parameter + "0").ok());
  PS_CHECK(!semantic_from_parameter("FF").ok());
  PS_CHECK(!semantic_from_parameter(std::string(8193, '0')).ok());
  for (std::size_t size = 0; size < original.value().payload.size(); ++size) {
    auto truncated = original.value();
    truncated.payload.resize(size);
    PS_CHECK(!decode_semantic(truncated).ok());
  }
  auto bad = original.value();
  bad.payload.push_back(0);
  PS_CHECK(!decode_semantic(bad).ok());
  bad = original.value();
  bad.payload[4 + std::strlen("sampled_signal")] = 65;
  PS_CHECK(!decode_semantic(bad).ok());
  bad.payload.resize(4097);
  PS_CHECK(!decode_semantic(bad).ok());
  auto zero = signal();
  zero.sample_origin = -0.0;
  PS_CHECK(encode_semantic(zero).value().payload == original.value().payload);
  auto bgr = rgba_semantics();
  std::swap(bgr.channels[0], bgr.channels[2]);
  auto permuted = encode_semantic(bgr);
  PS_CHECK(permuted.ok());
  PS_CHECK(decode_semantic(permuted.value()).value().channels[0].role ==
           "blue");
  auto rgb = encode_semantic(rgba_semantics()).take_value();
  rgb.version = 1;
  PS_CHECK(!decode_semantic(rgb).ok());
  auto duplicate = signal();
  duplicate.channels.push_back(duplicate.channels.front());
  PS_CHECK(!encode_semantic(duplicate).ok());
  PS_CHECK(!validate_semantic_descriptor(signal(), {ElementType::Float32, {0}})
                .ok());
  auto alpha = rgba_semantics();
  const auto value = [&](float a) {
    float samples[] = {-2, 4, .25F, a};
    std::vector<std::uint8_t> bytes(sizeof(samples));
    std::memcpy(bytes.data(), samples, sizeof(samples));
    return Value::create({ElementType::Float32, {1, 1, 4}},
                         Region::whole({1, 1, 4}), {0, {16, 16, 4}}, bytes)
        .take_value();
  };
  PS_CHECK(validate_semantic_value(alpha, value(.5F)).ok());
  PS_CHECK(!validate_semantic_value(alpha, value(0)).ok());
  alpha.association = "straight";
  PS_CHECK(validate_semantic_value(alpha, value(0)).ok());
  PS_CHECK(!validate_semantic_value(alpha, value(2)).ok());
  PS_CHECK(
      validate_semantic_value(alpha, value(0), ErrorCode::OperationFailed, [] {
        return ErrorCode::Cancelled;
      }).code == ErrorCode::Cancelled);
  return 0;
}
using multi_result::check;
using multi_result::take;
OperationMetadata metadata(SchemaTemplate schema) {
  OperationMetadata result;
  result.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  return result;
}
SchemaTemplate semantic_schema(const SemanticDescriptor& semantic) {
  const bool image = semantic.kind == SemanticKind::Image;
  const bool mask = semantic.kind == SemanticKind::Mask;
  auto schema = multi_result::schema(ElementType::Float32,
                                     image ? std::vector<std::uint64_t>{1, 1, 4}
                                     : mask ? std::vector<std::uint64_t>{1, 1}
                                            : std::vector<std::uint64_t>{3});
  schema.tensors[0].facets = {take(encode_semantic(semantic))};
  schema.tensors[0].layout.spatial = image;
  return schema;
}
struct TensorProgram {
  bool copy_samples, requested = false;
  explicit TensorProgram(bool copy_samples) : copy_samples(copy_samples) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto shape = spec.sample_shape();
    const auto demand =
        phase.query.tensor_outputs.value_or(take(Footprint::all(shape)));
    if (!requested && !demand.empty() && !phase.query.inputs.empty()) {
      requested = true;
      ResultProgramNeed need;
      for (unsigned i = 0; i < phase.query.inputs.size(); ++i)
        need.tensors.push_back(
            {i, 0,
             take(Footprint::all(phase.query.inputs[i]
                                     .result_schema->tensors[0]
                                     .sample_shape())),
             copy_samples && i == 0 ? 9U : 8U});
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto count = take(demand.element_count());
    if (count > 4096)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::ResourceExhausted, "bounded semantic fixture"});
    check(phase.consume_work(count));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    std::vector<ResultRelation> descriptor;
    if (!demand.empty()) {
      for (unsigned i = 0; i < phase.query.inputs.size(); ++i)
        descriptor.push_back(take(ResultRelation::cartesian(
            phase.resources, 1,
            {i, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
    }
    check(builder.bind_descriptor_relation(
        descriptor.empty()
            ? take(ResultRelation::cartesian(phase.resources, 1, {}))
            : take(ResultRelation::unite(phase.resources, descriptor))));
    const auto width = Value::element_size(spec.descriptor.element_type);
    for (const auto& box : demand.boxes()) {
      const auto size = take(box.element_count());
      auto bytes = take(phase.allocator.allocate(size * width));
      std::memset(bytes.data(), 0, bytes.size());
      auto relation = take(ResultRelation::cartesian(
          phase.resources, take(spec.sample_count()), {}));
      if (copy_samples) {
        const auto& input = phase.query.inputs[0].result_schema->tensors[0];
        std::uint64_t next = 0;
        check(
            take(Footprint::from_regions(shape, {box}))
                .visit(
                    [&](const auto& at) {
                      auto* destination = bytes.data() + next++ * width;
                      if (input.descriptor.element_type ==
                          spec.descriptor.element_type)
                        return phase.read_tensor(0, 0, at, destination, width);
                      float source = 0;
                      auto status =
                          phase.read_tensor(0, 0, at, &source, sizeof(source));
                      const double converted = source;
                      std::memcpy(destination, &converted, sizeof(converted));
                      return status;
                    },
                    size, phase.query.cancellation));
        std::vector<ResultMappedAxis> axes;
        for (unsigned axis = 0; axis < shape.size(); ++axis)
          axes.push_back({static_cast<std::int32_t>(axis), 0, 1, 1});
        relation = take(ResultRelation::mapped(
            phase.resources, shape, box, input.sample_shape(), axes,
            {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}));
      }
      check(builder.publish_tensor(0, box, ByteView(bytes.data(), bytes.size()),
                                   std::move(relation),
                                   {true, true, true, true}));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
OperationDefinition definition(std::string key, const SchemaTemplate& schema,
                               bool copy_samples = false) {
  OperationDefinition result;
  result.key = std::move(key);
  result.traits.outputs = {multi_result::output("value", schema)};
  result.traits.workspace_bytes = 32768;
  result.start_result = [copy_samples](const ResultProgramQuery&,
                                       const BufferAllocator& allocator) {
    return ResultContinuation::make<TensorProgram>(allocator, copy_samples);
  };
  return result;
}
OperationDefinition generator() {
  auto prototype = multi_result::schema(ElementType::Float32, {1});
  prototype.id = "test.generate";
  auto d = definition("test.generate", prototype);
  d.traits.requires_metadata_specialization = true;
  d.traits.outputs[0].output_schema.element_type_mask = 12;
  d.traits.outputs[0].output_schema.rank = 1;
  d.traits.parameter_schema = {
      {"count", OperationParameterType::Int64, true},
      {"dtype", OperationParameterType::String, true},
      {"semantic", OperationParameterType::String, true}};
  d.prepare_static =
      [prototype](const std::vector<OperationMetadata>&,
                  const std::map<std::string, ParameterValue>& parameters)
      -> Result<OperationPreparation> {
    const auto count = std::get<std::int64_t>(parameters.at("count"));
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (count <= 0 || (dtype != "float32" && dtype != "float64"))
      return Result<OperationPreparation>(
          Status{ErrorCode::InvalidArgument, "invalid generator count/dtype"});
    auto semantic = semantic_from_parameter(
        std::get<std::string>(parameters.at("semantic")));
    if (!semantic.ok())
      return Result<OperationPreparation>(semantic.status());
    auto schema = prototype;
    schema.tensors[0].descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        {static_cast<std::uint64_t>(count)}};
    schema.tensors[0].facets = {take(encode_semantic(semantic.value()))};
    OperationPreparation prepared;
    prepared.outputs.resize(1);
    prepared.outputs[0].metadata = metadata(std::move(schema));
    return Result<OperationPreparation>(std::move(prepared));
  };
  return d;
}
OperationDefinition copy_definition(const std::string& key,
                                    const SchemaTemplate& prototype,
                                    bool drop = false, bool widen = false) {
  auto output = prototype;
  output.id = "test.copy_samples";
  auto d = definition(key, output, true);
  d.traits.input_count = 1;
  OperationPortConstraint port;
  port.kind = OperationPortKind::Result;
  port.element_type_mask = 12;
  d.traits.input_schema = {port};
  d.traits.requires_metadata_specialization = true;
  d.prepare_static = [drop, widen](const std::vector<OperationMetadata>& inputs,
                                   const std::map<std::string, ParameterValue>&)
      -> Result<OperationPreparation> {
    auto schema = *inputs[0].result_schema;
    schema.id = "test.copy_samples";
    if (drop) {
      schema.tensors[0].facets.clear();
      schema.tensors[0].layout.spatial = false;
      schema.tensors[0].atomic_trailing_axes = 0;
    }
    if (widen)
      schema.tensors[0].descriptor.element_type = ElementType::Float64;
    OperationPreparation result;
    result.outputs.resize(1);
    result.outputs[0].metadata = metadata(std::move(schema));
    return Result<OperationPreparation>(std::move(result));
  };
  return d;
}
ResultRef source(const ResourceBudget& root, const SchemaTemplate& schema,
                 const std::vector<float>& values) {
  auto builder = take(ResultBuilder::start(root, schema, "semantic.source"));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  const auto& tensor = schema.tensors[0];
  check(builder.publish_tensor(
      0, Region::whole(tensor.sample_shape()),
      ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
               values.size() * sizeof(float)),
      take(ResultRelation::cartesian(root, take(tensor.sample_count()), {})),
      {true, true, true, true}));
  return take(builder.seal());
}
int inference() {
  auto registry = std::make_shared<OperationRegistry>();
  auto d = generator();
  PS_CHECK(registry->register_operation(d).ok());
  auto bad = d;
  bad.key = "bad";
  bad.traits.parameter_schema.push_back(bad.traits.parameter_schema[0]);
  PS_CHECK(!registry->register_operation(bad).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.nodes = {{1,
                d.key,
                {},
                {{"count", INT64_C(3)},
                 {"dtype", std::string("float32")},
                 {"semantic", take(semantic_parameter(signal()))}}}};
  doc.outputs = {{"output", 1, "value"}};
  GraphContext graph(doc);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  const auto& node = compiled.value().semantic.nodes()[0];
  const auto& tensor = node.outputs[0].result_schema->tensors[0];
  PS_CHECK(tensor.descriptor.shape == std::vector<std::uint64_t>{3});
  PS_CHECK(tensor.descriptor.element_type == ElementType::Float32);
  PS_CHECK(!tensor.facets.empty());
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  auto run = execution.execute(compiled.value().plan);
  if (!run.ok())
    std::cerr << run.status().message << "\n";
  PS_CHECK(run.ok());
  const auto& output = run.value().results.at("output");
  PS_CHECK(output.schema().tensors[0].facets[0].payload ==
           tensor.facets[0].payload);
  ResultProgramMetadata direct_metadata;
  direct_metadata.output.result_schema = node.outputs[0].result_schema;
  ResultProgramQuery query(direct_metadata, doc.nodes[0].parameters);
  query.semantic_key = "test.generate.direct";
  ResourceBudget root;
  auto allocator = root.allocator();
  auto continuation = registry->start_result(d.key, query, allocator);
  PS_CHECK(continuation.ok());

  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  ResultProgramPhase phase{
      query,
      objects,
      io,
      allocator,
      root,
      [&](std::uint64_t units) { return root.consume({units}); },
      std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
  auto state = continuation.take_value();
  auto direct = state.poll(phase);
  PS_CHECK(direct.ok() &&
           std::holds_alternative<ResultPublication>(direct.value()));
  const auto& direct_output =
      std::get<ResultPublication>(direct.value()).result;
  PS_CHECK(direct_output.schema().tensors[0].facets[0].payload ==
           tensor.facets[0].payload);
  for (unsigned i = 0; i < 3; ++i) {
    float first = 1, second = 1;
    PS_CHECK(
        output.read_tensor(take(output.descriptor()), 0, {i}, &first, 4).ok());
    PS_CHECK(
        direct_output
            .read_tensor(take(direct_output.descriptor()), 0, {i}, &second, 4)
            .ok());
    PS_CHECK(first == 0 && second == first);
  }
  doc.nodes[0].parameters["count"] = INT64_C(5);
  GraphContext changed(doc);
  auto changed_plan = compiler.compile(changed);
  PS_CHECK(changed_plan.ok() && changed_plan.value().semantic.digest().value !=
                                    compiled.value().semantic.digest().value);
  doc.nodes[0].parameters["count"] = INT64_C(0);
  GraphContext empty(doc);
  PS_CHECK(!compiler.compile(empty).ok());
  auto repeated = definition("test.repeated", multi_result::schema());
  repeated.traits.input_schema.resize(1);
  repeated.traits.input_schema[0].kind = OperationPortKind::Result;
  repeated.traits.input_schema[0].rank = 2;
  repeated.traits.input_schema[0].element_type_mask = 12;
  repeated.traits.repeated_minimum = 1;
  repeated.traits.repeated_maximum = 4;
  repeated.traits.requires_metadata_specialization = true;
  repeated.prepare_static = [](const std::vector<OperationMetadata>& inputs,
                               const std::map<std::string, ParameterValue>&)
      -> Result<OperationPreparation> {
    auto shape = inputs[0].result_schema->tensors[0].sample_shape();
    shape.push_back(inputs.size());
    OperationPreparation result;
    result.outputs.resize(1);
    result.outputs[0].metadata = metadata(multi_result::schema(
        inputs[0].result_schema->tensors[0].descriptor.element_type, shape));
    return Result<OperationPreparation>(std::move(result));
  };
  OperationRegistry inference_registry;
  PS_CHECK(inference_registry.register_operation(repeated).ok());
  const auto field =
      metadata(multi_result::schema(ElementType::Float32, {2, 4}));
  for (unsigned count = 1; count <= 4; ++count) {
    std::vector<OperationMetadata> inputs(count, field);
    auto traits =
        inference_registry.resolve_traits("test.repeated", inputs, {});
    PS_CHECK(traits.ok() && traits.value().repeated_resolved == count);
    auto inferred = infer_operation_output(traits.value(), inputs, {});
    PS_CHECK(inferred.ok() &&
             inferred.value().result_schema->tensors[0].descriptor.shape ==
                 (std::vector<std::uint64_t>{2, 4, count}));
  }
  auto resolved = inference_registry.resolve_traits("test.repeated",
                                                    {field, field, field}, {});
  PS_CHECK(resolved.ok() && resolved.value().input_schema.size() == 3);
  auto result =
      infer_operation_output(resolved.value(), {field, field, field}, {});
  PS_CHECK(result.ok() &&
           result.value().result_schema->tensors[0].descriptor.shape ==
               (std::vector<std::uint64_t>{2, 4, 3}));
  const auto wrong =
      metadata(multi_result::schema(ElementType::Float32, {2, 5}));
  PS_CHECK(inference_registry
               .resolve_traits("test.repeated", {field, wrong, field}, {})
               .status()
               .code == ErrorCode::TypeMismatch);
  PS_CHECK(!inference_registry.resolve_traits("test.repeated", {}, {}).ok());
  PS_CHECK(!inference_registry
                .resolve_traits("test.repeated",
                                {field, field, field, field, field}, {})
                .ok());
  auto overflow_traits = resolved.value();
  auto& overflow_schema = *overflow_traits.outputs[0].result_schema;
  overflow_schema.fields.push_back(
      {"rows",
       ElementType::Float64,
       {ResultExtentKind::InputAxis, 1, 0, 0, 0, 1, UINT64_MAX},
       {}});
  PS_CHECK(infer_operation_output(overflow_traits, {field, field, field}, {})
               .status()
               .code == ErrorCode::ResourceExhausted);
  auto preserve =
      copy_definition("test.widen", *field.result_schema, false, true);
  PS_CHECK(inference_registry.register_operation(preserve).ok());
  auto independent =
      inference_registry.resolve_traits("test.widen", {field}, {});
  PS_CHECK(independent.ok() &&
           independent.value()
                   .outputs[0]
                   .result_schema->tensors[0]
                   .descriptor.shape ==
               field.result_schema->tensors[0].descriptor.shape &&
           independent.value()
                   .outputs[0]
                   .result_schema->tensors[0]
                   .descriptor.element_type == ElementType::Float64);
  auto image_schema = semantic_schema(rgba_semantics());
  auto image = copy_definition("test.image", image_schema);
  image.traits.parameter_schema = {
      {"semantic", OperationParameterType::String, true}};
  image.traits.outputs[0].output_schema.requires_semantics = true;
  image.traits.outputs[0].output_schema.facets = image_schema.tensors[0].facets;
  image.prepare_static =
      [image_schema](const std::vector<OperationMetadata>&,
                     const std::map<std::string, ParameterValue>& parameters)
      -> Result<OperationPreparation> {
    auto schema = image_schema;
    schema.id = "test.copy_samples";
    auto selected = semantic_from_parameter(
        std::get<std::string>(parameters.at("semantic")));
    if (!selected.ok())
      return Result<OperationPreparation>(selected.status());
    schema.tensors[0].facets = {take(encode_semantic(selected.value()))};
    OperationPreparation result;
    result.outputs.resize(1);
    result.outputs[0].metadata = metadata(std::move(schema));
    return Result<OperationPreparation>(std::move(result));
  };
  PS_CHECK(inference_registry.register_operation(image).ok());
  auto straight = rgba_semantics();
  straight.association = "straight";
  PS_CHECK(
      inference_registry
          .resolve_traits("test.image", {metadata(image_schema)},
                          {{"semantic", take(semantic_parameter(straight))}})
          .status()
          .code == ErrorCode::TypeMismatch);
  PS_CHECK(inference_registry.freeze().ok());
  auto borrowed = std::shared_ptr<OperationRegistry>(&inference_registry,
                                                     [](OperationRegistry*) {});
  WorkflowDocument widened;
  widened.inputs = {
      multi_result::declaration(1, "input", *field.result_schema)};
  widened.nodes = {{1, "test.widen", {WorkflowInputReference{1}}, {}}};
  widened.outputs = {{"output", 1, "value"}};
  GraphContext widen_graph(widened);
  auto widen_plan = Compiler(borrowed).compile(widen_graph);
  PS_CHECK(widen_plan.ok());
  ExecutionContext widened_execution(borrowed, config);
  const std::vector<float> input_samples{1, 2, 3, 4, -5, -6, -7, -8};
  auto widened_run = widened_execution.execute(
      widen_plan.value().plan,
      {{{"input", source(take(widened_execution.resource_budget()),
                         *field.result_schema, input_samples)}}});
  PS_CHECK(widened_run.ok());
  const auto& widened_result = widened_run.value().results.at("output");
  for (unsigned row = 0; row < 2; ++row)
    for (unsigned column = 0; column < 4; ++column) {
      double number = 0;
      PS_CHECK(widened_result
                   .read_tensor(take(widened_result.descriptor()), 0,
                                {row, column}, &number, sizeof(number))
                   .ok() &&
               number == input_samples[row * 4 + column]);
    }
  WorkflowDocument repeated_doc;
  std::vector<WorkflowInput> repeated_inputs;
  ExecutionBindings repeated_bindings;
  auto repeated_root = take(widened_execution.resource_budget());
  for (unsigned i = 0; i < 3; ++i) {
    const auto name = "input" + std::to_string(i);
    repeated_doc.inputs.push_back(
        multi_result::declaration(i + 1, name, *field.result_schema));
    repeated_inputs.push_back(WorkflowInputReference{i + 1});
    repeated_bindings.inputs.push_back(
        {name, source(repeated_root, *field.result_schema, input_samples)});
  }
  repeated_doc.nodes = {{1, "test.repeated", repeated_inputs, {}}};
  repeated_doc.outputs = {{"output", 1, "value"}};
  GraphContext repeated_graph(repeated_doc);
  auto repeated_plan = Compiler(borrowed).compile(repeated_graph);
  PS_CHECK(repeated_plan.ok());
  auto repeated_run =
      widened_execution.execute(repeated_plan.value().plan, repeated_bindings);
  PS_CHECK(repeated_run.ok());
  const auto& repeated_output = repeated_run.value().results.at("output");
  auto repeated_facts = take(repeated_output.descriptor());
  for (unsigned row = 0; row < 2; ++row)
    for (unsigned column = 0; column < 4; ++column)
      for (unsigned input = 0; input < 3; ++input) {
        float number = 1;
        PS_CHECK(repeated_output
                     .read_tensor(repeated_facts, 0, {row, column, input},
                                  &number, sizeof(number))
                     .ok() &&
                 number == 0);
      }
  return 0;
}
int explicit_drop() {
  for (const auto& semantic :
       {rgba_semantics(), coverage_semantics(), signal()}) {
    const auto schema = semantic_schema(semantic);
    auto preserve = copy_definition("preserve", schema);
    preserve.traits.input_schema[0].requires_semantics = true;
    preserve.traits.input_schema[0].semantic_kind =
        static_cast<std::uint32_t>(semantic.kind);
    preserve.traits.outputs[0].output_schema.requires_semantics = true;
    preserve.traits.outputs[0].output_schema.facets = schema.tensors[0].facets;
    auto drop = copy_definition("drop", schema, true);
    drop.traits.input_schema[0] = preserve.traits.input_schema[0];
    auto invalid = drop;
    invalid.key = "invalid.drop";
    invalid.traits.outputs[0].output_schema =
        preserve.traits.outputs[0].output_schema;
    OperationRegistry checked;
    PS_CHECK(checked.register_operation(invalid).ok());
    PS_CHECK(checked.resolve_traits(invalid.key, {metadata(schema)}, {})
                 .status()
                 .code == ErrorCode::TypeMismatch);
    auto registry = std::make_shared<OperationRegistry>();
    PS_CHECK(registry->register_operation(preserve).ok());
    PS_CHECK(registry->register_operation(drop).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {multi_result::declaration(1, "input", schema)};
    doc.nodes = {{1, "drop", {WorkflowInputReference{1}}, {}},
                 {2, "preserve", {WorkflowNodeOutput{1, "value"}}, {}}};
    doc.outputs = {{"output", 2, "value"}};
    GraphContext rejected_graph(doc);
    PS_CHECK(Compiler(registry).compile(rejected_graph).status().code ==
             ErrorCode::TypeMismatch);
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext execution(registry, config);
    const auto samples = semantic.kind == SemanticKind::Image
                             ? std::vector<float>{-2, 4, .25F, .5F}
                         : semantic.kind == SemanticKind::Mask
                             ? std::vector<float>{.5F}
                             : std::vector<float>{1, 2, 3};
    auto input = source(take(execution.resource_budget()), schema, samples);
    for (const auto* key : {"drop", "preserve"}) {
      doc.nodes = {{1, key, {WorkflowInputReference{1}}, {}}};
      doc.outputs = {{"output", 1, "value"}};
      GraphContext graph(doc);
      auto compiled = Compiler(registry).compile(graph);
      PS_CHECK(compiled.ok());
      auto run = execution.execute(compiled.value().plan, {{{"input", input}}});
      PS_CHECK(run.ok());
      const auto& output = run.value().results.at("output");
      const auto& tensor = output.schema().tensors[0];
      PS_CHECK(std::string(key) == "drop"
                   ? tensor.facets.empty()
                   : tensor.facets[0].payload ==
                         schema.tensors[0].facets[0].payload);
      auto at = std::vector<std::uint64_t>(tensor.descriptor.shape.size(), 0);
      for (unsigned i = 0; i < samples.size(); ++i) {
        float value = 0;
        at.back() = i;
        PS_CHECK(output.read_tensor(take(output.descriptor()), 0, at, &value, 4)
                     .ok() &&
                 value == samples[i]);
      }
    }
  }
  return 0;
}
int dtype_masks() {
  auto definition = copy_definition("test.floating", multi_result::schema());
  OperationRegistry malformed;
  definition.traits.input_schema[0].element_type_mask = 1U << 7;
  PS_CHECK(!malformed.register_operation(definition).ok());
  definition.traits.input_schema[0].element_type_mask = 12;
  definition.traits.input_schema[0].element_type = 3;
  PS_CHECK(!malformed.register_operation(definition).ok() &&
           malformed.keys().empty());
  definition.traits.input_schema[0].element_type = 0;
  std::vector<std::string> digests;
  for (const auto mask : {4U, 12U}) {
    definition.traits.input_schema[0].element_type_mask = mask;
    auto registry = std::make_shared<OperationRegistry>();
    PS_CHECK(registry->register_operation(definition).ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {multi_result::declaration(1, "a")};
    doc.nodes = {{1, definition.key, {WorkflowInputReference{1}}, {}}};
    doc.outputs = {{"result", 1, "value"}};
    GraphContext graph(doc);
    Compiler compiler(registry);
    auto plan = compiler.compile(graph);
    PS_CHECK(plan.ok());
    digests.push_back(plan.value().semantic.digest().value);
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto run = context.execute(
        plan.value().plan,
        {{multi_result::binding(take(context.resource_budget()), "a", 4)}});
    PS_CHECK(run.ok() &&
             multi_result::number(run.value().results.at("result")) == 4);
    auto changed = doc;
    changed.inputs[0].result_schema = std::make_shared<SchemaTemplate>(
        multi_result::schema(ElementType::Float32));
    GraphContext fp32(changed);
    auto compiled = compiler.compile(fp32);
    PS_CHECK(mask == 4 ? compiled.status().code == ErrorCode::TypeMismatch
                       : compiled.ok());
  }
  PS_CHECK(digests[0] != digests[1]);
  return 0;
}
int builtin_identity() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  auto bytes = Value::create({ElementType::UInt8, {2}}, Region::whole({2}),
                             {0, {1}}, {3, 7}, {{"opaque", 1, {9}}})
                   .take_value();
  auto root = take(execution.resource_budget());
  auto byte_schema = multi_result::schema(ElementType::UInt8, {2});
  byte_schema.tensors[0].facets = bytes.facets();
  auto builder =
      take(ResultBuilder::start(root, byte_schema, "identity.source"));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(root, 1, {}))));
  check(builder.publish_tensor(
      0, bytes.region(), bytes.layout(), bytes.storage(),
      take(ResultRelation::cartesian(root, 2, {})), {true, true, true, true}));
  const std::vector<ResultRef> inputs{
      take(builder.seal()),
      source(root, semantic_schema(rgba_semantics()), {-2, 4, .25F, .5F}),
      source(root, semantic_schema(coverage_semantics()), {.5F}),
      source(root, semantic_schema(signal()), {1, 2, 3})};
  for (const auto& input : inputs) {
    for (const auto* key :
         {"core.identity", "core.delay", "core.gpu_fallback_probe"}) {
      WorkflowDocument doc;
      doc.inputs = {multi_result::declaration(1, "input", input.schema())};
      std::map<std::string, ParameterValue> parameters;
      if (std::string(key) == "core.delay")
        parameters["milliseconds"] = INT64_C(0);
      doc.nodes = {{1, key, {WorkflowInputReference{1}}, parameters}};
      doc.outputs = {{"output", 1, "value"}};
      GraphContext graph(doc);
      auto compiled = compiler.compile(graph);
      PS_CHECK(compiled.ok());
      auto run = execution.execute(compiled.value().plan, {{{"input", input}}});
      PS_CHECK(run.ok());
      const auto& output = run.value().results.at("output");
      PS_CHECK(output.schema().tensors[0].descriptor.element_type ==
               input.schema().tensors[0].descriptor.element_type);
      PS_CHECK(output.schema().tensors[0].facets[0].payload ==
               input.schema().tensors[0].facets[0].payload);
      const auto shape = input.schema().tensors[0].sample_shape();
      auto coverage = take(Footprint::all(shape));
      const auto input_facts = take(input.descriptor());
      const auto output_facts = take(output.descriptor());
      const auto width = Value::element_size(
          input.schema().tensors[0].descriptor.element_type);
      auto compared = coverage.visit(
          [&](const auto& at) {
            std::uint8_t expected[8]{}, actual[8]{};
            auto status =
                input.read_tensor(input_facts, 0, at, expected, width);
            if (status.ok())
              status = output.read_tensor(output_facts, 0, at, actual, width);
            if (!status.ok())
              return status;
            return std::memcmp(expected, actual, width) == 0
                       ? Status::success()
                       : Status{ErrorCode::OperationFailed,
                                "identity changed sample bits"};
          },
          take(coverage.element_count()));
      PS_CHECK(compared.ok());
    }
  }
  WorkflowDocument bad;
  WorkflowInputDeclaration bad_input;
  bad_input.id = 1;
  bad_input.name = "input";
  SchemaTemplate bad_schema;
  bad_schema.id = "photospider.tensor";
  ResultTensorSpec bad_tensor;
  bad_tensor.key = "samples";
  bad_tensor.descriptor = bytes.descriptor();
  bad_tensor.facets = bytes.facets();
  bad_schema.tensors.push_back(bad_tensor);
  bad_input.result_schema = std::make_shared<SchemaTemplate>(bad_schema);
  bad.inputs = {bad_input};
  bad.nodes = {{1,
                "math.add",
                {WorkflowInputReference{1}, WorkflowInputReference{1}},
                {}}};
  bad.outputs = {{"output", 1, "value"}};
  GraphContext bad_graph(bad);
  PS_CHECK(compiler.compile(bad_graph).status().code ==
           ErrorCode::TypeMismatch);
  return 0;
}
int c_contract() {
#ifdef PS_CONTRACT_FIXTURE
  auto registry = std::make_shared<OperationRegistry>();
  auto loaded = registry->load_plugin(PS_CONTRACT_FIXTURE);
  if (!loaded.ok())
    std::cerr << loaded.message << "\n";
  PS_CHECK(loaded.ok());
  PS_CHECK(registry->freeze().ok());
  Compiler compiler(registry);
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(registry, config);
  auto root = execution.resource_budget().take_value();
  const auto facet = encode_semantic(signal()).take_value();
  const auto parameters = std::map<std::string, ParameterValue>{
      {"dtype", std::string("float32")},
      {"semantic", semantic_parameter(signal()).take_value()}};
  const auto document = [&](unsigned count, ElementType type,
                            std::uint64_t length = 1) {
    WorkflowDocument doc;
    std::vector<WorkflowInput> inputs;
    for (unsigned i = 0; i < count; ++i) {
      doc.inputs.push_back(
          multi_result::declaration(i + 1, "input" + std::to_string(i),
                                    multi_result::schema(type, {length})));
      inputs.push_back(WorkflowInputReference{i + 1});
    }
    doc.nodes = {{1, "fixture.contract", std::move(inputs), parameters}};
    doc.outputs = {{"output", 1, "value"}};
    return doc;
  };
  const auto source = [&](const WorkflowInputDeclaration& declaration,
                          double value, unsigned layout) {
    const auto& schema = *declaration.result_schema;
    const auto& tensor = schema.tensors[0];
    const auto length = tensor.descriptor.shape[0];
    const auto width = Value::element_size(tensor.descriptor.element_type);
    const bool zero = layout == 2;
    const bool sparse = layout == 3;
    const auto stored = zero || sparse ? 1 : length;
    std::vector<std::uint8_t> bytes(stored * width);
    for (std::uint64_t i = 0; i < stored; ++i)
      multi_result::write_number(
          bytes.data() + (layout == 1 ? length - 1 - i : i) * width,
          tensor.descriptor.element_type, value + i);
    StridedLayout strides;
    strides.byte_offset = layout == 1 ? (length - 1) * width : 0;
    strides.byte_strides = {zero          ? 0
                            : layout == 1 ? -static_cast<std::int64_t>(width)
                                          : static_cast<std::int64_t>(width)};
    auto backing =
        Value::create(tensor.descriptor,
                      sparse ? Region({{0, 1}}) : Region::whole({length}),
                      strides, std::move(bytes))
            .take_value();
    auto builder =
        ResultBuilder::start(root, schema, "contract.source").take_value();
    multi_result::check(builder.bind_descriptor_relation(
        ResultRelation::cartesian(root, 1, {}).take_value()));
    multi_result::check(builder.publish_tensor(
        0, backing.region(), backing.layout(), backing.storage(),
        ResultRelation::cartesian(root, length, {}).take_value(),
        {true, true, true, true}));
    ExecutionBinding binding;
    binding.name = declaration.name;
    binding.result = builder.seal().take_value();
    return binding;
  };
  for (const auto input_type : {ElementType::Float32, ElementType::Float64}) {
    for (const auto output_type :
         {ElementType::Float32, ElementType::Float64}) {
      for (unsigned count : {1U, 2U, 3U, 4U}) {
        for (unsigned layout = 0; layout < 3; ++layout) {
          auto doc = document(count, input_type, 3);
          doc.nodes[0].parameters["dtype"] = std::string(
              output_type == ElementType::Float32 ? "float32" : "float64");
          GraphContext graph(doc);
          auto compiled = compiler.compile(graph);
          PS_CHECK(compiled.ok());
          PS_CHECK(compiled.value().plan.steps()[0].traits.repeated_resolved ==
                   count);
          const auto& metadata =
              *compiled.value().semantic.nodes()[0].outputs[0].result_schema;
          PS_CHECK(metadata.tensors[0].descriptor.shape ==
                   std::vector<std::uint64_t>{count});
          PS_CHECK(metadata.tensors[0].descriptor.element_type == output_type);
          PS_CHECK(metadata.tensors[0].facets[0].payload == facet.payload);
          ExecutionBindings bindings;
          for (unsigned i = 0; i < count; ++i)
            bindings.inputs.push_back(
                source(doc.inputs[i], i == 0 ? 2 : -3, layout));
          auto run = execution.execute(compiled.value().plan, bindings);
          PS_CHECK(run.ok());
          const auto& output = run.value().results.at("output");
          PS_CHECK(output.schema().tensors[0].facets[0].payload ==
                   facet.payload);
          auto facts = output.descriptor().take_value();
          for (unsigned i = 0; i < count; ++i) {
            double number = 0;
            if (output_type == ElementType::Float32) {
              float fp32 = 0;
              PS_CHECK(output.read_tensor(facts, 0, {i}, &fp32, 4).ok());
              number = fp32;
            } else {
              PS_CHECK(output.read_tensor(facts, 0, {i}, &number, 8).ok());
            }
            PS_CHECK(number == (i == 0 ? 2 : -3));
            const auto changed =
                Footprint::from_regions({3}, {Region({{0, 1}})}).take_value();
            auto dirty = run.value().dependencies.potential_dirty(
                doc.inputs[i].name, changed, 1, {}, ResultSupportTarget::Tensor,
                0);
            PS_CHECK(dirty.ok() &&
                     dirty.value().at("output") ==
                         Footprint::from_regions({count}, {Region({{i, 1}})})
                             .take_value());
            auto unrelated = run.value().dependencies.potential_dirty(
                doc.inputs[i].name,
                Footprint::from_regions({3}, {Region({{1, 1}})}).take_value(),
                1, {}, ResultSupportTarget::Tensor, 0);
            PS_CHECK(unrelated.ok() && unrelated.value().at("output").empty());
          }
        }
      }
    }
  }
  auto halves = document(2, ElementType::Float32);
  GraphContext halves_graph(halves);
  auto half_plan = compiler.compile(halves_graph).take_value();
  auto half_run = execution.execute(
      half_plan.plan,
      {{source(halves.inputs[0], .5, 0), source(halves.inputs[1], .5, 0)}});
  PS_CHECK(half_run.ok());
  auto half_output = half_run.value().results.at("output");
  for (unsigned i = 0; i < 2; ++i) {
    float value = 0;
    PS_CHECK(half_output
                 .read_tensor(half_output.descriptor().take_value(), 0, {i},
                              &value, 4)
                 .ok() &&
             value == .5F);
  }
  auto large = document(2, ElementType::Float64);
  large.nodes[0].parameters["dtype"] = std::string("float64");
  GraphContext large_graph(large);
  auto large_plan = compiler.compile(large_graph).take_value();
  auto large_run = execution.execute(large_plan.plan,
                                     {{source(large.inputs[0], 1e100, 0),
                                       source(large.inputs[1], -1e100, 0)}});
  PS_CHECK(large_run.ok());
  for (unsigned i = 0; i < 2; ++i) {
    const auto& output = large_run.value().results.at("output");
    double value = 0;
    PS_CHECK(
        output.read_tensor(output.descriptor().take_value(), 0, {i}, &value, 8)
            .ok() &&
        value == (i ? -1e100 : 1e100));
  }
  ExecutionBindings half_bindings{
      {source(halves.inputs[0], .5, 0), source(halves.inputs[1], .5, 0)}};
  auto frozen = execution.freeze(half_plan.plan, half_bindings).take_value();
  auto empty = execution.execute_fragments(
      frozen, {{"output", Footprint::from_regions({2}, {}).take_value()}});
  PS_CHECK(empty.ok());
  const auto& empty_output = empty.value().results.at("output");
  auto empty_facts = empty_output.descriptor().take_value();
  PS_CHECK(empty_facts.tensor_coverage(0).empty());
  float unavailable = 0;
  PS_CHECK(
      !empty_output.read_tensor(empty_facts, 0, {0}, &unavailable, 4).ok());
  CancellationSource stopped;
  PS_CHECK(stopped.cancel());
  PS_CHECK(execution.execute(half_plan.plan, half_bindings, stopped.token())
               .status()
               .code == ErrorCode::Cancelled);
  auto huge = document(2, ElementType::Float64, UINT64_C(1) << 61);
  GraphContext huge_graph(huge);
  auto huge_plan = compiler.compile(huge_graph);
  PS_CHECK(huge_plan.ok());
  auto huge_run = execution.execute(
      huge_plan.value().plan,
      {{source(huge.inputs[0], 2, 3), source(huge.inputs[1], -3, 3)}});
  PS_CHECK(huge_run.ok());
  for (unsigned i = 0; i < 2; ++i) {
    float value = 0;
    const auto& output = huge_run.value().results.at("output");
    PS_CHECK(
        output.read_tensor(output.descriptor().take_value(), 0, {i}, &value, 4)
            .ok() &&
        value == (i ? -3 : 2));
  }
  auto shifted = signal();
  shifted.sample_origin = 2;
  shifted.sample_step = 1;
  auto changed_semantic = halves;
  changed_semantic.nodes[0].parameters["semantic"] =
      semantic_parameter(shifted).take_value();
  GraphContext shifted_graph(changed_semantic);
  auto shifted_plan = compiler.compile(shifted_graph);
  PS_CHECK(shifted_plan.ok() && shifted_plan.value().semantic.digest().value !=
                                    half_plan.semantic.digest().value);
  const auto& shifted_schema =
      *shifted_plan.value().semantic.nodes()[0].outputs[0].result_schema;
  PS_CHECK(shifted_schema.tensors[0].facets[0].payload ==
           encode_semantic(shifted).take_value().payload);
  PS_CHECK(half_output.schema().tensors[0].facets[0].payload == facet.payload);
  for (unsigned count : {0U, 5U}) {
    GraphContext graph(document(count, ElementType::Float64));
    PS_CHECK(compiler.compile(graph).status().code == ErrorCode::TypeMismatch);
  }
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto doc = document(2, ElementType::Float64);
    auto schema = *doc.inputs[1].result_schema;
    if (bad == 0)
      schema.tensors[0].descriptor.element_type = ElementType::Int64;
    else if (bad == 1)
      schema.tensors[0].descriptor.element_type = ElementType::Float32;
    else if (bad == 2)
      schema.tensors[0].descriptor.shape = {2};
    else
      schema.tensors[0].descriptor.shape = {1, 1};
    doc.inputs[1].result_schema = std::make_shared<SchemaTemplate>(schema);
    GraphContext graph(doc);
    PS_CHECK(compiler.compile(graph).status().code == ErrorCode::TypeMismatch);
  }
  for (const auto& semantic : {std::string("0"), std::string("FF"),
                               std::string("00"), std::string(8194, '0')}) {
    auto doc = document(1, ElementType::Float64);
    doc.nodes[0].parameters["semantic"] = semantic;
    GraphContext graph(doc);
    PS_CHECK(!compiler.compile(graph).ok());
  }
  for (const auto& dtype : {std::string("int64"), std::string("FLOAT32")}) {
    auto doc = document(1, ElementType::Float64);
    doc.nodes[0].parameters["dtype"] = dtype;
    GraphContext graph(doc);
    PS_CHECK(compiler.compile(graph).status().code ==
             ErrorCode::InvalidArgument);
  }
#ifdef PS_BAD_CONTRACT_1
  for (const auto* path :
       {PS_BAD_CONTRACT_1, PS_BAD_CONTRACT_2, PS_BAD_CONTRACT_3,
        PS_BAD_CONTRACT_4, PS_BAD_CONTRACT_5, PS_BAD_CONTRACT_6,
        PS_BAD_CONTRACT_7, PS_BAD_CONTRACT_8, PS_BAD_CONTRACT_9,
        PS_BAD_CONTRACT_10, PS_BAD_CONTRACT_11, PS_BAD_CONTRACT_12}) {
    OperationRegistry malformed;
    auto rejected = malformed.load_plugin(path);
    PS_CHECK(rejected.code == ErrorCode::InvalidArgument);
    PS_CHECK(malformed.keys().empty());
  }
#endif
#endif
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(explicit_drop() == 0);
  PS_CHECK(codec() == 0);
  PS_CHECK(inference() == 0);
  PS_CHECK(c_contract() == 0);
  PS_CHECK(builtin_identity() == 0);
  PS_CHECK(dtype_masks() == 0);
  return 0;
}
