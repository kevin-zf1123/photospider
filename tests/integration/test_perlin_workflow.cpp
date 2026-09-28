#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
const char* key = "noise.perlin2002_3d_v1_strict_cpu_whole";
Result<ExecutionResult> run(const Value& input, unsigned workers, bool narrow,
                            bool default_dtype = false, bool exhausted = false,
                            bool cancelled = false, bool source_view = false,
                            double* execute_ms = nullptr) {
  auto registry = make_default_operation_registry(false);
  WorkflowDocument document;
  auto layout = input.layout();
  layout.byte_offset = 0;
  layout.origin.clear();
  std::int64_t stride = Value::element_size(input.descriptor().element_type);
  for (std::size_t axis = input.descriptor().shape.size(); axis; --axis) {
    layout.byte_strides[axis - 1] = stride;
    stride *= input.descriptor().shape[axis - 1];
  }
  document.inputs = {{1,
                      "coordinates",
                      input.descriptor(),
                      Region::whole(input.descriptor().shape),
                      layout,
                      {}}};
  std::map<std::string, ParameterValue> parameters;
  if (!default_dtype)
    parameters["dtype"] = std::string(narrow ? "float32" : "float64");
  document.nodes = {{1, key, {WorkflowInputReference{1}}, parameters}};
  document.outputs = {{"values", 1, "values"}};
  if (source_view) {
    document.inputs.clear();
    document.nodes[0].inputs = {WorkflowNodeOutput{2, "value"}};
    OperationDefinition source;
    source.key = "test.coordinates";
    auto& output = source.traits.outputs[0];
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = input.descriptor().shape;
    output.output_element_type = input.descriptor().element_type;
    output.preserve_output_views = true;
    source.traits.estimated_bytes = input.bytes().size();
    source.callback = [input](const OperationInvocation&) {
      return Result<Value>(input);
    };
    auto registered = registry->register_operation(std::move(source));
    if (!registered.ok())
      return Result<ExecutionResult>(registered);
    document.nodes.push_back({2, "test.coordinates", {}, {}});
  }
  registry->freeze();
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  if (exhausted)
    config.managed_resources->maximum_work = 10000;
  ExecutionContext execution(registry, config);
  CancellationSource cancellation;
  if (cancelled)
    cancellation.cancel();
  ExecutionBindings bindings;
  if (!source_view)
    bindings.inputs = {{"coordinates", input}};
  ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(1000000000);
  options.maximum_dependency_work = UINT64_C(1000000000);
  const auto started = std::chrono::steady_clock::now();
  auto result = execution.execute(compiled.value().plan, std::move(bindings),
                                  cancellation.token(), options);
  if (execute_ms)
    *execute_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  if (execute_ms && result.ok()) {
    auto budget = execution.resource_budget();
    if (!budget.ok())
      return Result<ExecutionResult>(budget.status());
    auto value = result.take_value();
    value.diagnostics.managed_resources = budget.value().statistics();
    return Result<ExecutionResult>(std::move(value));
  }
  return result;
}
Value make(const std::vector<std::uint64_t>& raw, bool narrow) {
  const unsigned width = narrow ? 4 : 8;
  std::vector<std::uint8_t> bytes(raw.size() * width);
  for (unsigned i = 0; i < raw.size(); ++i)
    std::memcpy(bytes.data() + i * width, &raw[i], width);
  return Value::create({narrow ? ElementType::Float32 : ElementType::Float64,
                        {raw.size() / 3, 3}},
                       Region::whole({raw.size() / 3, 3}),
                       {0, {3 * width, width}}, std::move(bytes))
      .take_value();
}
}  // namespace
int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--tiled-stdin")
    key = "noise.perlin2002_3d_v1_strict_cpu_tiled";
  if (argc == 2 && std::string(argv[1]) == "--benchmark") {
    constexpr unsigned count = 16384;
    std::vector<std::uint64_t> raw(count * 3);
    for (unsigned i = 0; i < raw.size(); ++i) {
      const double value =
          static_cast<double>((i * 1709U + 719U) % 131071U) / 65536. - 1.;
      std::memcpy(&raw[i], &value, 8);
    }
    const auto input = make(raw, false);
    std::vector<std::uint8_t> reference;
    for (unsigned workers : {1, 4, 8}) {
      std::vector<double> times;
      std::uint64_t peak = 0, work = 0;
      for (unsigned repetition = 0; repetition < 6; ++repetition) {
        double elapsed = 0;
        auto result =
            run(input, workers, false, false, false, false, false, &elapsed);
        PS_CHECK(result.ok());
        const auto& bytes = result.value().values.at("values").bytes();
        if (reference.empty())
          reference.assign(bytes.begin(), bytes.end());
        PS_CHECK(std::equal(reference.begin(), reference.end(), bytes.begin(),
                            bytes.end()));
        if (repetition)
          times.push_back(elapsed);
        peak = result.value()
                   .diagnostics.managed_resources->peak[ResourceKind::Host];
        work = result.value().diagnostics.managed_resources->issued.work;
      }
      std::sort(times.begin(), times.end());
      std::cout << "{\"count\":" << count << ",\"workers\":" << workers
                << ",\"median_ms\":" << times[2] << ",\"p95_ms\":" << times[4]
                << ",\"peak_host_bytes\":" << peak
                << ",\"issued_work\":" << work
                << ",\"bitwise_reference\":true}\n";
    }
    return 0;
  }
  if (argc == 2 && (std::string(argv[1]) == "--stdin" ||
                    std::string(argv[1]) == "--tiled-stdin")) {
    unsigned input_narrow, output_narrow;
    std::vector<std::uint64_t> raw(3);
    while (std::cin >> std::dec >> input_narrow >> output_narrow >> std::hex >>
           raw[0] >> raw[1] >> raw[2]) {
      auto result = run(make(raw, input_narrow), 1, output_narrow);
      if (!result.ok()) {
        std::cerr << result.status().message << '\n';
        return 1;
      }
      std::uint64_t output = 0;
      const auto& value = result.value().values.at("values");
      std::memcpy(&output, value.bytes().data(), output_narrow ? 4 : 8);
      std::cout << std::hex << output << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  std::vector<std::uint64_t> raw(323 * 3);
  for (unsigned i = 0; i < 323; ++i) {
    const double x = .25 + 256. * i;
    std::memcpy(&raw[3 * i], &x, 8);
  }
  const auto input = make(raw, false);
  for (unsigned workers : {1, 4, 16}) {
    for (bool narrow : {false, true}) {
      auto result = run(input, workers, narrow, !narrow);
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      const auto& value = result.value().values.at("values");
      PS_CHECK(value.descriptor().shape == std::vector<std::uint64_t>{323});
      for (unsigned i = 0; i < 323; ++i) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, value.bytes().data() + i * (narrow ? 4 : 8),
                    narrow ? 4 : 8);
        // Exact analytic slice x-fade(x) at x=1/4, period 256.
        PS_CHECK(bits == (narrow ? UINT64_C(0x3e160000)
                                 : UINT64_C(0x3fc2c00000000000)));
      }
    }
  }
  // Direct public registry invocation preserves a legal strided view. Workflow
  // transport can materialize a producer, so it is tested separately above.
  for (unsigned i = 0; i < 323; ++i) {
    const double x = .125 + .0625 * (i % 7) + 256. * i;
    std::memcpy(&raw[3 * i], &x, 8);
  }
  const auto varied = make(raw, false);
  std::vector<std::uint8_t> padded(varied.bytes().size() + 17, 0xff);
  std::memcpy(padded.data() + 17, varied.bytes().data(), varied.bytes().size());
  auto view = Value::create(varied.descriptor(), varied.region(),
                            {17, {-24, 8}, {322, 0}}, std::move(padded));
  PS_CHECK(view.ok());
  auto reversed = run(view.value(), 4, false, false, false, false, true);
  PS_CHECK(reversed.ok());
  auto original = run(varied, 1, false);
  PS_CHECK(original.ok());
  auto registry = make_default_operation_registry();
  const std::vector<Value> views{view.value()};
  const std::vector<Region> demands{view.value().region()};
  const std::map<std::string, ParameterValue> parameters;
  OperationInvocation call(views, demands, parameters, Backend::Cpu, {},
                           Region::whole({323}));
  auto direct = registry->invoke(key, call);
  PS_CHECK(direct.ok());
  for (unsigned i = 0; i < 323; ++i) {
    const auto* expected =
        original.value().values.at("values").bytes().data() + (322 - i) * 8;
    PS_CHECK(
        std::memcmp(reversed.value().values.at("values").bytes().data() + i * 8,
                    expected, 8) == 0);
    PS_CHECK(std::memcmp(direct.value().bytes().data() + i * 8, expected, 8) ==
             0);
  }
  auto exhausted = run(input, 4, false, false, true);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  auto cancelled = run(input, 4, false, false, false, true);
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
  raw[3] = UINT64_C(0x7ff0000000000001);
  auto invalid = run(make(raw, false), 4, false);
  PS_CHECK(!invalid.ok() &&
           invalid.status().code == ErrorCode::InvalidArgument);
  return 0;
}
