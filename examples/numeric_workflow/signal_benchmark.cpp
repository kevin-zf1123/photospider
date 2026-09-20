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

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/inverse_curves.hpp"
#include "photospider/numeric/lowpass.hpp"
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
  ps::ExecutionBindings bindings;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& value = inputs[i];
      const auto name = "input" + std::to_string(i);
      document.inputs.push_back({i + 1, name, value.descriptor(),
                                 value.region(), value.layout(),
                                 value.facets()});
      bindings.inputs.push_back({name, value});
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
void supply(const std::shared_ptr<ps::DependencySession>& session,
            const ps::DependencyRequest& request,
            const std::vector<ps::Value>& inputs) {
  std::vector<ps::Footprint> wanted;
  for (const auto& input : inputs)
    wanted.push_back(take(ps::Footprint::none(input.descriptor().shape)));
  for (const auto& need : take(session->pending_reads()))
    wanted[need.port] = take(wanted[need.port].unite(need.samples));
  std::vector<ps::ValueFragments> supplied;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    auto all = take(ps::ValueFragments::create(
        inputs[i].descriptor(), inputs[i].facets(),
        take(ps::Footprint::all(inputs[i].descriptor().shape)), {inputs[i]}));
    supplied.push_back(take(all.restrict(wanted[i])));
  }
  require(session->supply(supplied, request.snapshot_identity).ok(),
          "curve exact supply");
}
ps::ValueFragments direct(
    const std::shared_ptr<ps::OperationRegistry>& registry,
    const ps::WorkflowNode& node, const std::vector<ps::Value>& inputs,
    const ps::Footprint& outputs) {
  ps::DependencyRequest request;
  for (const auto& v : inputs)
    request.inputs.push_back({v.descriptor(), v.facets()});
  request.parameters = node.parameters;
  request.snapshot_identity = "curve-direct";
  request.outputs = outputs;
  request.limits.maximum_work = UINT64_C(2048) * 1024 * 1024;
  ps::ResourceBudget budget(ps::ResourceLimits{});
  auto session = take(
      registry->start_dependency(node.operation, request, budget.allocator()));
  for (;;) {
    auto progress = take(session->poll());
    if (auto* result = std::get_if<ps::DependencyResult>(&progress))
      return result->value;
    supply(session, request, inputs);
  }
}
ps::Value reversed_unaligned(const ps::Value& value) {
  auto bytes = value.bytes();
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = bytes.size() / width;
  auto owner = take(ps::BufferAllocator{}.allocate(bytes.size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(owner.data() + 1 + (count - 1 - i) * width,
                bytes.data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (std::size_t i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(owner).freeze()));
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
void benchmark(ps::CpuNumericProfile profile, const std::string& selected) {
  std::cout << "operation,profile,input_shape,requested,dtype,region,workers,"
               "cache,repetitions,median_us,max_us,output_bytes,peak_payload,"
               "peak_metadata,retained_payload,retained_metadata,source_"
               "elements,evaluated,fallbacks\n";
  for (unsigned operation = 0; operation < 12; ++operation)
    for (unsigned count : {1, 256}) {
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
            ps::WorkflowInputReference{3}, ps::ElementType::Float64,
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
      Fixture fixture(node, inputs);
      fixture.document.outputs = {{"result", 1, output}};
      auto wanted = region({size}, std::move(regions));
      ps::ResourceBudget budget;
      ps::ValueFragments retained;
      std::vector<std::int64_t> times;
      std::uint64_t source_elements = 0, evaluated = 0, fallbacks = 0;
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
        auto frozen = take(context.freeze(plan.plan, fixture.bindings));
        ps::ExecutionOptions options;
        options.maximum_dependency_work = UINT64_C(64) * 1024 * 1024 * 1024;
        options.dependencies.maximum_work = UINT64_C(32) * 1024 * 1024 * 1024;
        options.maximum_dependency_cache_work = 0;
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
          retained = {};
          const auto start = std::chrono::steady_clock::now();
          auto result = take(context.execute_fragments(
              frozen, {{"result", wanted}}, {}, options));
          times.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
          source_elements = evaluated = fallbacks = 0;
          for (const auto& support : take(result.dependencies.source_support()))
            source_elements += take(support.second.element_count());
          for (const auto& timing : result.diagnostics.operation_timings) {
            evaluated += timing.numeric.evaluated_values;
            fallbacks += timing.numeric.strict_fallbacks;
          }
          require(evaluated == count, "benchmark output accounting");
          require(fallbacks == (profile == ps::CpuNumericProfile::Strict ||
                                        operation == 0
                                    ? 0U
                                    : count),
                  "benchmark fallback accounting");
          for (unsigned i = 0; i < count; ++i) {
            std::uint64_t bits = 0;
            require(result.values.at("result")
                            .read({operation < 2 ? i : 4 * i + 2}, &bits, 8)
                            .ok() &&
                        bits == expected[i],
                    "independent analytic benchmark bits");
          }
          retained = result.values.at("result");
        }
      }
      std::sort(times.begin(), times.end());
      const auto stats = budget.statistics();
      std::cout << node.operation << ',' << selected << ','
                << (operation < 2 ? 33 : size) << ',' << count << ",Float64,"
                << (operation < 2 ? "Whole" : "disjoint") << ",1,off,3,"
                << times[1] << ',' << times[2] << ',' << count * 8 << ','
                << stats.peak[ps::ResourceKind::Payload] << ','
                << stats.peak[ps::ResourceKind::Metadata] << ','
                << stats.live[ps::ResourceKind::Payload] << ','
                << stats.live[ps::ResourceKind::Metadata] << ','
                << source_elements << ',' << evaluated << ',' << fallbacks
                << '\n'
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
    benchmark(profile, selected);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
