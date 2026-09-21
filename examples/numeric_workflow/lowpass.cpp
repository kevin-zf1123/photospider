#include "photospider/numeric/lowpass.hpp"

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

#include "accuracy.hpp"  // NOLINT(build/include_subdir)
#include "photospider/numeric/arrays.hpp"
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
ps::WorkflowNode authored(unsigned kernel, unsigned radius, double parameter,
                          double beta, ps::numeric::LowpassBoundary boundary,
                          ps::CpuNumericProfile profile, unsigned axis = 0) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  if (kernel == 3)
    return take(lowpass_uniform_kaiser_sinc_node(
        1, ps::WorkflowInputReference{1}, axis, radius, parameter, beta,
        boundary, profile));
  auto helper = kernel == 0   ? lowpass_uniform_hann_sinc_node
                : kernel == 1 ? lowpass_uniform_hamming_sinc_node
                : kernel == 2 ? lowpass_uniform_blackman_sinc_node
                              : lowpass_uniform_gaussian_node;
  return take(helper(1, ps::WorkflowInputReference{1}, axis, radius, parameter,
                     boundary, profile));
}
void oracle(ps::CpuNumericProfile profile) {
  unsigned kernel = 0, type = 0, radius = 0, boundary = 0, n = 0;
  std::uint64_t parameter = 0, beta = 0;
  while (std::cin >> kernel >> type >> radius >> boundary >> n >> std::hex >>
         parameter >> beta >> std::dec) {
    std::vector<std::uint64_t> samples(n);
    for (auto& value : samples)
      std::cin >> std::hex >> value >> std::dec;
    double p = 0, b = 0;
    std::memcpy(&p, &parameter, 8);
    std::memcpy(&b, &beta, 8);
    Fixture fixture(
        authored(kernel, radius, p, b,
                 static_cast<ps::numeric::LowpassBoundary>(boundary), profile),
        {array(static_cast<ps::ElementType>(type), {n}, samples)});
    auto result =
        fixture.run({{"values", take(ps::Footprint::all({n}))}}, false);
    if (!result.ok()) {
      std::cout << "error\n";
      std::cerr << result.status().message << '\n';
      continue;
    }
    for (unsigned i = 0; i < n; ++i) {
      std::uint64_t actual = 0;
      require(result.value()
                  .values.at("values")
                  .read({i}, &actual, type == 4 ? 4 : 8)
                  .ok(),
              "lowpass oracle read");
      if (i)
        std::cout << ' ';
      std::cout << std::hex << actual;
    }
    std::cout << std::dec << '\n';
  }
}
void fixtures(ps::CpuNumericProfile profile) {
  const std::array<std::uint64_t, 5> expected{
      UINT64_C(0x3fe38d7050d05568), UINT64_C(0x3fe2f660651f7f7c),
      UINT64_C(0x3fe655124d269c1c), UINT64_C(0x3fdc2755e149a310),
      UINT64_C(0x3fd9c486742831f7)};
  for (unsigned kernel = 0; kernel < 5; ++kernel) {
    Fixture impulse(authored(kernel, 2, kernel == 4 ? 1. : .25, 0,
                             ps::numeric::LowpassBoundary::Reflect, profile),
                    {doubles({5}, {0, 0, 1, 0, 0})});
    auto result = take(
        impulse.run({{"values", region({5}, {ps::Region({{2, 1}})})}}, false));
    std::uint64_t actual = 0;
    require(result.values.at("values").read({2}, &actual, 8).ok() &&
                numeric_accuracy(actual, expected[kernel], profile),
            "certified full-kernel impulse fixture");
    for (auto boundary : {ps::numeric::LowpassBoundary::Reflect,
                          ps::numeric::LowpassBoundary::Replicate,
                          ps::numeric::LowpassBoundary::Wrap}) {
      Fixture constant(
          authored(kernel, 4, kernel == 4 ? 1. : .2, 4, boundary, profile),
          {doubles({3}, {-0., -0., -0.})});
      auto preserved = take(
          constant.run({{"values", take(ps::Footprint::all({3}))}}, false));
      for (unsigned i = 0; i < 3; ++i) {
        actual = 0;
        require(preserved.values.at("values").read({i}, &actual, 8).ok() &&
                    actual == raw(-0.),
                "constant -0 including short signal boundaries");
      }
    }
  }
  std::cout << "five independent R2 impulse constants and short-signal -0 DC "
               "fixtures passed\n";
}
void support_and_ieee(ps::CpuNumericProfile profile) {
  const auto snan = UINT64_C(0x7ff0000000000042),
             negative_nan = UINT64_C(0xfff0000000000056),
             inf = UINT64_C(0x7ff0000000000000);
  Fixture identity(
      authored(0, 1, .2, 0, ps::numeric::LowpassBoundary::Reflect, profile),
      {array(ps::ElementType::Float64, {3}, {snan, raw(7), negative_nan})});
  auto result = take(
      identity.run({{"values", region({3}, {ps::Region({{1, 1}})})}}, false));
  require(take(result.dependencies.source_support()).at("input0") ==
              region({3}, {ps::Region({{1, 1}})}),
          "Hann R1 exactzero nonreads");
  std::uint64_t actual = 0;
  require(
      result.values.at("values").read({1}, &actual, 8).ok() && actual == raw(7),
      "Hann R1 identity");
  Fixture zero_ends(
      authored(1, 2, .25, 0, ps::numeric::LowpassBoundary::Zero, profile),
      {array(ps::ElementType::Float64, {5},
             {snan, 0, raw(1), 0, negative_nan})});
  auto skipped = take(
      zero_ends.run({{"values", region({5}, {ps::Region({{2, 1}})})}}, false));
  require(take(skipped.dependencies.source_support()).at("input0") ==
              region({5}, {ps::Region({{1, 3}})}),
          "integer sinc zeros omit Hamming endpoints");
  Fixture nan_order(
      authored(4, 2, .01, 0, ps::numeric::LowpassBoundary::Reflect, profile),
      {array(ps::ElementType::Float64, {3}, {snan, inf, negative_nan})});
  auto propagated = take(
      nan_order.run({{"values", region({3}, {ps::Region({{0, 1}})})}}, false));
  require(propagated.values.at("values").read({0}, &actual, 8).ok() &&
              actual == (negative_nan | UINT64_C(0x8000000000000)),
          "reflected first logical negative tap NaN, even tiny Gaussian");
  Fixture mixed_inf(
      authored(4, 1, 1, 0, ps::numeric::LowpassBoundary::Zero, profile),
      {array(ps::ElementType::Float64, {3},
             {inf, 0, inf | (UINT64_C(1) << 63)})});
  auto mixed = take(
      mixed_inf.run({{"values", region({3}, {ps::Region({{1, 1}})})}}, false));
  require(mixed.values.at("values").read({1}, &actual, 8).ok() &&
              actual == (inf | UINT64_C(0x8000000000000)),
          "mixed signed infinities canonical NaN");
  Fixture wrapped(
      authored(0, 2, .2, 0, ps::numeric::LowpassBoundary::Wrap, profile),
      {doubles({8}, {0, 1, 2, 3, 4, 5, 6, 7})});
  auto edge = take(
      wrapped.run({{"values", region({8}, {ps::Region({{0, 1}})})}}, false));
  require(take(edge.dependencies.source_support()).at("input0") ==
              region({8}, {ps::Region({{0, 2}}), ps::Region({{7, 1}})}),
          "exact wrapped edge support");
  require(take(edge.dependencies.potential_dirty(
                   "input0", region({8}, {ps::Region({{3, 1}})})))
              .at("values")
              .empty(),
          "distant sample does not dirty wrapped edge");
  std::cout << "zero tap nonreads, Gaussian underflow support, logical NaN/Inf "
               "order and wrap dirty support passed\n";
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
      support_and_ieee(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
