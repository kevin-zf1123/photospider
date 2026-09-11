#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/semantic.hpp"
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
  const auto value = [&](const SemanticDescriptor& s, float a) {
    float samples[] = {-2, 4, .25F, a};
    std::vector<std::uint8_t> bytes(sizeof(samples));
    std::memcpy(bytes.data(), samples, sizeof(samples));
    return Value::create({ElementType::Float32, {1, 1, 4}},
                         Region::whole({1, 1, 4}), {0, {16, 16, 4}}, bytes,
                         {encode_semantic(s).take_value()})
        .take_value();
  };
  PS_CHECK(validate_semantic_value(alpha, value(alpha, .5F)).ok());
  PS_CHECK(!validate_semantic_value(alpha, value(alpha, 0)).ok());
  alpha.association = "straight";
  PS_CHECK(validate_semantic_value(alpha, value(alpha, 0)).ok());
  PS_CHECK(!validate_semantic_value(alpha, value(alpha, 2)).ok());
  PS_CHECK(validate_semantic_value(alpha, value(alpha, 0),
                                   ErrorCode::OperationFailed,
                                   [] { return ErrorCode::Cancelled; })
               .code == ErrorCode::Cancelled);
  return 0;
}
OperationDefinition generator() {
  OperationDefinition d;
  d.key = "test.generate";
  auto& t = d.traits;
  t.outputs[0].shape_rule = OperationShapeRule::Axes;
  t.outputs[0].output_axes = {
      {OperationExtentSource::Parameter, 1, "count", 0, 0, 0}};
  t.outputs[0].output_dtype_rule = OperationDtypeRule::Parameter;
  t.outputs[0].output_dtype_parameter = "dtype";
  t.outputs[0].output_semantic_rule = OperationSemanticRule::Parameter;
  t.outputs[0].output_semantic_parameter = "semantic";
  t.parameter_schema = {{"count", OperationParameterType::Int64, true},
                        {"dtype", OperationParameterType::String, true},
                        {"semantic", OperationParameterType::String, true}};
  d.callback = [t](const OperationInvocation& call) -> Result<Value> {
    auto traits = resolve_operation_traits(t, 0, call.parameters);
    if (!traits.ok())
      return Result<Value>(traits.status());
    auto output = infer_operation_output(traits.value(), {}, call.parameters);
    if (!output.ok())
      return Result<Value>(output.status());
    auto writer = MutableValue::allocate(output.value().descriptor,
                                         call.output_region, call.allocator);
    if (!writer.ok())
      return Result<Value>(writer.status());
    auto value = writer.take_value();
    std::memset(value.data(), 0, value.size());
    return std::move(value).publish(output.value().facets);
  };
  return d;
}
int inference() {
  auto registry = std::make_shared<OperationRegistry>();
  auto d = generator();
  PS_CHECK(registry->register_operation(d).ok());
  auto bad = d;
  bad.key = "bad";
  bad.traits.outputs[0].output_axes[0].parameter = "absent";
  PS_CHECK(!registry->register_operation(bad).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  doc.nodes = {{1,
                d.key,
                {},
                {{"count", INT64_C(3)},
                 {"dtype", std::string("float32")},
                 {"semantic", semantic_parameter(signal()).take_value()}}}};
  doc.outputs = {{"output", 1, "value"}};
  GraphContext graph(doc);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  const auto& node = compiled.value().semantic.nodes()[0];
  PS_CHECK(node.output_descriptor.shape == std::vector<std::uint64_t>{3});
  PS_CHECK(node.output_descriptor.element_type == ElementType::Float32);
  PS_CHECK(!node.output_facets.empty());
  ExecutionContext execution(registry);
  auto run = execution.execute(compiled.value().plan);
  PS_CHECK(run.ok());
  PS_CHECK(run.value().values.at("output").facets()[0].payload ==
           node.output_facets[0].payload);
  const std::vector<Value> inputs;
  const std::vector<Region> demands;
  auto direct =
      registry->invoke(d.key, {inputs, demands, doc.nodes[0].parameters});
  PS_CHECK(direct.ok() && direct.value().copy_bytes() ==
                              run.value().values.at("output").copy_bytes());
  doc.nodes[0].parameters["count"] = INT64_C(5);
  GraphContext changed(doc);
  auto changed_plan = compiler.compile(changed);
  PS_CHECK(changed_plan.ok() && changed_plan.value().semantic.digest().value !=
                                    compiled.value().semantic.digest().value);
  doc.nodes[0].parameters["count"] = INT64_C(0);
  GraphContext empty(doc);
  PS_CHECK(!compiler.compile(empty).ok());
  OperationTraits repeated;
  repeated.input_schema = {OperationPortConstraint{}};
  repeated.input_schema[0].rank = 2;
  repeated.repeated_minimum = 1;
  repeated.repeated_maximum = 4;
  repeated.outputs[0].shape_rule = OperationShapeRule::Axes;
  repeated.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  repeated.outputs[0].output_axes = {
      {OperationExtentSource::InputAxis, 1, {}, 0, 0, 0},
      {OperationExtentSource::InputAxis, 1, {}, 0, 1, 0},
      {OperationExtentSource::InputCount, 1, {}, 0, 0, 0}};
  auto resolved = resolve_operation_traits(repeated, 3, {});
  PS_CHECK(resolved.ok() && resolved.value().input_schema.size() == 3);
  const OperationMetadata field{{ElementType::Float32, {2, 4}}, {}};
  auto result =
      infer_operation_output(resolved.value(), {field, field, field}, {});
  PS_CHECK(result.ok() && result.value().descriptor.shape ==
                              (std::vector<std::uint64_t>{2, 4, 3}));
  auto wrong = field;
  wrong.descriptor.shape[1] = 5;
  PS_CHECK(!infer_operation_output(resolved.value(), {field, wrong, field}, {})
                .ok());
  PS_CHECK(!resolve_operation_traits(repeated, 0, {}).ok());
  PS_CHECK(!resolve_operation_traits(repeated, 5, {}).ok());
  repeated.outputs[0].output_axes[0].offset = UINT64_MAX;
  auto overflow = resolve_operation_traits(repeated, 1, {}).take_value();
  PS_CHECK(infer_operation_output(overflow, {field}, {}).status().code ==
           ErrorCode::ResourceExhausted);
  auto preserve = repeated;
  preserve.repeated_minimum = preserve.repeated_maximum = 0;
  preserve.input_count = 1;
  preserve.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  preserve.outputs[0].output_axes.clear();
  preserve.outputs[0].output_dtype_rule = OperationDtypeRule::Declared;
  preserve.outputs[0].output_element_type = ElementType::Float64;
  auto independent = infer_operation_output(preserve, {field}, {});
  PS_CHECK(independent.ok() &&
           independent.value().descriptor.element_type == ElementType::Float64);
  OperationTraits image;
  image.input_count = 1;
  image.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}};
  image.outputs[0].output_schema = image.input_schema[0];
  image.outputs[0].output_element_type = ElementType::Float32;
  image.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  image.outputs[0].output_semantic_rule = OperationSemanticRule::Parameter;
  image.outputs[0].output_semantic_parameter = "semantic";
  image.parameter_schema = {{"semantic", OperationParameterType::String, true}};
  auto straight = rgba_semantics();
  straight.association = "straight";
  const std::map<std::string, ParameterValue> contradictory{
      {"semantic", semantic_parameter(straight).take_value()}};
  auto image_resolved = resolve_operation_traits(image, 1, contradictory);
  PS_CHECK(image_resolved.ok());
  const OperationMetadata rgba{
      {ElementType::Float32, {1, 1, 4}},
      {encode_semantic(rgba_semantics()).take_value()}};
  PS_CHECK(infer_operation_output(image_resolved.value(), {rgba}, contradictory)
               .status()
               .code == ErrorCode::TypeMismatch);
  return 0;
}
int explicit_drop() {
  for (const auto kind :
       {OperationPortKind::RgbaFloat32, OperationPortKind::Float32Mask,
        OperationPortKind::Typed}) {
    const bool mask = kind == OperationPortKind::Float32Mask;
    const auto semantic = mask ? coverage_semantics() : rgba_semantics();
    OperationMetadata input{
        {ElementType::Float32, mask ? std::vector<std::uint64_t>{1, 1}
                                    : std::vector<std::uint64_t>{1, 1, 4}},
        {encode_semantic(semantic).take_value()}};
    OperationTraits traits;
    traits.input_count = 1;
    traits.input_schema.resize(1);
    traits.outputs[0].output_element_type = ElementType::Float32;
    traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
    traits.input_schema[0].kind = kind;
    traits.outputs[0].output_schema.kind = kind;
    traits.outputs[0].output_semantic_rule = OperationSemanticRule::Drop;
    PS_CHECK(infer_operation_output(traits, {input}, {}).status().code ==
             ErrorCode::TypeMismatch);
    OperationRegistry invalid;
    auto rejected = invalid.register_operation(
        {"drop", traits, [](const OperationInvocation& call) {
           return Result<Value>(call.inputs[0]);
         }});
    PS_CHECK(rejected.code == ErrorCode::InvalidArgument);
    PS_CHECK(invalid.keys().empty());
    auto preserved = traits;
    preserved.outputs[0].output_semantic_rule =
        OperationSemanticRule::PreserveInput;
    PS_CHECK(invalid
                 .register_operation({"preserve", preserved,
                                      [](const OperationInvocation& call) {
                                        return Result<Value>(call.inputs[0]);
                                      }})
                 .ok());
    traits.outputs[0].output_schema = {};
    auto dropped = infer_operation_output(traits, {input}, {});
    PS_CHECK(dropped.ok() && dropped.value().facets.empty());
    auto registry = std::make_shared<OperationRegistry>();
    const auto callback = [](const OperationInvocation& call) {
      return Result<Value>(call.inputs[0]);
    };
    PS_CHECK(registry->register_operation({"drop", traits, callback}).ok());
    traits.input_schema[0].kind = kind;
    traits.outputs[0].output_semantic_rule =
        OperationSemanticRule::PreserveInput;
    PS_CHECK(registry->register_operation({"typed", traits, callback}).ok());
    PS_CHECK(registry->freeze().ok());
    auto typed =
        Value::create(
            input.descriptor, Region::whole(input.descriptor.shape),
            mask ? StridedLayout{0, {4, 4}} : StridedLayout{0, {16, 16, 4}},
            std::vector<std::uint8_t>(mask ? 4 : 16), input.facets)
            .take_value();
    WorkflowDocument doc;
    doc.inputs = {{1, "input", typed.descriptor(), typed.region(),
                   typed.layout(), typed.facets()}};
    doc.nodes = {{1, "drop", {WorkflowInputReference{1}}, {}},
                 {2, "typed", {WorkflowNodeOutput{1, "value"}}, {}}};
    doc.outputs = {{"output", 2, "value"}};
    GraphContext graph(doc);
    Compiler compiler(registry);
    PS_CHECK(compiler.compile(graph).status().code == ErrorCode::TypeMismatch);
  }
  return 0;
}
int dtype_masks() {
  OperationDefinition definition;
  definition.key = "test.floating";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  definition.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  definition.traits.outputs[0].output_dtype_rule = OperationDtypeRule::Input;
  definition.callback = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  OperationRegistry malformed;
  definition.traits.input_schema[0].element_type_mask = 16;
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
    doc.inputs = {{1,
                   "a",
                   {ElementType::Float64, {1}},
                   Region::whole({1}),
                   {0, {8}},
                   {}}};
    doc.nodes = {{1, definition.key, {WorkflowInputReference{1}}, {}}};
    doc.outputs = {{"result", 1, "value"}};
    GraphContext graph(doc);
    Compiler compiler(registry);
    auto plan = compiler.compile(graph);
    PS_CHECK(plan.ok());
    digests.push_back(plan.value().semantic.digest().value);
  }
  PS_CHECK(digests[0] != digests[1]);
  return 0;
}
int builtin_identity() {
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  auto coverage = encode_semantic(coverage_semantics()).take_value();
  auto mask =
      Value::create({ElementType::Float32, {1, 1}}, Region::whole({1, 1}),
                    {0, {4, 4}}, std::vector<std::uint8_t>(4), {coverage})
          .take_value();
  auto bytes = Value::create({ElementType::UInt8, {2}}, Region::whole({2}),
                             {0, {1}}, {3, 7}, {{"opaque", 1, {9}}})
                   .take_value();
  for (const auto& value : {mask, bytes}) {
    for (const auto* key :
         {"core.identity", "core.delay", "core.gpu_fallback_probe"}) {
      std::map<std::string, ParameterValue> parameters;
      if (std::string(key) == "core.delay")
        parameters["milliseconds"] = INT64_C(0);
      WorkflowDocument doc;
      doc.inputs = {{1, "input", value.descriptor(), value.region(),
                     value.layout(), value.facets()}};
      doc.nodes = {{1, key, {WorkflowInputReference{1}}, parameters}};
      doc.outputs = {{"output", 1, "value"}};
      GraphContext graph(doc);
      auto compiled = compiler.compile(graph);
      PS_CHECK(compiled.ok());
      auto result =
          execution.execute(compiled.value().plan, {{{"input", value}}});
      PS_CHECK(result.ok());
      const auto& output = result.value().values.at("output");
      PS_CHECK(output.descriptor().element_type ==
               value.descriptor().element_type);
      PS_CHECK(output.copy_bytes() == value.copy_bytes());
      PS_CHECK(output.facets()[0].payload == value.facets()[0].payload);
      const std::vector<Value> inputs{value};
      const std::vector<Region> demands{value.region()};
      auto direct = registry->invoke(key, {inputs, demands, parameters});
      PS_CHECK(direct.ok() &&
               direct.value().copy_bytes() == value.copy_bytes());
    }
  }
  WorkflowDocument bad;
  bad.inputs = {{1, "input", bytes.descriptor(), bytes.region(), bytes.layout(),
                 bytes.facets()}};
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
  PS_CHECK(registry->load_plugin(PS_CONTRACT_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument doc;
  const auto parameters = std::map<std::string, ParameterValue>{
      {"dtype", std::string("float32")},
      {"semantic", semantic_parameter(signal()).take_value()}};
  doc.inputs = {
      {1, "a", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}},
      {2, "b", {ElementType::Float64, {1}}, Region::whole({1}), {0, {8}}, {}}};
  doc.nodes = {{1,
                "fixture.contract",
                {WorkflowInputReference{1}, WorkflowInputReference{2}},
                parameters}};
  doc.outputs = {{"output", 1, "value"}};
  GraphContext graph(doc);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  PS_CHECK(compiled.value().plan.steps()[0].traits.repeated_resolved == 2);
  ExecutionContext execution(registry);
  auto run = execution.execute(
      compiled.value().plan,
      {{{"a", Value::from_float64(2)}, {"b", Value::from_float64(-3)}}});
  PS_CHECK(run.ok());
  float values[2];
  std::memcpy(values, run.value().values.at("output").bytes().data(),
              sizeof(values));
  PS_CHECK(values[0] == 2 && values[1] == -3);
  PS_CHECK(decode_semantic(run.value().values.at("output").facets()[0]).ok());
  for (auto& input : doc.inputs) {
    input.descriptor.element_type = ElementType::Float32;
    input.layout.byte_strides = {4};
  }
  GraphContext fp32_graph(doc);
  auto fp32_plan = compiler.compile(fp32_graph);
  PS_CHECK(fp32_plan.ok());
  const float half = .5F;
  std::vector<std::uint8_t> raw(4);
  std::memcpy(raw.data(), &half, 4);
  auto scalar = Value::create({ElementType::Float32, {1}}, Region::whole({1}),
                              {0, {4}}, raw)
                    .take_value();
  auto fp32_run = execution.execute(fp32_plan.value().plan,
                                    {{{"a", scalar}, {"b", scalar}}});
  PS_CHECK(fp32_run.ok());
  std::memcpy(values, fp32_run.value().values.at("output").bytes().data(),
              sizeof(values));
  PS_CHECK(values[0] == .5F && values[1] == .5F);
  doc.inputs[0].descriptor.element_type = ElementType::Int64;
  doc.inputs[0].layout.byte_strides = {8};
  GraphContext integer_graph(doc);
  PS_CHECK(compiler.compile(integer_graph).status().code ==
           ErrorCode::TypeMismatch);

#ifdef PS_BAD_CONTRACT_1
  for (const auto* path :
       {PS_BAD_CONTRACT_1, PS_BAD_CONTRACT_2, PS_BAD_CONTRACT_3,
        PS_BAD_CONTRACT_4, PS_BAD_CONTRACT_5, PS_BAD_CONTRACT_6,
        PS_BAD_CONTRACT_8, PS_BAD_CONTRACT_9}) {
    OperationRegistry malformed;
    PS_CHECK(!malformed.load_plugin(path).ok());
    PS_CHECK(malformed.keys().empty());
  }
  for (const auto* path :
       {PS_BAD_CONTRACT_10, PS_BAD_CONTRACT_11, PS_BAD_CONTRACT_12}) {
    OperationRegistry dropped;
    auto rejected = dropped.load_plugin(path);
    PS_CHECK(rejected.code == ErrorCode::InvalidArgument);
    PS_CHECK(dropped.keys().empty());
  }
  auto wide = std::make_shared<OperationRegistry>();
  PS_CHECK(wide->load_plugin(PS_BAD_CONTRACT_7).ok());
  PS_CHECK(wide->freeze().ok());
  WorkflowDocument enormous;
  enormous.nodes = {
      {1,
       "fixture.contract",
       {},
       {{"dtype", std::string("float64")},
        {"semantic", semantic_parameter(signal()).take_value()}}}};
  enormous.outputs = {{"output", 1, "value"}};
  GraphContext huge_graph(enormous);
  Compiler wide_compiler(wide);
  PS_CHECK(wide_compiler.compile(huge_graph).status().code ==
           ErrorCode::ResourceExhausted);
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
