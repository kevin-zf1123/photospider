// NUM-04 trig acceptance and timings: Result workflow, computation poll and raw
// math.
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
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/numeric_math_operation.hpp"
#include "math_benchmark_result.hpp"  // NOLINT(build/include_subdir)
#include "photospider/numeric/unary.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)

namespace {
namespace numeric = ps::plugin_internal::numeric_ops;
unsigned selected_kind = 2;
const char* selected_name = "sin";
void select(const std::string& name) {
  const char* names[] = {"sin", "cos", "sinpi", "cospi", "sinc", "sincpi"};
  const unsigned kinds[] = {2, 3, 5, 6, 8, 9};
  for (unsigned i = 0; i < 6; ++i) {
    if (name == names[i]) {
      selected_kind = kinds[i];
      selected_name = names[i];
      return;
    }
  }
  throw std::runtime_error("unknown trig function");
}
std::int64_t ordered(std::uint32_t bits) {
  return bits & UINT32_C(0x80000000)
             ? INT64_C(0x80000000) - (bits & UINT32_C(0x7fffffff))
             : INT64_C(0x80000000) + bits;
}
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
std::uint32_t bits(float f) {
  std::uint32_t result;
  std::memcpy(&result, &f, 4);
  return result;
}
void verify(const std::vector<float>& input,
            const std::vector<std::uint32_t>& out) {
  for (std::size_t i = 0; i < input.size(); ++i) {
    const double x = input[i], pi = 0x1.921fb54442d18p1;
    const double reference =
        selected_kind == 2       ? std::sin(x)
        : selected_kind == 3     ? std::cos(x)
        : selected_kind == 5     ? std::sin(pi * x)
        : selected_kind == 6     ? std::cos(pi * x)
        : selected_kind == 8     ? (x == 0 ? 1 : std::sin(x) / x)
        : x == 0                 ? 1
        : x == std::nearbyint(x) ? 0
                                 : std::sin(pi * x) / (pi * x);
    const auto expected = bits(static_cast<float>(reference));
    const auto distance = std::llabs(ordered(out[i]) - ordered(expected));
    require(distance <= 4, "benchmark libm smoke check");
  }
}
ps::OperationDefinition operation() {
  return numeric::point_math_operation<numeric::CertifiedMath>(
      "numeric.trig_measurement",
      static_cast<numeric::CertifiedKind>(selected_kind), selected_profile(), 1,
      12);
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
  auto control = std::make_shared<point_math_checks::Control>();
  math_benchmark_result::Workflow workflow(operation(), control);
  std::uint64_t maximum = 0;
  numeric::CertifiedMath single(selected_profile());
  std::vector<std::uint32_t> partition_reference(inputs.size());
  for (const auto chunk : {1U, 3U, 7U, 64U, 65U, 257U}) {
    for (std::size_t offset = 0; offset < inputs.size(); offset += chunk) {
      const auto n = std::min<std::size_t>(chunk, inputs.size() - offset);
      std::vector<float> data(inputs.begin() + offset,
                              inputs.begin() + offset + n);
      auto value = input_value(data);
      workflow.bind(value);
      const auto out = math_benchmark_result::words(take(workflow.run()));
      for (std::size_t i = 0; i < n; ++i) {
        if (chunk == 1) {
          partition_reference[offset + i] = out[i];
          const auto direct = take(single.evaluate(
              static_cast<numeric::CertifiedKind>(selected_kind),
              ps::ElementType::Float32, bits(data[i]), 0,
              [](std::uint64_t) { return ps::Status::success(); },
              [] { return ps::Status::success(); }));
          require(direct == out[i], "scalar and SIMD trig bit identity");
        } else {
          require(partition_reference[offset + i] == out[i],
                  "partition bit identity");
        }
        const auto expected = references[offset + i];
        const auto magnitude = expected & UINT32_C(0x7fffffff);
        if (!magnitude || magnitude >= UINT32_C(0x7f800000) ||
            magnitude < UINT32_C(0x00800000)) {
          require(out[i] == expected, "oracle special/subnormal bits");
        } else {
          const auto distance = std::llabs(ordered(out[i]) - ordered(expected));
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
      workflow.bind(value);
      control->rounding = mode;
      std::fesetround(mode);
      std::feclearexcept(FE_ALL_EXCEPT);
      std::feraiseexcept(FE_DIVBYZERO);
      const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
      const auto out = math_benchmark_result::words(take(workflow.run()));
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
  control->rounding.reset();
  std::fesetround(FE_TONEAREST);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::vector<float> resource_inputs(65536, .125f);
#if defined(__aarch64__)
  const auto public_profile = ps::CpuNumericProfile::AppleSiliconNeon;
#else
  const auto public_profile = ps::CpuNumericProfile::X86Avx2;
#endif
  ps::Result<ps::WorkflowNode> built =
      selected_kind == 2 ? ps::numeric::sin_node(
                               1, ps::WorkflowInputReference{1}, public_profile)
      : selected_kind == 3
          ? ps::numeric::cos_node(1, ps::WorkflowInputReference{1},
                                  public_profile)
      : selected_kind == 5
          ? ps::numeric::sinpi_node(1, ps::WorkflowInputReference{1},
                                    public_profile)
      : selected_kind == 6
          ? ps::numeric::cospi_node(1, ps::WorkflowInputReference{1},
                                    public_profile)
      : selected_kind == 8
          ? ps::numeric::sinc_node(1, ps::WorkflowInputReference{1},
                                   public_profile)
          : ps::numeric::sincpi_node(1, ps::WorkflowInputReference{1},
                                     public_profile);
  auto node = take(std::move(built));
  point_math_checks::resources(node, {input_value(resource_inputs)},
                               resource_inputs.size() * 4);
  // Compute expected admission independently of batch scheduling; fallback
  // refinement is charged by the existing certified engine exactly once.
  std::vector<float> mixed(
      inputs.begin(),
      inputs.begin() + std::min<std::size_t>(16, inputs.size()));
  const auto mixed_value = input_value(mixed);
  std::uint64_t charged = 2 + 2 * mixed.size();
  numeric::CertifiedMath math(selected_profile());
  for (const auto value : mixed) {
    const auto word = bits(value);
    take(math.evaluate(
        static_cast<numeric::CertifiedKind>(selected_kind),
        ps::ElementType::Float32, word, 0,
        [&](std::uint64_t work) {
          charged += work;
          return ps::Status::success();
        },
        [] { return ps::Status::success(); }));
  }
  workflow.bind(mixed_value);
  for (const bool enough : {true, false}) {
    control->maximum_work = enough ? charged : charged - 1;
    {
      const auto result = workflow.run();
      if (enough) {
        require(result.ok() && control->work == charged,
                "Result fallback work charged once at exact callback budget");
      } else {
        require(!result.ok() &&
                    result.status().code == ps::ErrorCode::ResourceExhausted &&
                    result.status().reason == ps::FailureReason::WorkLimit &&
                    control->computation_polls == 1,
                "Result mixed callback budget rejection threshold");
      }
    }
    require(workflow.root().statistics().live[ps::ResourceKind::Payload] == 0,
            "mixed success/failure releases temporary Result payload");
  }
  std::cout << "PASS " << inputs.size()
            << " MPFR cases x 6 partitions, maximum_steps=" << maximum
            << "; layouts/fenv/resources/cancellation passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--timing-scopes") {
      std::cout << "{\"public\":\"result_workflow\","
                   "\"core\":\"result_computation_poll\","
                   "\"raw\":\"simd_kernel\"}\n";
      return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "check") {
      select(argv[2]);
      acceptance(argv[3]);
      return 0;
    }
    require(argc >= 6,
            "usage: function layer N span repetitions [profile|measure] "
            "[normal|mixed|outside|landmark|tiny] [layout=3]");
    select(argv[1]);
    const std::string layer = argv[2], range = argv[4];
    const auto n = std::stoull(argv[3]);
    const auto repeats = std::stoul(argv[5]);
    require(n > 0 && repeats > 0, "N and repetitions must be positive");
    require(layer == "public" || layer == "core" || layer == "raw", "layer");
    const float span = std::stof(range);
    require(span > 0, "span");
    std::mt19937 random(404);
    std::vector<float> input(n), raw(n);
    for (auto& x : input)
      x = (static_cast<double>(random()) / UINT32_MAX * 2 - 1) * span;
    const std::string distribution = argc > 7 ? argv[7] : "normal";
    const unsigned layout = argc > 8 ? std::stoul(argv[8]) : 3;
    require(layout <= 3,
            "layout: 0 unaligned, 1 reverse, 2 broadcast, 3 dense");
    require(distribution == "normal" || distribution == "mixed" ||
                distribution == "outside" || distribution == "landmark" ||
                distribution == "tiny",
            "distribution");
    const float outside =
        selected_kind == 5 || selected_kind == 6 ? .375f : 2.0f;
    if (distribution == "mixed") {
      for (std::size_t i = 0; i < n; i += 16)
        input[i] = outside;
    } else if (distribution == "outside") {
      std::fill(input.begin(), input.end(), outside);
    } else if (distribution == "landmark") {
      const float landmark = selected_kind == 5 || selected_kind == 6 ? .25f
                             : selected_kind == 9                     ? 1.0f
                                                                      : 0.0f;
      std::fill(input.begin(), input.end(), landmark);
    } else if (distribution == "tiny") {
      std::fill(input.begin(), input.end(), 0x1p-130f);
    }
    if (layout == 2)
      std::fill(input.begin(), input.end(), input.front());
    require(layer != "raw" || (distribution == "normal" && layout == 3),
            "raw measurements require dense normal-domain inputs");
    auto value = input_value(input, layout);
    auto control = std::make_shared<point_math_checks::Control>();
    control->measure_computation = layer == "core";
    std::unique_ptr<math_benchmark_result::Workflow> workflow;
    if (layer != "raw") {
      workflow = std::make_unique<math_benchmark_result::Workflow>(
          operation(), layer == "core" ? control : nullptr);
      workflow->bind(value);
    }
    std::vector<double> times;
    std::uint64_t peak = 0, checksum = 0;
    for (unsigned repeat = 0; repeat <= repeats; ++repeat) {
      ps::ResultRef output;
      const auto start = std::chrono::steady_clock::now();
      if (layer != "raw") {
        output = take(workflow->run());
        if (layer == "public")
          peak = std::max(peak, workflow->peak_payload());
      } else {
        ps::input_internal::Float32Environment environment;
        require(selected_kind == 9 ||
                    span <= (selected_kind == 5 || selected_kind == 6 ? .25f
                                                                      : 1.0f),
                "raw domain");
#if defined(PHOTOSPIDER_TRIG_BASELINE)
        throw std::runtime_error("raw polynomial unavailable in baseline");
#else
        numeric::trig_simd_f32(selected_kind, input.data(), raw.data(), n);
#endif
      }
      const auto elapsed = layer == "core"
                               ? control->computation_us
                               : std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
      if (repeat)
        times.push_back(elapsed);
      if (repeat && argc > 6 && std::string(argv[6]) == "profile")
        continue;
      std::vector<std::uint32_t> out;
      if (layer == "raw") {
        out.resize(n);
        std::memcpy(out.data(), raw.data(), n * 4);
      } else {
        out = math_benchmark_result::words(output);
      }
      verify(input, out);
      checksum += out[n / 2];
    }
    std::sort(times.begin(), times.end());
    std::cout << selected_name << "," << layer << ',' << n << ',' << range
              << ',' << repeats << ',' << times[times.size() / 2] << ','
              << times.front() << ',' << times.back() << ','
              << (layer == "public" ? std::to_string(peak) : "N/A") << ','
              << checksum << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
