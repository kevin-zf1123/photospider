// NUM Result workflow equivalence, caller fenv and managed-resource checks.
#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/certified_math.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "photospider/photospider.hpp"

namespace {
namespace n = ps::plugin_internal::numeric_ops;
template <class T>
T take(ps::Result<T> r) {
  if (!r.ok())
    throw std::runtime_error(r.status().message);
  return r.take_value();
}
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct InputData final {
  ps::ValueDescriptor descriptor;
  ps::StridedLayout layout;
  std::vector<std::uint8_t> bytes;
};
InputData input(const std::vector<std::uint64_t>& bits, bool narrow,
                unsigned layout) {
  const unsigned width = narrow ? 4 : 8;
  std::vector<std::uint8_t> buffer(bits.size() * width + 1);
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(
        buffer.data() + 1 + (layout == 1 ? bits.size() - 1 - i : i) * width,
        &bits[i], width);
  return {{narrow ? ps::ElementType::Float32 : ps::ElementType::Float64,
           {bits.size()}},
          {layout == 1 ? 1 + (bits.size() - 1) * width : 1,
           {layout == 1   ? -static_cast<std::int64_t>(width)
            : layout == 2 ? 0
                          : static_cast<std::int64_t>(width)}},
          std::move(buffer)};
}
InputData matrix_input(const std::vector<std::uint64_t>& bits, bool narrow,
                       bool transposed) {
  const unsigned width = narrow ? 4 : 8;
  std::vector<std::uint8_t> buffer(64 * width + 1);
  for (unsigned i = 0; i < 64; ++i) {
    const unsigned offset = transposed ? (i % 16) * 4 + i / 16 : i;
    std::memcpy(buffer.data() + 1 + offset * width, &bits[i], width);
  }
  return {
      {narrow ? ps::ElementType::Float32 : ps::ElementType::Float64, {4, 16}},
      {1,
       {static_cast<std::int64_t>((transposed ? 1 : 16) * width),
        static_cast<std::int64_t>((transposed ? 4 : 1) * width)}},
      std::move(buffer)};
}
#if defined(__aarch64__)
constexpr auto kProfile = n::SequenceProfile::AppleSilicon;
#else
constexpr auto kProfile = n::SequenceProfile::X86Avx2;
#endif
const char* operation_name(n::CertifiedKind kind) {
  switch (kind) {
    case n::CertifiedKind::Exp:
      return "exp";
    case n::CertifiedKind::Ln:
      return "ln";
    case n::CertifiedKind::Sin:
      return "sin";
    case n::CertifiedKind::Cos:
      return "cos";
    case n::CertifiedKind::Tan:
      return "tan";
    case n::CertifiedKind::Pow:
      return "pow";
    case n::CertifiedKind::Atan2:
      return "atan2";
    default:
      throw std::runtime_error("unsupported batch fixture kind");
  }
}
struct BatchWorkflow final {
  std::unique_ptr<ps::ExecutionContext> context;
  ps::ResourceBudget root;
  std::shared_ptr<ps::GraphContext> graph;
  ps::FrozenExecution frozen;
  explicit BatchWorkflow(n::CertifiedKind kind,
                         const std::vector<InputData>& inputs,
                         ps::ResourceLimits limits = {}) {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = limits;
    auto registry = ps::make_default_operation_registry();
    context = std::make_unique<ps::ExecutionContext>(registry, config);
    root = take(context->resource_budget());
    ps::WorkflowDocument document;
    ps::WorkflowNode node;
    node.id = 7;
    node.operation = std::string("numeric.") + operation_name(kind) +
                     (kProfile == n::SequenceProfile::AppleSilicon
                          ? "_accelerated_apple_silicon"
                          : "_accelerated_x86_64");
    ps::ExecutionBindings bindings;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& data = inputs[i];
      ps::SchemaTemplate schema;
      schema.id = "example.math_batch.input";
      ps::ResultTensorSpec tensor;
      tensor.key = "data";
      tensor.descriptor = data.descriptor;
      schema.tensors.push_back(std::move(tensor));
      auto bytes = take(root.allocator().allocate(data.bytes.size()));
      std::memcpy(bytes.data(), data.bytes.data(), data.bytes.size());
      auto builder =
          take(ps::ResultBuilder::start(root, schema, "batch.input"));
      require(builder
                  .bind_descriptor_relation(
                      take(ps::ResultRelation::cartesian(root, 1, {})))
                  .ok(),
              "batch source descriptor");
      require(builder
                  .publish_tensor(
                      0, ps::Region::whole(data.descriptor.shape), data.layout,
                      std::move(bytes).freeze(),
                      take(ps::ResultRelation::cartesian(
                          root, take(schema.tensors[0].sample_count()), {})),
                      {true, true, true, true})
                  .ok(),
              "batch source publication");
      ps::WorkflowInputDeclaration declaration;
      declaration.id = i + 11;
      declaration.name = "input" + std::to_string(i);
      declaration.result_schema = std::make_shared<ps::SchemaTemplate>(schema);
      node.inputs.push_back(ps::WorkflowInputReference{declaration.id});
      bindings.inputs.push_back({declaration.name, take(builder.seal())});
      document.inputs.push_back(std::move(declaration));
    }
    document.nodes.push_back(std::move(node));
    document.outputs = {{"out", 7, "values"}};
    graph = std::make_shared<ps::GraphContext>(document);
    auto plan = take(ps::Compiler(registry).compile(*graph)).plan;
    frozen = take(context->freeze(plan, bindings));
  }
  ps::Result<ps::ExecutionResult> run(const ps::CancellationToken& stop = {}) {
    return context->execute(frozen, stop);
  }
};
std::vector<std::uint64_t> words(const ps::ResultRef& result) {
  const auto facts = take(result.descriptor());
  const auto& spec = result.schema().tensors[0];
  const auto shape = spec.sample_shape();
  const auto width = ps::Value::element_size(spec.descriptor.element_type);
  std::vector<std::uint64_t> out(take(spec.sample_count()), 0);
  std::vector<std::uint64_t> at(shape.size(), 0);
  for (auto& word : out) {
    require(result.read_tensor(facts, 0, at, &word, width).ok(),
            "batch output read");
    for (std::size_t axis = at.size(); axis-- > 0;) {
      if (++at[axis] < shape[axis])
        break;
      at[axis] = 0;
    }
  }
  return out;
}
void require_released(const ps::ResourceBudget& root) {
  for (auto live : root.statistics().live.values)
    require(live == 0, "released Result workflow resources");
}
void check_resources(n::CertifiedKind kind,
                     const std::vector<InputData>& inputs) {
  ps::ResourceStatistics baseline;
  ps::ResourceBudget root;
  ps::ResultRef held;
  {
    BatchWorkflow workflow(kind, inputs);
    root = workflow.root;
    auto answer = take(workflow.run());
    held = answer.results.at("out");
    baseline = root.statistics();
  }
  // Result payload and metadata stay owned after the worker context retires.
  require(!words(held).empty(), "Result read after context retirement");
  held = {};
  require_released(root);
  require(
      baseline.issued.work > 0 && baseline.peak[ps::ResourceKind::Payload] > 0,
      "charged Result work and allocation");
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    ps::ResourceLimits limits;
    limits.maximum_work = baseline.issued.work - (scenario == 1);
    limits.capacity[ps::ResourceKind::Payload] =
        baseline.peak[ps::ResourceKind::Payload] - (scenario == 2);
    {
      BatchWorkflow workflow(kind, inputs, limits);
      root = workflow.root;
      auto result = workflow.run();
      if (!(scenario == 0
                ? result.ok()
                : !result.ok() &&
                      result.status().code ==
                          ps::ErrorCode::ResourceExhausted &&
                      (scenario != 1 ||
                       result.status().reason == ps::FailureReason::WorkLimit)))
        throw std::runtime_error(
            "Result resource scenario=" + std::to_string(scenario) + " code=" +
            std::to_string(static_cast<unsigned>(result.status().code)) +
            " reason=" +
            std::to_string(static_cast<unsigned>(result.status().reason)) +
            " work=" + std::to_string(root.statistics().issued.work) + "/" +
            std::to_string(limits.maximum_work) + " payload=" +
            std::to_string(root.statistics().peak[ps::ResourceKind::Payload]) +
            "/" + std::to_string(limits.capacity[ps::ResourceKind::Payload]));
      if (scenario == 0)
        require(root.statistics().issued.work == baseline.issued.work &&
                    root.statistics().peak[ps::ResourceKind::Payload] ==
                        baseline.peak[ps::ResourceKind::Payload],
                "repeatable Result resource accounting");
    }
    require_released(root);
  }
  {
    BatchWorkflow workflow(kind, inputs);
    root = workflow.root;
    ps::CancellationSource stop;
    stop.cancel();
    require(
        workflow.run(stop.token()).status().code == ps::ErrorCode::Cancelled,
        "cancelled Result workflow");
  }
  require_released(root);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto available = n::sequence_profile_available(kProfile);
    if (!available.ok()) {
      std::cout << "SKIP " << available.message << '\n';
      return 77;
    }
    const bool timing = argc > 1 && std::string(argv[1]) == "time";
    const unsigned size =
        timing ? (argc > 2 ? std::stoul(argv[2]) : 262144) : 257;
    require(size > 0, "nonempty workload");
    std::mt19937 rng(405);
    std::cout << "kind,dtype,N,scalar_math_us,result_workflow_us\n";
    for (unsigned kind : {0U, 1U, 2U, 3U, 4U, 10U, 11U}) {
      for (bool narrow : {true, false}) {
        const auto dtype =
            narrow ? ps::ElementType::Float32 : ps::ElementType::Float64;
        const bool binary = kind >= 10;
        std::vector<std::uint64_t> a(size), b(size), expected(size);
        for (unsigned i = 0; i < size; ++i) {
          double x = .125 + .75 * static_cast<double>(rng()) / UINT32_MAX;
          a[i] = n::numeric_bits(x, narrow);
          b[i] = n::numeric_bits(.3125, narrow);
        }
        if (!timing) {
          const auto inf =
              narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
          const std::array<std::uint64_t, 8> special{
              0,
              UINT64_C(1) << (narrow ? 31 : 63),
              1,
              inf,
              inf | 42,
              n::numeric_bits(-1, narrow),
              n::numeric_bits(1, narrow),
              n::numeric_bits(2, narrow)};
          for (unsigned i = 0; i < special.size(); ++i) {
            a[i] = special[i];
            if (binary)
              b[i + special.size()] = special[i];
          }
          if (kind == 0) {
            for (unsigned i = 0; i < 2; ++i) {
              const auto edge = n::numeric_bits(i ? 80 : -80, narrow);
              a[16 + 3 * i] = edge - 1;
              a[17 + 3 * i] = edge;
              a[18 + 3 * i] = edge + 1;
            }
            a[22] = n::numeric_bits(-90, narrow);
            a[23] = n::numeric_bits(100, narrow);
            a[24] = n::numeric_bits(1 + 0x1p-30, narrow);
            a[25] = n::numeric_bits(1, narrow);
          }
        }
        n::CertifiedMath scalar(kProfile);
        std::vector<double> scalar_times;
        for (unsigned repeat = 0; repeat < (timing ? 8U : 1U); ++repeat) {
          const auto start = std::chrono::steady_clock::now();
          for (unsigned i = 0; i < size; ++i)
            expected[i] = take(scalar.evaluate(
                static_cast<n::CertifiedKind>(kind), dtype, a[i], b[i],
                [](std::uint64_t) { return ps::Status::success(); },
                [] { return ps::Status::success(); }));
          const auto elapsed = std::chrono::duration<double, std::micro>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
          if (repeat)
            scalar_times.push_back(elapsed);
        }
        if (!timing && kind == 0 && !narrow)
          require(expected[24] != expected[25],
                  "exp retains binary64 input precision");
        std::sort(scalar_times.begin(), scalar_times.end());
        std::vector<double> times;
        for (unsigned layout = 0; layout < (timing ? 1U : 3U); ++layout) {
          std::vector<InputData> inputs{input(a, narrow, layout)};
          if (binary)
            inputs.push_back(input(b, narrow, layout));
          BatchWorkflow workflow(static_cast<n::CertifiedKind>(kind), inputs);
          for (int mode :
               {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            if (timing && mode != FE_TONEAREST)
              continue;
            std::fesetround(mode);
            std::feclearexcept(FE_ALL_EXCEPT);
            std::feraiseexcept(FE_DIVBYZERO);
            const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
            for (unsigned repeat = 0; repeat < (timing ? 8U : 1U); ++repeat) {
              std::feclearexcept(FE_ALL_EXCEPT);
              std::feraiseexcept(FE_DIVBYZERO);
              auto before = std::chrono::steady_clock::now();
              auto output = take(workflow.run());
              require(std::fegetround() == mode &&
                          std::fetestexcept(FE_ALL_EXCEPT) == flags,
                      "fenv restoration");
              const auto duration =
                  std::chrono::duration<double, std::micro>(
                      std::chrono::steady_clock::now() - before)
                      .count();
              auto actual = words(output.results.at("out"));
              require(
                  actual.size() == size && output.diagnostics.cache_hits == 0,
                  "complete uncached Result workflow");
              for (unsigned i = 0; i < size; ++i)
                require(actual[i] == expected[layout == 2 ? 0 : i],
                        "batch/scalar identity including special values");
              if (repeat)
                times.push_back(duration);
            }
          }
        }
        std::fesetround(FE_TONEAREST);
        std::feclearexcept(FE_ALL_EXCEPT);
        if (!timing) {
          // Mixed packed/transposed binary ports still need shared coordinates;
          // all-packed multidimensional inputs may skip coordinate increments.
          for (bool transposed : {false, true}) {
            std::vector<InputData> inputs{matrix_input(a, narrow, transposed)};
            if (binary)
              inputs.push_back(matrix_input(b, narrow, !transposed));
            BatchWorkflow workflow(static_cast<n::CertifiedKind>(kind), inputs);
            const auto actual = words(take(workflow.run()).results.at("out"));
            require(actual.size() == 64, "matrix Result shape");
            for (unsigned i = 0; i < 64; ++i)
              require(actual[i] == expected[i], "matrix layout identity");
          }
          for (unsigned chunk : {1U, 3U, 7U, 64U, 65U}) {
            for (unsigned offset = 0; offset < size; offset += chunk) {
              const auto end = std::min(size, offset + chunk);
              std::vector<InputData> inputs{
                  input({a.begin() + offset, a.begin() + end}, narrow, 0)};
              if (binary)
                inputs.push_back(
                    input({b.begin() + offset, b.begin() + end}, narrow, 0));
              BatchWorkflow workflow(static_cast<n::CertifiedKind>(kind),
                                     inputs);
              auto actual = words(take(workflow.run()).results.at("out"));
              require(actual.size() == end - offset, "partition Result shape");
              for (unsigned i = offset; i < end; ++i)
                require(actual[i - offset] == expected[i],
                        "partition identity");
            }
          }
          std::vector<InputData> inputs{input(a, narrow, 0)};
          if (binary)
            inputs.push_back(input(b, narrow, 0));
          check_resources(static_cast<n::CertifiedKind>(kind), inputs);
        }
        std::sort(times.begin(), times.end());
        std::cout << kind << ',' << (narrow ? 32 : 64) << ',' << size << ','
                  << (scalar_times.empty()
                          ? 0
                          : scalar_times[scalar_times.size() / 2])
                  << ',' << (times.empty() ? 0 : times[times.size() / 2])
                  << '\n';
      }
    }
    std::cout << (timing
                      ? "PASS Result workflow/scalar timing and caller fenv\n"
                      : "PASS Result workflow/scalar, layouts, caller fenv, "
                        "limits and cancellation\n");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
