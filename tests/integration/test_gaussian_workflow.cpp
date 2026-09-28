#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
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
Parameters parameters(std::int64_t rx = 1, std::int64_t ry = 1,
                      const std::string& boundary = "clamp") {
  return {{"radius_x", rx},
          {"radius_y", ry},
          {"sigma_x", 1.},
          {"sigma_y", 1.},
          {"x_axis", std::int64_t{1}},
          {"y_axis", std::int64_t{0}},
          {"cval", 3.},
          {"boundary", boundary}};
}
Value input_value(bool narrow, const std::vector<std::uint64_t>& shape,
                  const std::vector<std::uint64_t>& bits) {
  ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64, shape};
  auto writer = MutableValue::allocate(descriptor, Region::whole(shape),
                                       BufferAllocator{})
                    .take_value();
  const auto width = narrow ? 4 : 8;
  for (unsigned i = 0; i < bits.size(); ++i)
    std::memcpy(writer.data() + i * width, &bits[i], width);
  return std::move(writer).publish().take_value();
}
Result<Value> run(const Value& input, Parameters parameters, unsigned workers,
                  ResourceLimits limits = {},
                  CancellationToken cancellation = {}, bool producer = false,
                  bool tiled = false) {
  auto registry = make_default_operation_registry(false);
  WorkflowDocument document;
  document.inputs = {{1, "input", input.descriptor(), input.region(),
                      input.layout(), input.facets()}};
  document.nodes = {{1,
                     tiled ? "filter.gaussian_baked64_v1_strict_cpu_tiled"
                           : "filter.gaussian_baked64_v1_strict_cpu_whole",
                     {WorkflowInputReference{1}},
                     std::move(parameters)}};
  document.outputs = {{"output", 1, "output"}};
  ExecutionBindings bindings{{{"input", input}}};
  if (producer) {
    OperationDefinition source;
    source.key = "test.gaussian_input";
    auto& output = source.traits.outputs[0];
    output.output_element_type = input.descriptor().element_type;
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = input.descriptor().shape;
    output.preserve_output_views = true;
    source.traits.estimated_bytes = input.bytes().size();
    source.callback = [input](const OperationInvocation&) {
      return Result<Value>(input);
    };
    auto registered = registry->register_operation(std::move(source));
    if (!registered.ok())
      return Result<Value>(registered);
    document.inputs.clear();
    bindings.inputs.clear();
    document.nodes[0].inputs = {WorkflowNodeOutput{2, "value"}};
    document.nodes.push_back({2, "test.gaussian_input", {}, {}});
  }
  registry->freeze();
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_width = planning.tile_height = 2;
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    return Result<Value>(compiled.status());
  ExecutionContextConfig config;
  config.gpu_enabled = false;
  config.cpu_workers = workers;
  config.managed_resources = limits;
  ExecutionContext context(registry, config);
  ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(1000000000000);
  options.maximum_dependency_work = UINT64_C(1000000000000);
  auto result =
      context.execute(compiled.value().plan, bindings, cancellation, options);
  return result.ok() ? Result<Value>(result.value().values.at("output"))
                     : Result<Value>(result.status());
}
std::uint64_t word(const Value& value, unsigned index) {
  std::uint64_t result = 0;
  const auto width = Value::element_size(value.descriptor().element_type);
  std::memcpy(&result, value.bytes().data() + index * width, width);
  return result;
}
double number(std::uint64_t bits) {
  double value;
  std::memcpy(&value, &bits, 8);
  return value;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc == 2 && (std::string(argv[1]) == "--stdin" ||
                    std::string(argv[1]) == "--tiled-stdin")) {
    unsigned narrow, height, width, rx, ry;
    std::uint64_t sx, sy, cval;
    std::string boundary;
    while (std::cin >> std::dec >> narrow >> height >> width >> rx >> ry >>
           std::hex >> sx >> sy >> cval >> boundary) {
      std::vector<std::uint64_t> data(height * width);
      for (auto& value : data)
        std::cin >> std::hex >> value;
      auto p = parameters(rx, ry, boundary);
      p["sigma_x"] = number(sx);
      p["sigma_y"] = number(sy);
      p["cval"] = number(cval);
      auto result = run(input_value(narrow, {height, width}, data), p, 4, {},
                        {}, false, std::string(argv[1]) == "--tiled-stdin");
      if (!result.ok()) {
        std::cerr << result.status().message << '\n';
        return 1;
      }
      for (unsigned i = 0; i < data.size(); ++i)
        std::cout << std::hex << word(result.value(), i) << ' ';
      std::cout << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  auto input = input_value(true, {1, 3}, {0x3f800000, 0x40000000, 0x40800000});
  const std::array<const char*, 5> boundaries{"constant", "clamp", "wrap",
                                              "reflect_half", "reflect_whole"};
  const std::array<std::array<std::uint32_t, 3>, 5> expected{
      {{0x401df06a, 0x402b01b3, 0x40452444},
       {0x3fa314ae, 0x40118a57, 0x405ceb52},
       {0x4006295c, 0x40118a57, 0x40284c4c},
       {0x3fa314ae, 0x40118a57, 0x405ceb52},
       {0x3fc6295c, 0x40118a57, 0x4039d6a4}}};
  for (unsigned i = 0; i < boundaries.size(); ++i)
    for (unsigned workers : {1, 4}) {
      auto result = run(input, parameters(1, 1, boundaries[i]), workers);
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      for (unsigned j = 0; j < 3; ++j)
        PS_CHECK(word(result.value(), j) == expected[i][j]);
    }
  auto exceptional =
      input_value(true, {1, 3}, {0xff800123, 0x7f800456, 0x80000000});
  auto copied = run(exceptional, parameters(0, 0), 4);
  PS_CHECK(copied.ok() &&
           copied.value().copy_bytes() == exceptional.copy_bytes());
  auto arithmetic = run(exceptional, parameters(1, 0), 4);
  PS_CHECK(arithmetic.ok() && word(arithmetic.value(), 0) == 0x7fc00456);
  for (const auto* name : {"sigma_x", "sigma_y", "radius_x", "boundary"}) {
    auto p = parameters();
    p.erase(name);
    PS_CHECK(!run(input, p, 1).ok());
  }
  for (const auto& change : std::vector<std::pair<std::string, ParameterValue>>{
           {"sigma_x", -1.},
           {"sigma_y", 0.},
           {"radius_x", std::int64_t{-1}},
           {"x_axis", std::int64_t{0}},
           {"boundary", std::string("mirror")}}) {
    auto p = parameters();
    p[change.first] = change.second;
    auto result = run(input, p, 1);
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::InvalidArgument);
  }
  std::vector<std::uint64_t> batch_bits(70);
  for (unsigned i = 0; i < batch_bits.size(); ++i) {
    const double value = (static_cast<int>(i * 17 % 53) - 26) / 8.;
    std::memcpy(&batch_bits[i], &value, 8);
  }
  auto batch = input_value(false, {2, 5, 7}, batch_bits);
  auto batch_parameters = parameters(2, 1, "wrap");
  batch_parameters["x_axis"] = std::int64_t{2};
  batch_parameters["y_axis"] = std::int64_t{1};
  auto serial = run(batch, batch_parameters, 1);
  auto parallel = run(batch, batch_parameters, 4);
  PS_CHECK(serial.ok() && parallel.ok());
  PS_CHECK(serial.value().copy_bytes() == parallel.value().copy_bytes());
  for (unsigned plane = 0; plane < 2; ++plane) {
    std::vector<std::uint64_t> slice(batch_bits.begin() + plane * 35,
                                     batch_bits.begin() + (plane + 1) * 35);
    auto separate =
        run(input_value(false, {5, 7}, slice), parameters(2, 1, "wrap"), 4);
    PS_CHECK(separate.ok());
    PS_CHECK(std::memcmp(separate.value().bytes().data(),
                         parallel.value().bytes().data() + plane * 35 * 8,
                         35 * 8) == 0);
  }
  std::vector<std::uint8_t> reversed_bytes(80);
  const std::uint32_t reversed_words[] = {0x40800000, 0x40000000, 0x3f800000};
  std::memcpy(reversed_bytes.data() + 17, reversed_words, 12);
  auto reversed =
      Value::create({ElementType::Float32, {1, 3}}, Region::whole({1, 3}),
                    {57, {12, -4}, {5, 7}}, reversed_bytes);
  PS_CHECK(reversed.ok());
  auto strided = run(reversed.value(), parameters(), 4, {}, {}, true);
  if (!strided.ok())
    std::cerr << strided.status().message << "\n";
  PS_CHECK(strided.ok());
  for (unsigned i = 0; i < 3; ++i)
    PS_CHECK(word(strided.value(), i) == expected[1][i]);
  ResourceLimits work;
  work.maximum_work = 10000;
  auto exhausted = run(input, parameters(), 4, work);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  work.maximum_work = 5000000;
  auto arithmetic_budget = run(batch, batch_parameters, 4, work);
  PS_CHECK(!arithmetic_budget.ok() &&
           arithmetic_budget.status().code == ErrorCode::ResourceExhausted);
  ResourceLimits capacity;
  capacity.capacity[ResourceKind::Payload] = 4096;
  auto shortage = run(input, parameters(), 4, capacity);
  PS_CHECK(!shortage.ok() &&
           shortage.status().code == ErrorCode::ResourceExhausted);
  CancellationSource stop;
  stop.cancel();
  auto cancelled = run(input, parameters(), 4, {}, stop.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
  auto huge = parameters(INT64_MAX, 1);
  auto overflow = run(input, huge, 1);
  PS_CHECK(!overflow.ok() &&
           overflow.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
