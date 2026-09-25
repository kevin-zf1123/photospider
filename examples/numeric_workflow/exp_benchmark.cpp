// NUM-04 exp acceptance and timings: public execution, callback and raw math.
#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/numeric_math_operation.hpp"
#include "photospider/numeric/unary.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace {
namespace numeric = ps::plugin_internal::numeric_ops;
template <class T>
T take(ps::Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
}
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
numeric::SequenceProfile selected_profile() {
#if defined(__aarch64__)
  return numeric::SequenceProfile::AppleSilicon;
#else
  return numeric::SequenceProfile::X86Avx2;
#endif
}
ps::Value input_value(const std::vector<float>& values, unsigned layout = 0) {
  auto buffer = take(ps::BufferAllocator{}.allocate(values.size() * 4 +
                                                    (layout == 3 ? 0 : 1)));
  const std::uint64_t base = layout == 3 ? 0 : 1;
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto index = layout == 1 ? values.size() - 1 - i : i;
    std::memcpy(buffer.data() + base + index * 4, &values[i], 4);
  }
  ps::StridedLayout strides{layout == 1 ? base + (values.size() - 1) * 4 : base,
                            {layout == 1   ? -4
                             : layout == 2 ? 0
                                           : 4}};
  return take(ps::Value::from_storage(
      {ps::ElementType::Float32, {values.size()}},
      ps::Region::whole({values.size()}), strides, std::move(buffer).freeze()));
}
struct Call final {
  std::vector<ps::Value> inputs;
  std::vector<ps::Region> demands;
  std::map<std::string, ps::ParameterValue> parameters;
  ps::OperationInvocation invocation;
  explicit Call(const ps::Value& value)
      : inputs{value},
        demands{value.region()},
        invocation(inputs, demands, parameters, ps::Backend::Cpu, {},
                   value.region()) {}
};
std::vector<std::uint32_t> words(const ps::Value& value) {
  std::vector<std::uint32_t> result(value.descriptor().shape[0]);
  std::memcpy(result.data(),
              value.bytes().data() + take(value.byte_address({0})),
              result.size() * sizeof(std::uint32_t));
  return result;
}
std::uint32_t bits(float f) {
  std::uint32_t result;
  std::memcpy(&result, &f, 4);
  return result;
}
void verify(const std::vector<float>& input,
            const std::vector<std::uint32_t>& out) {
  for (std::size_t i = 0; i < input.size(); ++i) {
    const auto expected =
        bits(static_cast<float>(std::exp(static_cast<double>(input[i]))));
    const auto distance =
        std::llabs(static_cast<std::int64_t>(out[i]) - expected);
    require(distance <= 4, "benchmark libm smoke check");
  }
}
ps::OperationDefinition operation() {
  return numeric::point_math_operation<numeric::CertifiedMath>(
      "numeric.exp_measurement", numeric::CertifiedKind::Exp,
      selected_profile(), 1, 12);
}
void acceptance(const std::string& path) {
  // File consists of pairs of input/MPFR correctly-rounded output uint32 words.
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "open oracle corpus");
  std::vector<float> inputs;
  std::vector<std::uint32_t> references;
  std::uint32_t pair[2];
  while (stream.read(reinterpret_cast<char*>(pair), sizeof(pair))) {
    float x;
    std::memcpy(&x, &pair[0], 4);
    inputs.push_back(x);
    references.push_back(pair[1]);
  }
  require(!inputs.empty(), "nonempty corpus");
  const auto op = operation();
  std::uint64_t maximum = 0;
  numeric::CertifiedMath single(selected_profile());
  std::vector<std::uint32_t> partition_reference(inputs.size());
  for (const auto chunk : {1U, 3U, 7U, 64U, 65U, 257U}) {
    for (std::size_t offset = 0; offset < inputs.size(); offset += chunk) {
      const auto n = std::min<std::size_t>(chunk, inputs.size() - offset);
      std::vector<float> data(inputs.begin() + offset,
                              inputs.begin() + offset + n);
      auto value = input_value(data);
      Call call(value);
      const auto out = words(take(op.callback(call.invocation)));
      for (std::size_t i = 0; i < n; ++i) {
        if (chunk == 1) {
          partition_reference[offset + i] = out[i];
          const auto direct = take(single.evaluate(
              numeric::CertifiedKind::Exp, ps::ElementType::Float32,
              bits(data[i]), 0,
              [](std::uint64_t) { return ps::Status::success(); },
              [] { return ps::Status::success(); }));
          require(direct == out[i], "scalar and SIMD exp bit identity");
        } else {
          require(partition_reference[offset + i] == out[i],
                  "partition bit identity");
        }
        const auto source = bits(data[i]) & 0x7fffffffU;
        const auto expected = references[offset + i];
        if (source > 0x42a00000U || source == 0) {
          require(out[i] == expected, "oracle strict fallback/special bits");
        } else {
          const auto distance =
              std::llabs(static_cast<std::int64_t>(out[i]) - expected);
          maximum = std::max(maximum, static_cast<std::uint64_t>(distance));
          require(distance <= 4, "MPFR four-step bound");
        }
      }
    }
  }
  // Mixed layouts and caller fenv must preserve identical per-lane output bits.
  std::vector<float> normal(
      inputs.begin(),
      inputs.begin() + std::min<std::size_t>(65, inputs.size()));
  std::vector<std::uint32_t> reference;
  for (unsigned layout = 0; layout < 3; ++layout) {
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      auto value = input_value(normal, layout);
      Call call(value);
      std::fesetround(mode);
      std::feclearexcept(FE_ALL_EXCEPT);
      std::feraiseexcept(FE_DIVBYZERO);
      const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
      const auto out = words(take(op.callback(call.invocation)));
      require(std::fegetround() == mode &&
                  std::fetestexcept(FE_ALL_EXCEPT) == flags,
              "restore fenv");
      if (reference.empty())
        reference = out;
      for (std::size_t i = 0; i < out.size(); ++i)
        require(out[i] == reference[layout == 2 ? 0 : i],
                "layout/fenv identity");
    }
  }
  std::fesetround(FE_TONEAREST);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::vector<float> resource_inputs(65536, 1.0f);
#if defined(__aarch64__)
  const auto public_profile = ps::CpuNumericProfile::AppleSiliconNeon;
#else
  const auto public_profile = ps::CpuNumericProfile::X86Avx2;
#endif
  auto node = take(
      ps::numeric::exp_node(1, ps::WorkflowInputReference{1}, public_profile));
  point_math_checks::resources(node, {input_value(resource_inputs)},
                               resource_inputs.size() * 4);
  // Compute expected admission independently of batch scheduling; fallback
  // refinement is charged by the existing certified engine exactly once.
  std::vector<float> mixed(inputs.begin(), inputs.begin() + 16);
  const auto mixed_value = input_value(mixed);
  std::uint64_t charged = 2 + 2 * mixed.size();
  numeric::CertifiedMath math(selected_profile());
  for (const auto value : mixed) {
    const auto word = bits(value);
    if ((word & UINT32_C(0x7fffffff)) <= UINT32_C(0x42a00000)) {
      charged +=
          numeric::DirectedInterval::kSlots * numeric::DirectedInterval::kWords;
    } else {
      take(math.evaluate(
          numeric::CertifiedKind::Exp, ps::ElementType::Float32, word, 0,
          [&](std::uint64_t work) {
            charged += work;
            return ps::Status::success();
          },
          [] { return ps::Status::success(); }));
    }
  }
  for (const bool enough : {true, false}) {
    ps::ResourceLimits limits;
    limits.maximum_work = enough ? charged : charged - 1;
    ps::ResourceBudget budget(limits);
    {
      ps::ResourceAllocationScope scope(budget);
      Call call(mixed_value);
      call.invocation.allocator = budget.allocator();
      const auto result = op.callback(call.invocation);
      if (enough) {
        require(result.ok() && budget.statistics().issued.work == charged,
                "fallback admission charged once at exact budget");
      } else {
        require(!result.ok() &&
                    result.status().code == ps::ErrorCode::ResourceExhausted,
                "mixed budget rejection threshold");
      }
    }
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "mixed success/failure release");
  }
  std::cout << "PASS " << inputs.size()
            << " MPFR cases x 6 partitions, maximum_steps=" << maximum
            << "; layouts/fenv/resources/cancellation passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "check") {
      acceptance(argv[2]);
      return 0;
    }
    require(argc >= 5,
            "usage: layer(public|core|raw) N range(10|80|mixed) repetitions");
    const std::string layer = argv[1], range = argv[3];
    const auto n = std::stoull(argv[2]);
    const auto repeats = std::stoul(argv[4]);
    require(n > 0 && repeats > 0, "N and repetitions must be positive");
    require(layer == "public" || layer == "core" || layer == "raw", "layer");
    require(range == "10" || range == "80" || range == "mixed", "range");
    std::mt19937 random(404);
    std::vector<float> input(n), raw(n);
    const float span = range == "80" ? 80.0f : 10.0f;
    for (auto& x : input)
      x = (static_cast<double>(random()) / UINT32_MAX * 2 - 1) * span;
    if (range == "mixed") {
      for (std::size_t i = 0; i < n; i += 16)
        input[i] = -90;
    }
    auto value = input_value(input, 3);
    auto registry = std::make_shared<ps::OperationRegistry>();
    require(registry->register_operation(operation()).ok(),
            "register measurement");
    require(registry->freeze().ok(), "freeze measurement registry");
    ps::WorkflowDocument document;
    document.inputs = {
        {1, "x", value.descriptor(), value.region(), value.layout(), {}}};
    ps::WorkflowNode node;
    node.id = 1;
    node.operation = "numeric.exp_measurement";
    node.inputs = {ps::WorkflowInputReference{1}};
    document.nodes = {node};
    document.outputs = {{"values", 1, "values"}};
    ps::GraphContext graph(document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = UINT64_C(1) << 30;
    ps::ExecutionContext context(registry, config);
    ps::ExecutionBindings bindings;
    bindings.inputs = {{"x", value}};
    auto snapshot = take(context.freeze(plan.plan, bindings));
    ps::DemandQuery query{{"values", take(ps::Footprint::all({n}))}};
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(1) << 50;
    options.dependencies.maximum_work = UINT64_C(1) << 50;
    options.dependencies.sets.maximum_work = UINT64_C(1) << 50;
    options.maximum_dependency_cache_work = 0;
    Call call(value);
    auto op = operation();
    std::vector<double> times;
    std::uint64_t peak = 0, checksum = 0;
    for (unsigned repeat = 0; repeat <= repeats; ++repeat) {
      ps::Value output;
      const auto start = std::chrono::steady_clock::now();
      if (layer == "public") {
        auto run =
            take(context.execute_fragments(snapshot, query, {}, options));
        output = run.values.at("values").fragments().at(0);
        peak = std::max(peak, run.diagnostics.peak_live_bytes);
      } else if (layer == "core") {
        output = take(op.callback(call.invocation));
      } else {
        ps::input_internal::Float32Environment environment;
        require(range != "mixed", "raw IQK domain");
        numeric::exp_simd_f32(input.data(), raw.data(), n);
      }
      const auto elapsed = std::chrono::duration<double, std::micro>(
                               std::chrono::steady_clock::now() - start)
                               .count();
      if (repeat)
        times.push_back(elapsed);
      if (repeat && argc > 5 && std::string(argv[5]) == "profile")
        continue;
      std::vector<std::uint32_t> out;
      if (layer == "raw") {
        out.resize(n);
        std::memcpy(out.data(), raw.data(), n * 4);
      } else {
        out = words(output);
      }
      verify(input, out);
      checksum += out[n / 2];
    }
    std::sort(times.begin(), times.end());
    std::cout << "iqk," << layer << ',' << n << ',' << range << ',' << repeats
              << ',' << times[times.size() / 2] << ',' << times.front() << ','
              << times.back() << ','
              << (layer == "public" ? std::to_string(peak) : "N/A") << ','
              << checksum << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
