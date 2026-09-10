#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Parameters = std::map<std::string, ParameterValue>;
Value value(const std::vector<std::uint64_t>& shape,
            const std::vector<float>& samples,
            const std::vector<ValueFacet>& facets = {}) {
  auto made = MutableValue::allocate({ElementType::Float32, shape},
                                     Region::whole(shape), BufferAllocator{});
  auto output = made.take_value();
  std::memcpy(output.data(), samples.data(), samples.size() * 4);
  return std::move(output).publish(facets).take_value();
}
Value image(const SemanticDescriptor& s, const std::vector<float>& samples) {
  return value({1, samples.size() / s.channels.size(), s.channels.size()},
               samples, {encode_semantic(s).take_value()});
}
SemanticDescriptor rgb(bool alpha = true) {
  auto s = rgba_semantics();
  s.association = alpha ? "straight" : "none";
  if (!alpha)
    s.channels.pop_back();
  return s;
}
SemanticDescriptor descriptor(const Value& v) {
  return decode_semantic(v.facets()[0]).take_value();
}
Value output(const Result<ExecutionResult>& r) {
  return r.value().values.at("result");
}
bool close(const Value& actual, const std::vector<float>& expected,
           double tolerance = 1e-5) {
  if (actual.bytes().size() != expected.size() * 4)
    return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    float number;
    std::memcpy(&number, actual.bytes().data() + i * 4, 4);
    if (!std::isfinite(number) || std::abs(number - expected[i]) >
                                      tolerance * (1 + std::abs(expected[i])))
      return false;
  }
  return true;
}
Parameters selection(std::initializer_list<std::uint32_t> indices) {
  return {{"indices", channel_indices_parameter(indices).take_value()}};
}
Result<ExecutionResult> graph(const std::vector<Value>& inputs,
                              const std::vector<WorkflowNode>& nodes,
                              std::shared_ptr<OperationRegistry> registry = {},
                              ExecutionContextConfig config = {}) {
  if (!registry)
    registry = make_default_operation_registry();
  WorkflowDocument doc;
  ExecutionBindings bindings;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto name = "input" + std::to_string(i);
    doc.inputs.push_back({i + 1, name, inputs[i].descriptor(),
                          inputs[i].region(), inputs[i].layout(),
                          inputs[i].facets()});
    bindings.inputs.push_back({name, inputs[i]});
  }
  doc.nodes = nodes;
  doc.outputs = {{"result", nodes.back().id, "value"}};
  GraphContext context(doc);
  Compiler compiler(registry);
  auto plan = compiler.compile(context);
  if (!plan.ok())
    return Result<ExecutionResult>(plan.status());
  ExecutionContext execution(registry, config);
  return execution.execute(plan.value().plan, bindings);
}
Result<ExecutionResult> run(const std::string& key,
                            const std::vector<Value>& inputs,
                            const Parameters& parameters = {}) {
  std::vector<WorkflowInput> references;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    references.push_back(WorkflowInputReference{i + 1});
  return graph(inputs, {{1, key, references, parameters}});
}
Result<ExecutionResult> producer_run(const std::string& key, const Value& input,
                                     const Parameters& parameters) {
  auto base = make_default_operation_registry();
  auto registry = std::make_shared<OperationRegistry>();
  auto status = registry->register_operation(
      {key, base->find_traits(key).take_value(),
       [base, key](const OperationInvocation& call) {
         return base->invoke(key, call);
       }});
  if (!status.ok())
    return Result<ExecutionResult>(status);
  OperationTraits source;
  source.output_element_type = input.descriptor().element_type;
  source.shape_rule = OperationShapeRule::Fixed;
  source.fixed_output_shape = input.descriptor().shape;
  source.output_semantic_rule = OperationSemanticRule::Establish;
  source.output_facets = input.facets();
  source.estimated_bytes = input.storage()->capacity();
  status = registry->register_operation(
      {"fixture.view", source,
       [input](const OperationInvocation&) { return Result<Value>(input); }});
  if (!status.ok())
    return Result<ExecutionResult>(status);
  status = registry->freeze();
  if (!status.ok())
    return Result<ExecutionResult>(status);
  return graph({},
               {{1, "fixture.view", {}, {}},
                {2, key, {WorkflowNodeOutput{1, "value"}}, parameters}},
               registry);
}
int merge_arity() {
  auto base = make_default_operation_registry();
  const auto traits = base->find_traits("channel.merge").take_value();
  PS_CHECK(traits.input_count == 0 && traits.repeated_minimum == 2 &&
           traits.repeated_maximum == 4);
  unsigned callbacks = 0;
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry
               ->register_operation({"channel.merge", traits,
                                     [&](const OperationInvocation& call) {
                                       ++callbacks;
                                       return base->invoke("channel.merge",
                                                           call);
                                     }})
               .ok());
  PS_CHECK(registry->freeze().ok());
  SemanticDescriptor vector;
  vector.kind = SemanticKind::VectorField;
  vector.unit = "pixels";
  vector.channels = {{"dx", "x", "pixels"}, {"dy", "y", "pixels"}};
  vector.coordinate_space = "pixel_displacement";
  vector.direction = "forward";
  auto vector3 = vector;
  vector3.channels.push_back({"dz", "z", "pixels"});
  SemanticDescriptor complex;
  complex.kind = SemanticKind::ComplexField;
  complex.channels = {{"real", "real", "dimensionless"},
                      {"imag", "imaginary", "dimensionless"}};
  complex.coordinate_space = "frequency_unshifted";
  complex.direction = "forward_negative_inverse_1n";
  for (const auto& target : {vector, vector3, complex, rgb(false), rgb()}) {
    const auto count = target.channels.size();
    const std::vector<Value> inputs(count, value({1, 1}, {.5F}));
    const std::vector<Region> demands(count, Region::whole({1, 1}));
    const Parameters parameters{
        {"semantic", semantic_parameter(target).take_value()}};
    std::vector<WorkflowInput> references;
    for (std::size_t i = 0; i < count; ++i)
      references.push_back(WorkflowInputReference{i + 1});
    callbacks = 0;
    auto direct =
        registry->invoke("channel.merge", {inputs, demands, parameters});
    PS_CHECK(direct.ok() && callbacks == 1);
    auto compiled =
        graph(inputs, {{1, "channel.merge", references, parameters}}, registry);
    PS_CHECK(compiled.ok() && callbacks == 2);
    PS_CHECK(close(output(compiled), std::vector<float>(count, .5F), 0));
    PS_CHECK(output(compiled).facets()[0].payload ==
             encode_semantic(target).value().payload);
    PS_CHECK(direct.value().copy_bytes() == output(compiled).copy_bytes());
  }
  const Parameters parameters{
      {"semantic", semantic_parameter(rgb(false)).take_value()}};
  for (const std::size_t count : {1U, 5U}) {
    const std::vector<Value> inputs(count, value({1, 1}, {.5F}));
    const std::vector<Region> demands(count, Region::whole({1, 1}));
    std::vector<WorkflowInput> references;
    for (std::size_t i = 0; i < count; ++i)
      references.push_back(WorkflowInputReference{i + 1});
    callbacks = 0;
    PS_CHECK(
        resolve_operation_traits(traits, count, parameters).status().code ==
        ErrorCode::InvalidArgument);
    PS_CHECK(registry->invoke("channel.merge", {inputs, demands, parameters})
                 .status()
                 .code == ErrorCode::InvalidArgument);
    PS_CHECK(
        graph(inputs, {{1, "channel.merge", references, parameters}}, registry)
            .status()
            .code == ErrorCode::TypeMismatch);
    PS_CHECK(callbacks == 0);
  }
  return 0;
}
int composition() {
  auto source = image(rgba_semantics(), {-2, .5F, 4, .5F, 1, 2, 3, 1});
  for (float factor : {1.F, 2.F}) {
    std::vector<WorkflowNode> nodes;
    for (std::int64_t c = 0; c < 4; ++c)
      nodes.push_back({static_cast<std::uint64_t>(c + 1),
                       "channel.extract",
                       {WorkflowInputReference{1}},
                       {{"index", c}}});
    nodes.push_back(
        {5,
         "numeric.multiply",
         {WorkflowNodeOutput{1, "value"}, WorkflowInputReference{2}},
         {}});
    nodes.push_back(
        {6,
         "channel.merge",
         {WorkflowNodeOutput{5, "value"}, WorkflowNodeOutput{2, "value"},
          WorkflowNodeOutput{3, "value"}, WorkflowNodeOutput{4, "value"}},
         {{"semantic", semantic_parameter(rgba_semantics()).take_value()}}});
    auto result = graph({source, value({1, 2}, {factor, factor})}, nodes);
    PS_CHECK(result.ok());
    PS_CHECK(
        close(output(result), {-2 * factor, .5F, 4, .5F, factor, 2, 3, 1}, 0));
    PS_CHECK(output(result).facets()[0].payload == source.facets()[0].payload);
  }
  auto coverage = graph({source}, {{1,
                                    "channel.extract",
                                    {WorkflowInputReference{1}},
                                    {{"index", INT64_C(3)}}},
                                   {2,
                                    "mask.downsample_box",
                                    {WorkflowNodeOutput{1, "value"}},
                                    {{"factor", INT64_C(2)}}}});
  PS_CHECK(coverage.ok() && close(output(coverage), {.75F}, 0));
  PS_CHECK(output(coverage).facets()[0].payload ==
           encode_semantic(coverage_semantics()).value().payload);
  auto bgr = run("channel.swizzle", {source}, selection({2, 1, 0, 3}));
  PS_CHECK(bgr.ok() && descriptor(output(bgr)).channels[0].role == "blue");
  PS_CHECK(close(output(bgr), {4, .5F, -2, .5F, 3, 2, 1, 1}, 0));
  auto original =
      run("channel.swizzle", {output(bgr)}, selection({2, 1, 0, 3}));
  PS_CHECK(original.ok() &&
           output(original).copy_bytes() == source.copy_bytes());
  auto duplicated = run("channel.swizzle", {source}, selection({0, 0}));
  PS_CHECK(duplicated.ok() && output(duplicated).facets().empty());
  PS_CHECK(output(duplicated).descriptor().shape ==
           (std::vector<std::uint64_t>{1, 2, 2}));
  PS_CHECK(close(output(duplicated), {-2, -2, 1, 1}, 0));
  auto premul_drop = run("channel.swizzle", {source}, selection({0, 1, 2}));
  PS_CHECK(premul_drop.ok() && output(premul_drop).facets().empty());
  auto straight_drop = run("channel.swizzle", {image(rgb(), {-2, .5F, 4, .5F})},
                           selection({2, 1, 0}));
  PS_CHECK(straight_drop.ok() &&
           descriptor(output(straight_drop)).association == "none");
  auto alpha_first = run("channel.swizzle", {source}, selection({3, 0, 1, 2}));
  PS_CHECK(alpha_first.ok() && output(alpha_first).facets().empty());
  return 0;
}
int alpha() {
  const float tiny = std::numeric_limits<float>::denorm_min();
  const auto source = image(
      rgb(), {-2, .5F, 4, .5F, 1, -1, 2, 1, 1, -1, 0, tiny, 9, 8, 7, -0.F});
  auto associated = run("alpha.associate", {source});
  PS_CHECK(associated.ok());
  PS_CHECK(close(
      output(associated),
      {-1, .25F, 2, .5F, 1, -1, 2, 1, tiny, -tiny, 0, tiny, 0, 0, 0, -0.F}, 0));
  auto restored = run("alpha.unassociate", {output(associated)});
  PS_CHECK(restored.ok());
  PS_CHECK(close(output(restored),
                 {-2, .5F, 4, .5F, 1, -1, 2, 1, 1, -1, 0, tiny, 0, 0, 0, -0.F},
                 0));
  for (std::size_t pixel = 0; pixel < 4; ++pixel) {
    PS_CHECK(
        std::memcmp(source.bytes().data() + (pixel * 4 + 3) * 4,
                    output(associated).bytes().data() + (pixel * 4 + 3) * 4,
                    4) == 0);
    PS_CHECK(std::memcmp(source.bytes().data() + (pixel * 4 + 3) * 4,
                         output(restored).bytes().data() + (pixel * 4 + 3) * 4,
                         4) == 0);
  }
  auto overflow =
      run("alpha.unassociate", {image(rgba_semantics(), {1, 0, 0, tiny})});
  PS_CHECK(overflow.status().code == ErrorCode::OperationFailed);
  PS_CHECK(run("alpha.associate", {output(associated)}).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(run("alpha.unassociate", {source}).status().code ==
           ErrorCode::TypeMismatch);
  auto assigned = run("color.assign", {value({1, 1, 4}, {-2, 3, 4, 0})},
                      {{"semantic", semantic_parameter(rgb()).take_value()}});
  PS_CHECK(assigned.ok() && close(output(assigned), {-2, 3, 4, 0}, 0));
  PS_CHECK(
      run("color.assign", {value({1, 1, 4}, {-2, 3, 4, 0})},
          {{"semantic", semantic_parameter(rgba_semantics()).take_value()}})
          .status()
          .code == ErrorCode::OperationFailed);
  return 0;
}
int colors() {
  auto source =
      image(rgb(false), {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, -1, 2, 4});
  auto xyz = run("color.rgb_to_xyz", {source});
  PS_CHECK(xyz.ok());
  PS_CHECK(close(output(xyz),
                 {.4123908F, .2126390F, .01933082F, .35758434F, .71516868F,
                  .11919478F, .18048079F, .07219232F, .95053215F, .95045593F, 1,
                  1.08905775F, 1.0247010F, 1.5064677F, 4.0211874F},
                 2e-6));
  auto back = run("color.xyz_to_rgb", {output(xyz)});
  PS_CHECK(back.ok() &&
           close(output(back), {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, -1, 2, 4},
                 2e-6));
  auto bgr = run("channel.swizzle", {source}, selection({2, 1, 0}));
  PS_CHECK(bgr.ok());
  auto bgr_xyz = run("color.rgb_to_xyz", {output(bgr)});
  PS_CHECK(bgr_xyz.ok() &&
           output(bgr_xyz).copy_bytes() == output(xyz).copy_bytes());
  auto custom = rgb();
  custom.channels[3].name = "X";
  auto named = run("color.rgb_to_xyz", {image(custom, {1, .5F, -.25F, -0.F})});
  PS_CHECK(named.ok() && descriptor(output(named)).channels[3].name == "A");
  PS_CHECK(std::memcmp(output(named).bytes().data() + 12,
                       image(custom, {1, .5F, -.25F, -0.F}).bytes().data() + 12,
                       4) == 0);
  for (const auto white :
       {rgba_semantics().white,
        std::array<double, 3>{.3457 / .3585, 1, (1 - .3457 - .3585) / .3585}}) {
    auto s = descriptor(output(xyz));
    s.white = white;
    auto samples =
        image(s, {static_cast<float>(white[0]), 1, static_cast<float>(white[2]),
                  0, 0, 0, -.1F, .2F, 2});
    auto lab = run("color.xyz_to_lab", {samples});
    PS_CHECK(lab.ok());
    float values[9];
    std::memcpy(values, output(lab).bytes().data(), sizeof(values));
    PS_CHECK(std::abs(values[0] - 100) < 1e-5 && std::abs(values[1]) < 1e-4 &&
             std::abs(values[2]) < 1e-4);
    PS_CHECK(values[3] == 0 && values[4] == 0 && values[5] == 0);
    auto roundtrip = run("color.lab_to_xyz", {output(lab)});
    PS_CHECK(roundtrip.ok() &&
             close(output(roundtrip),
                   {static_cast<float>(white[0]), 1,
                    static_cast<float>(white[2]), 0, 0, 0, -.1F, .2F, 2},
                   2e-6));
    PS_CHECK(descriptor(output(roundtrip)).white == white);
    if (white != rgba_semantics().white)
      PS_CHECK(run("color.xyz_to_rgb", {samples}).status().code ==
               ErrorCode::TypeMismatch);
  }
  PS_CHECK(run("color.rgb_to_xyz", {image(rgba_semantics(), {1, 2, 3, .5F})})
               .status()
               .code == ErrorCode::TypeMismatch);
  PS_CHECK(run("color.xyz_to_lab", {source}).status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(run("color.rgb_to_xyz",
               {image(rgb(false), {std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max()})})
               .status()
               .code == ErrorCode::OperationFailed);
  return 0;
}
int contracts() {
  for (const auto* text : {"", "01", "1,", "1,,2", "64", "-1", " 1", "1 2"})
    PS_CHECK(!channel_indices_from_parameter(text).ok());
  PS_CHECK(!channel_indices_parameter({}).ok());
  PS_CHECK(!channel_indices_parameter(std::vector<std::uint32_t>(65)).ok());
  PS_CHECK(!channel_indices_from_parameter(std::string(192, '0')).ok());
  auto maximum = channel_indices_parameter(std::vector<std::uint32_t>(64, 63));
  PS_CHECK(maximum.ok() && maximum.value().size() == 191 &&
           channel_indices_from_parameter(maximum.value()).value().size() ==
               64);
  auto source = image(rgb(), {1, 2, 3, .5F});
  PS_CHECK(
      run("channel.extract", {source}, {{"index", INT64_C(4)}}).status().code ==
      ErrorCode::InvalidArgument);
  PS_CHECK(run("channel.swizzle", {source}, selection({4})).status().code ==
           ErrorCode::InvalidArgument);
  auto red = run("channel.extract", {source}, {{"index", INT64_C(0)}});
  PS_CHECK(red.ok());
  PS_CHECK(run("channel.merge",
               {output(red), output(red), output(red), value({1, 1}, {1})},
               {{"semantic", semantic_parameter(rgb()).take_value()}})
               .status()
               .code == ErrorCode::TypeMismatch);
  SemanticDescriptor field;
  field.kind = SemanticKind::ScalarField;
  field.unit = "meters";
  field.channels = {{"R", "red", "meters"}};
  auto meters = value({1, 1}, {1}, {encode_semantic(field).take_value()});
  PS_CHECK(run("channel.merge",
               {meters, value({1, 1}, {2}), value({1, 1}, {3})},
               {{"semantic", semantic_parameter(rgb(false)).take_value()}})
               .status()
               .code == ErrorCode::TypeMismatch);
  auto malformed = rgb();
  malformed.transfer = "srgb";
  PS_CHECK(!encode_semantic(malformed).ok());
  malformed = rgb();
  malformed.channels[0].unit = "meters";
  PS_CHECK(!encode_semantic(malformed).ok());
  auto assigned = run("color.assign", {value({1, 1, 3}, {1, 2, 3})},
                      {{"semantic", std::string("FF")}});
  PS_CHECK(assigned.status().code == ErrorCode::InvalidArgument);
  // A target can establish non-image HWC channels through the same merge rule.
  SemanticDescriptor vector;
  vector.kind = SemanticKind::VectorField;
  vector.unit = "pixels";
  vector.channels = {{"dx", "x", "pixels"}, {"dy", "y", "pixels"}};
  vector.coordinate_space = "pixel_displacement";
  vector.direction = "forward";
  auto merged = run("channel.merge", {value({1, 1}, {-2}), value({1, 1}, {3})},
                    {{"semantic", semantic_parameter(vector).take_value()}});
  PS_CHECK(merged.ok() &&
           descriptor(output(merged)).kind == SemanticKind::VectorField &&
           close(output(merged), {-2, 3}, 0));
  auto extracted =
      run("channel.extract", {output(merged)}, {{"index", INT64_C(1)}});
  PS_CHECK(extracted.ok() && descriptor(output(extracted)).unit == "pixels");
  auto double_field = [](double sample) {
    return Value::create({ElementType::Float64, {1, 1}}, Region::whole({1, 1}),
                         {0, {8, 8}}, Value::from_float64(sample).copy_bytes())
        .take_value();
  };
  PS_CHECK(run("channel.merge",
               {double_field(1), double_field(2), double_field(3)},
               {{"semantic", semantic_parameter(rgb(false)).take_value()}})
               .status()
               .code == ErrorCode::TypeMismatch);
  auto double_vector =
      run("channel.merge", {double_field(-2), double_field(3)},
          {{"semantic", semantic_parameter(vector).take_value()}});
  PS_CHECK(double_vector.ok() &&
           output(double_vector).descriptor().element_type ==
               ElementType::Float64);
  auto double_component =
      run("channel.extract", {output(double_vector)}, {{"index", INT64_C(1)}});
  PS_CHECK(double_component.ok());
  double component;
  std::memcpy(&component, output(double_component).bytes().data(), 8);
  PS_CHECK(component == 3 &&
           descriptor(output(double_component)).unit == "pixels");
  SemanticDescriptor complex;
  complex.kind = SemanticKind::ComplexField;
  complex.channels = {{"real", "real", "dimensionless"},
                      {"imag", "imaginary", "dimensionless"}};
  complex.coordinate_space = "frequency_unshifted";
  complex.direction = "forward_negative_inverse_1n";
  auto complex_value =
      run("channel.merge", {value({1, 1}, {-2}), value({1, 1}, {3})},
          {{"semantic", semantic_parameter(complex).take_value()}});
  PS_CHECK(complex_value.ok() && descriptor(output(complex_value)).kind ==
                                     SemanticKind::ComplexField);
  auto reversed_complex =
      run("channel.swizzle", {output(complex_value)}, selection({1, 0}));
  PS_CHECK(reversed_complex.ok() && output(reversed_complex).facets().empty() &&
           close(output(reversed_complex), {3, -2}, 0));
  const float backing[] = {.5F, 4, 2, -1};
  std::vector<std::uint8_t> padded(17);
  std::memcpy(padded.data() + 1, backing, 16);
  auto view =
      Value::create({ElementType::Float32, {1, 1, 4}}, Region::whole({1, 1, 4}),
                    {1, {16, 16, -4}, {0, 0, 3}}, padded,
                    {encode_semantic(rgba_semantics()).take_value()})
          .take_value();
  auto permuted_view =
      producer_run("channel.swizzle", view, selection({2, 1, 0, 3}));
  PS_CHECK(permuted_view.ok() &&
           close(output(permuted_view), {4, 2, -1, .5F}, 0));
  auto divided_view = producer_run("alpha.unassociate", view, {});
  PS_CHECK(divided_view.ok() &&
           close(output(divided_view), {-2, 4, 8, .5F}, 0));
  ExecutionContextConfig limited;
  limited.maximum_live_bytes = 1;
  PS_CHECK(graph({source},
                 {{1,
                   "channel.swizzle",
                   {WorkflowInputReference{1}},
                   selection({2, 1, 0, 3})}},
                 {}, limited)
               .status()
               .code == ErrorCode::ResourceExhausted);
  CancellationSource cancelled;
  std::uint64_t live = 0;
  BufferAllocator allocator([&](std::uint64_t bytes) {
    live += bytes;
    cancelled.cancel();
    return Result<std::shared_ptr<void>>(
        std::shared_ptr<void>(new int(0), [&, bytes](void* p) {
          delete static_cast<int*>(p);
          live -= bytes;
        }));
  });
  const std::vector<Value> inputs{source};
  const std::vector<Region> demands{source.region()};
  const Parameters none;
  auto registry = make_default_operation_registry();
  auto stopped = registry->invoke(
      "alpha.associate", {inputs, demands, none, Backend::Cpu,
                          cancelled.token(), source.region(), allocator});
  PS_CHECK(stopped.status().code == ErrorCode::Cancelled && live == 0);
  return 0;
}
int c_contract() {
#ifdef PS_CHANNEL_CONTRACT_FIXTURE
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_CHANNEL_CONTRACT_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  auto source = image(rgb(false), {1, 2, 3});
  auto result = graph({source},
                      {{1,
                        "fixture.channel_contract",
                        {WorkflowInputReference{1}},
                        selection({2, 1, 0})}},
                      registry);
  PS_CHECK(result.ok() &&
           descriptor(output(result)).channels[0].role == "blue" &&
           close(output(result), {0, 0, 0}, 0));
  result = graph({source},
                 {{1,
                   "fixture.channel_contract",
                   {WorkflowInputReference{1}},
                   selection({0, 0})}},
                 registry);
  PS_CHECK(result.ok() && output(result).facets().empty() &&
           output(result).descriptor().shape[2] == 2);
#ifdef PS_BAD_CHANNEL_1
  for (const auto* path : {PS_BAD_CHANNEL_1, PS_BAD_CHANNEL_2}) {
    OperationRegistry bad;
    PS_CHECK(!bad.load_plugin(path).ok() && bad.keys().empty());
  }
#endif
#endif
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(merge_arity() == 0);
  PS_CHECK(composition() == 0);
  PS_CHECK(alpha() == 0);
  PS_CHECK(colors() == 0);
  PS_CHECK(contracts() == 0);
  PS_CHECK(c_contract() == 0);
  return 0;
}
