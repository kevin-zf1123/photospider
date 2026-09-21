#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/numeric/arrays.hpp"
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
    document.outputs = {{"samples", node.id, "samples"}};
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
ps::WorkflowNode authored(unsigned kernel, double radius, double parameter,
                          double beta, ps::numeric::LowpassBoundary boundary,
                          ps::CpuNumericProfile profile, unsigned axis = 0) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  if (kernel == 3)
    return take(lowpass_nonuniform_kaiser_sinc_node(
        1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
        radius, parameter, beta, boundary, profile));
  auto helper = kernel == 0   ? lowpass_nonuniform_hann_sinc_node
                : kernel == 1 ? lowpass_nonuniform_hamming_sinc_node
                : kernel == 2 ? lowpass_nonuniform_blackman_sinc_node
                              : lowpass_nonuniform_gaussian_node;
  return take(helper(1, ps::WorkflowInputReference{1},
                     ps::WorkflowInputReference{2}, axis, radius, parameter,
                     boundary, profile));
}
void oracle(ps::CpuNumericProfile profile) {
  unsigned kernel = 0, xt = 0, vt = 0, boundary = 0, n = 0;
  std::uint64_t radius = 0, parameter = 0, beta = 0;
  while (std::cin >> kernel >> xt >> vt >> boundary >> n >> std::hex >>
         radius >> parameter >> beta >> std::dec) {
    std::vector<std::uint64_t> positions(n), values(n);
    for (auto& value : positions)
      std::cin >> std::hex >> value >> std::dec;
    for (auto& value : values)
      std::cin >> std::hex >> value >> std::dec;
    double r = 0, p = 0, b = 0;
    std::memcpy(&r, &radius, 8);
    std::memcpy(&p, &parameter, 8);
    std::memcpy(&b, &beta, 8);
    Fixture fixture(
        authored(kernel, r, p, b,
                 static_cast<ps::numeric::LowpassBoundary>(boundary), profile),
        {array(static_cast<ps::ElementType>(xt), {n}, positions),
         array(static_cast<ps::ElementType>(vt), {n}, values)});
    auto result =
        fixture.run({{"samples", take(ps::Footprint::all({n}))}}, false);
    if (!result.ok()) {
      std::cout << (result.status().reason == ps::FailureReason::InvalidDomain
                        ? "domain"
                    : result.status().reason ==
                            ps::FailureReason::ArithmeticOverflow
                        ? "overflow"
                        : "error")
                << '\n';
      if (result.status().reason != ps::FailureReason::InvalidDomain &&
          result.status().reason != ps::FailureReason::ArithmeticOverflow)
        std::cerr << result.status().message << '\n';
      continue;
    }
    for (unsigned i = 0; i < n; ++i) {
      std::uint64_t actual = 0;
      require(result.value()
                  .values.at("samples")
                  .read({i}, &actual, vt == 4 ? 4 : 8)
                  .ok(),
              "continuous oracle read");
      if (i)
        std::cout << ' ';
      std::cout << std::hex << actual;
    }
    std::cout << std::dec << '\n';
  }
}
void fixtures(ps::CpuNumericProfile profile) {
  for (unsigned kernel = 0; kernel < 5; ++kernel) {
    Fixture affine(authored(kernel, .5, kernel == 4 ? 1 : .25, 4,
                            ps::numeric::LowpassBoundary::Reflect, profile),
                   {doubles({3}, {0, .75, 2}), doubles({3}, {1, 2.5, 5})});
    auto result = take(
        affine.run({{"samples", region({3}, {ps::Region({{1, 1}})})}}, false));
    std::uint64_t actual = 0;
    require(result.values.at("samples").read({1}, &actual, 8).ok() &&
                actual == raw(2.5),
            "affine center exact");
    Fixture inserted(
        authored(kernel, .5, kernel == 4 ? 1 : .25, 4,
                 ps::numeric::LowpassBoundary::Reflect, profile),
        {doubles({5}, {0, .5, .75, 1, 2}), doubles({5}, {1, 2, 2.5, 3, 5})});
    auto refined = take(inserted.run(
        {{"samples", region({5}, {ps::Region({{2, 1}})})}}, false));
    require(refined.values.at("samples").read({2}, &actual, 8).ok() &&
                actual == raw(2.5),
            "collinear knot insertion");
    for (bool narrow : {false, true}) {
      auto dtype = narrow ? ps::ElementType::Float32 : ps::ElementType::Float64;
      for (unsigned magnitude : {1, 3}) {
        Fixture half(
            authored(kernel, .5, kernel == 4 ? 1 : .25, 4,
                     ps::numeric::LowpassBoundary::Zero, profile),
            {doubles({2}, {0, 1}), array(dtype, {2}, {magnitude, magnitude})});
        auto midpoint =
            take(half.run({{"samples", take(ps::Footprint::all({2}))}}, false));
        for (unsigned i = 0; i < 2; ++i) {
          actual = 0;
          require(midpoint.values.at("samples")
                          .read({i}, &actual, narrow ? 4 : 8)
                          .ok() &&
                      actual == (magnitude == 1 ? 0U : 2U),
                  "continuous half-subnormal ties");
        }
      }
    }
    for (auto boundary : {ps::numeric::LowpassBoundary::Reflect,
                          ps::numeric::LowpassBoundary::Replicate,
                          ps::numeric::LowpassBoundary::Wrap,
                          ps::numeric::LowpassBoundary::Zero}) {
      Fixture constant(
          authored(kernel, .5, kernel == 4 ? 1 : .25, 4, boundary, profile),
          {doubles({2}, {0, 1}), doubles({2}, {2, 2})});
      auto preserved = take(
          constant.run({{"samples", take(ps::Footprint::all({2}))}}, false));
      for (unsigned i = 0; i < 2; ++i) {
        actual = 0;
        require(
            preserved.values.at("samples").read({i}, &actual, 8).ok() &&
                actual ==
                    raw(boundary == ps::numeric::LowpassBoundary::Zero ? 1 : 2),
            "continuous boundary normalization");
      }
    }
  }
  std::cout << "five continuous kernels: affine 2.5, collinear insertion, "
               "zero-boundary halves and Float32/64 subnormal ties passed\n";
}
void support_and_failures(ps::CpuNumericProfile profile) {
  const auto nan = UINT64_C(0x7ff0000000000042);
  for (unsigned kernel = 0; kernel < 5; ++kernel) {
    Fixture fixture(
        authored(kernel, 1, kernel == 4 ? 1 : .25, 4,
                 ps::numeric::LowpassBoundary::Zero, profile),
        {doubles({4}, {0, 1, 2, 3}),
         array(ps::ElementType::Float64, {4}, {raw(0), raw(1), raw(2), nan})});
    auto rejected =
        fixture.run({{"samples", region({4}, {ps::Region({{1, 1}})})}}, false);
    require(!rejected.ok() &&
                rejected.status().reason == ps::FailureReason::InvalidDomain &&
                rejected.status().detail.scope == ps::FailureScope::Run,
            "undelivered nonfinite sample fails Whole run");
    fixture.bindings.inputs[1].value = doubles({4}, {0, 1, 2, 3});
    auto result = take(
        fixture.run({{"samples", region({4}, {ps::Region({{1, 1}})})}}, false));
    auto support = take(result.dependencies.source_support());
    require(support.at("input0") == take(ps::Footprint::all({4})) &&
                support.at("input1") == take(ps::Footprint::all({4})),
            "Whole complete support");
    require(take(result.dependencies.potential_dirty(
                     "input1", region({4}, {ps::Region({{3, 1}})})))
                    .at("samples") == region({4}, {ps::Region({{1, 1}})}),
            "remote sample dirties Whole output");
    std::uint64_t actual = 0;
    require(result.values.at("samples").read({1}, &actual, 8).ok() &&
                actual == raw(1),
            "continuous affine interior result");
    fixture.bindings.inputs[1].value =
        array(ps::ElementType::Float64, {4}, {raw(0), raw(1), raw(2), nan});
    fixture.document.nodes[0].parameters["support_radius"] = 1.25;
    auto failed =
        fixture.run({{"samples", region({4}, {ps::Region({{1, 1}})})}}, false);
    require(!failed.ok() &&
                failed.status().reason == ps::FailureReason::InvalidDomain,
            "positive overlap validates previously untouched endpoint");
    fixture.document.nodes[0].parameters["support_radius"] = 1.;
    fixture.bindings.inputs[0].value =
        array(ps::ElementType::Float64, {4}, {0, raw(1), raw(2), nan});
    failed =
        fixture.run({{"samples", region({4}, {ps::Region({{1, 1}})})}}, false);
    require(!failed.ok() &&
                failed.status().reason == ps::FailureReason::InvalidDomain,
            "global remote position validation");
  }
  Fixture columns(
      authored(4, .5, 1, 0, ps::numeric::LowpassBoundary::Reflect, profile),
      {doubles({3}, {0, .75, 2}),
       array(ps::ElementType::Float64, {3, 2},
             {raw(1), nan, raw(2.5), nan, raw(5), nan})});
  auto result = columns.run(
      {{"samples", region({3, 2}, {ps::Region({{1, 1}, {0, 1}})})}}, false);
  require(!result.ok() &&
              result.status().reason == ps::FailureReason::InvalidDomain &&
              result.status().detail.scope == ps::FailureScope::Run,
          "invalid unrequested column fails Whole run");
  std::cout << "complete input support, global positions, remote columns, full "
               "dirty and Run errors passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      fixtures(profile);
      support_and_failures(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
