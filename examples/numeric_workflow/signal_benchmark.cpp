#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "accuracy.hpp"  // NOLINT(build/include_subdir)
#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/ops/numeric/inverse_curves.hpp"
#include "photospider/ops/numeric/lowpass.hpp"
#include "photospider/photospider.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ps::SchemaTemplate source_schema(const ps::Value& value) {
  ps::SchemaTemplate schema;
  schema.id = "benchmark.input";
  ps::ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = value.descriptor();
  tensor.facets = value.facets();
  for (const auto& facet : tensor.facets)
    if (facet.key == "photospider.color-array")
      tensor.atomic_trailing_axes = 1;
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
ps::ResultRef source(const ps::ResourceBudget& root, const ps::Value& value,
                     const ps::SchemaTemplate& schema) {
  auto builder = take(ps::ResultBuilder::start(
      root, schema, "benchmark.source", {}, {}, 128, 128, value.resources()));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "benchmark source descriptor");
  // Immutable caller storage is admitted as Referenced without copying it
  // into the execution Root's Payload capacity.
  require(builder
              .publish_tensor(
                  0, value.region(), value.layout(), value.storage(),
                  take(ps::ResultRelation::cartesian(
                      root, take(schema.tensors[0].sample_count()), {})),
                  {true, true, true, true})
              .ok(),
          "benchmark source Result publication");
  return take(builder.seal());
}
ps::ExecutionBindings bind_sources(const ps::ResourceBudget& root,
                                   const std::vector<ps::Value>& sources,
                                   const ps::WorkflowDocument& document) {
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < sources.size(); ++i)
    bindings.inputs.push_back(
        {document.inputs[i].name,
         source(root, sources[i], *document.inputs[i].result_schema)});
  return bindings;
}
std::uint64_t read_bits(const ps::ResultRef& result,
                        const ps::ResultDescriptor& descriptor,
                        const std::vector<std::uint64_t>& at,
                        std::size_t width) {
  std::vector<ps::RegionDimension> dimensions;
  for (auto coordinate : at)
    dimensions.push_back({coordinate, 1});
  const auto window = take(
      result.acquire_tensor(descriptor, 0, ps::Region(std::move(dimensions))));
  const auto row = take(window.row_run(at));
  require(
      width == ps::Value::element_size(window.spec().descriptor.element_type),
      "benchmark sample width");
  std::uint64_t bits = 0;
  std::memcpy(&bits, row.data, width);
  return bits;
}
ps::Value array(ps::ElementType type, const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& bits) {
  const auto width = ps::Value::element_size(type);
  auto buffer = take(ps::BufferAllocator{}.allocate(bits.size() * width));
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(buffer.data() + i * width, &bits[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = width;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                      {0, strides},
                                      std::move(buffer).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> sources;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& value = inputs[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back(
          {i + 1, name,
           std::make_shared<ps::SchemaTemplate>(source_schema(value))});
      sources.push_back(value);
    }
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(const ps::DemandQuery& query,
                                   bool cache = true,
                                   std::uint64_t proof_work = UINT64_C(64) *
                                                              1024 * 1024,
                                   std::uint64_t cache_bytes = 65536) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 4 * 1024 * 1024;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto bindings =
        bind_sources(take(context.resource_budget()), sources, document);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& samples) {
  std::vector<std::uint64_t> bits;
  for (auto value : samples)
    bits.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, bits);
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
ps::WorkflowNode authored(bool continuous, unsigned kernel,
                          ps::CpuNumericProfile profile, unsigned axis = 0) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  if (continuous) {
    if (kernel == 3)
      return take(lowpass_nonuniform_kaiser_sinc_node(
          1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
          .5, .25, 2, LowpassBoundary::Reflect, profile));
    const auto helper = kernel == 0   ? lowpass_nonuniform_hann_sinc_node
                        : kernel == 1 ? lowpass_nonuniform_hamming_sinc_node
                        : kernel == 2 ? lowpass_nonuniform_blackman_sinc_node
                                      : lowpass_nonuniform_gaussian_node;
    return take(helper(
        1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
        .5, kernel == 4 ? 1. : .25, LowpassBoundary::Reflect, profile));
  }
  if (kernel == 3)
    return take(lowpass_uniform_kaiser_sinc_node(
        1, ps::WorkflowInputReference{1}, axis, 2, .25, 2,
        LowpassBoundary::Reflect, profile));
  const auto helper = kernel == 0   ? lowpass_uniform_hann_sinc_node
                      : kernel == 1 ? lowpass_uniform_hamming_sinc_node
                      : kernel == 2 ? lowpass_uniform_blackman_sinc_node
                                    : lowpass_uniform_gaussian_node;
  return take(helper(1, ps::WorkflowInputReference{1}, axis, 2,
                     kernel == 4 ? 1. : .25, LowpassBoundary::Reflect,
                     profile));
}
void benchmark(ps::CpuNumericProfile profile, const std::string& selected,
               const std::string& filter, bool inverse_float32,
               unsigned repetitions) {
  require(repetitions > 0, "positive repetitions");
  std::cout << "operation,profile,input_shape,requested,dtype,region,workers,"
               "cache,repetitions,median_us,max_us,output_bytes,peak_payload,"
               "peak_metadata,retained_payload,retained_metadata,source_"
               "elements,evaluated,fallbacks\n";
  for (unsigned operation = 0; operation < 12; ++operation)
    for (unsigned count : {1, 256}) {
      const bool narrow = inverse_float32 && operation < 2;
      const unsigned width = narrow ? 4 : 8;
      std::vector<ps::Value> inputs;
      ps::WorkflowNode node;
      std::vector<ps::Region> regions;
      std::vector<std::uint64_t> expected;
      std::uint64_t size = 0;
      std::string output = "values";
      if (operation < 2) {
        std::vector<double> x, y, q;
        for (unsigned i = 0; i < 33; ++i) {
          x.push_back(i);
          y.push_back(i);
        }
        for (unsigned i = 0; i < count; ++i) {
          q.push_back((i + .5) * 16 / count + .125);
          expected.push_back(raw(q.back()));
        }
        inputs = {doubles({33}, x), doubles({33}, y), doubles({count}, q)};
        node = take((operation ? ps::numeric::invert_pchip_node
                               : ps::numeric::invert_linear_node)(
            1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
            ps::WorkflowInputReference{3},
            narrow ? ps::ElementType::Float32 : ps::ElementType::Float64,
            ps::numeric::CurveDomain::Reject, profile));
        size = count;
        regions = {ps::Region({{0, count}})};
      } else {
        const bool continuous = operation >= 7;
        const unsigned kernel = (operation - 2) % 5;
        size = 4 * count + 1;
        std::vector<double> x(size), y(size, 0);
        for (unsigned i = 0; i < size; ++i)
          x[i] = i;
        for (unsigned i = 0; i < count; ++i) {
          y[4 * i + 2] = 1;
          regions.emplace_back(
              std::vector<ps::RegionDimension>{{4 * i + 2, 1}});
        }
        if (continuous)
          inputs.push_back(doubles({size}, x));
        inputs.push_back(doubles({size}, y));
        node = authored(continuous, kernel, profile);
        if (kernel == 3)
          node.parameters["beta"] = 0.;
        if (continuous)
          output = "samples";
        const std::array<std::uint64_t, 5> uniform{
            UINT64_C(0x3fe38d7050d05568), UINT64_C(0x3fe2f660651f7f7c),
            UINT64_C(0x3fe655124d269c1c), UINT64_C(0x3fdc2755e149a310),
            UINT64_C(0x3fd9c486742831f7)};
        const std::array<std::uint64_t, 5> nonuniform{
            UINT64_C(0x3feb4acc471e0946), UINT64_C(0x3fead54b401f5288),
            UINT64_C(0x3febe5c34bfc2c4e), UINT64_C(0x3fe8236d96d18592),
            UINT64_C(0x3fe82a4d23df6f86)};
        expected.assign(count, (continuous ? nonuniform : uniform)[kernel]);
      }
      if (!filter.empty() && node.operation.find(filter) == std::string::npos)
        continue;
      Fixture fixture(node, inputs);
      fixture.document.outputs = {{"result", 1, output}};
      auto wanted = region({size}, std::move(regions));
      ps::ResourceBudget budget;
      ps::ResultRef retained;
      std::vector<std::int64_t> times;
      std::uint64_t source_elements = 0;
      {
        ps::GraphContext graph(fixture.document);
        auto plan = take(ps::Compiler(fixture.registry).compile(graph));
        ps::ExecutionContextConfig config;
        config.cpu_workers = 1;
        config.result_cache_bytes = 0;
        config.maximum_live_bytes = 64 * 1024 * 1024;
        config.managed_resources = ps::ResourceLimits{};
        ps::ExecutionContext context(fixture.registry, config);
        budget = take(context.resource_budget());
        auto bindings = bind_sources(budget, fixture.sources, fixture.document);
        auto frozen = take(context.freeze(plan.plan, bindings));
        ps::ExecutionOptions options;
        options.maximum_dependency_work = UINT64_C(64) * 1024 * 1024 * 1024;
        options.dependencies.maximum_work = UINT64_C(32) * 1024 * 1024 * 1024;
        options.maximum_dependency_cache_work = 0;
        for (unsigned repeat = 0; repeat <= repetitions; ++repeat) {
          retained = {};
          const auto start = std::chrono::steady_clock::now();
          auto result = take(context.execute_fragments(
              frozen, {{"result", wanted}}, {}, options));
          if (repeat)
            times.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
          source_elements = 0;
          for (const auto& support : take(result.dependencies.source_support()))
            source_elements += take(support.second.element_count());
          const auto& output_result = result.results.at("result");
          const auto descriptor = take(output_result.descriptor());
          for (unsigned i = 0; i < count; ++i) {
            auto bits = read_bits(output_result, descriptor,
                                  {operation < 2 ? i : 4 * i + 2}, width);
            if (narrow) {
              const auto word = static_cast<std::uint32_t>(bits);
              float value;
              std::memcpy(&value, &word, 4);
              bits = raw(static_cast<double>(value));
            }
            // The collinear inverse queries are exactly representable in
            // Float32; require exact equality for this analytic fixture.
            require(narrow ? bits == expected[i]
                           : numeric_accuracy(bits, expected[i], profile),
                    "independent analytic benchmark bits");
          }
          retained = output_result;
        }
      }
      std::sort(times.begin(), times.end());
      const auto stats = budget.statistics();
      std::cout << node.operation << ',' << selected << ','
                << (operation < 2 ? 33 : size) << ',' << count
                << (narrow ? ",Float32," : ",Float64,")
                << (operation < 2 ? "Whole" : "disjoint") << ",1,off,"
                << repetitions << ',' << times[times.size() / 2] << ','
                << times.back() << ',' << count * width << ','
                << stats.peak[ps::ResourceKind::Payload] << ','
                << stats.peak[ps::ResourceKind::Metadata] << ','
                << stats.live[ps::ResourceKind::Payload] << ','
                << stats.live[ps::ResourceKind::Metadata] << ','
                << source_elements << ",N/A,N/A" << '\n'
                << std::flush;
      retained = {};
      require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
                  budget.statistics().live[ps::ResourceKind::Metadata] == 0,
              "benchmark owner release");
    }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    benchmark(profile, selected, argc > 2 ? argv[2] : "",
              argc > 3 && std::string(argv[3]) == "float32",
              argc > 4 ? std::stoul(argv[4]) : 7);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
