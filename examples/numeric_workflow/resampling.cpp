#include "photospider/numeric/resampling.hpp"

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
ps::numeric::ResampledSignal append(
    Fixture* fixture, bool pchip, bool multi, ps::CpuNumericProfile profile,
    ps::ElementType dtype = ps::ElementType::Float64) {
  fixture->document.nodes.clear();
  fixture->document.outputs.clear();
  auto helper = pchip ? (multi ? ps::numeric::resample_pchip_multi
                               : ps::numeric::resample_pchip)
                      : (multi ? ps::numeric::resample_linear_multi
                               : ps::numeric::resample_linear);
  auto result =
      take(helper(fixture->document, ps::WorkflowInputReference{1},
                  ps::WorkflowInputReference{2}, ps::WorkflowInputReference{3},
                  dtype, ps::numeric::CurveDomain::Reject, profile));
  const auto exports = result.outputs();
  fixture->document.outputs.assign(exports.begin(), exports.end());
  return result;
}
void examples(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (bool multi : {false, true}) {
      const std::vector<std::uint64_t> yshape =
          multi ? std::vector<std::uint64_t>{3, 2}
                : std::vector<std::uint64_t>{3};
      Fixture fixture(
          {1, "core.identity", {ps::WorkflowInputReference{3}}, {}},
          {doubles({3}, pchip ? std::vector<double>{0, 1, 2}
                              : std::vector<double>{0, 1, 3}),
           doubles(yshape, multi ? std::vector<double>{0, 4, pchip ? 1. : 2.,
                                                       pchip ? 3. : 2., 4, 0}
                                 : std::vector<double>{0, pchip ? 1. : 2., 4}),
           doubles({3}, pchip ? std::vector<double>{.5, 1.5, .5}
                              : std::vector<double>{2, .5, 2})});
      append(&fixture, pchip, multi, profile);
      auto result =
          take(fixture.run({{"samples", take(ps::Footprint::all(yshape))},
                            {"positions", take(ps::Footprint::all({3}))}},
                           false));
      for (unsigned i = 0; i < 3; ++i) {
        const double expected =
            pchip ? (i == 1 ? 2.1875 : .3125) : (i == 1 ? 1. : 3.);
        for (unsigned c = 0; c < (multi ? 2U : 1U); ++c) {
          std::vector<std::uint64_t> at{i};
          if (multi)
            at.push_back(c);
          std::uint64_t actual = 0;
          require(result.values.at("samples").read(at, &actual, 8).ok() &&
                      actual == raw(c ? 4 - expected : expected),
                  "resampling public samples");
        }
        std::uint64_t actual = 0, source = 0;
        std::memcpy(&source,
                    fixture.bindings.inputs[2].value.bytes().data() + i * 8, 8);
        require(result.values.at("positions").read({i}, &actual, 8).ok() &&
                    actual == source,
                "independent position bits");
      }
      auto partial_shape = yshape;
      std::vector<ps::RegionDimension> dimensions{{0, 1}};
      if (multi)
        dimensions.push_back({0, 1});
      auto partial = take(fixture.run(
          {{"samples", region(partial_shape, {ps::Region(dimensions)})}},
          false));
      auto full_support = take(partial.dependencies.source_support());
      for (unsigned port = 0; port < 3; ++port)
        require(
            full_support.at("input" + std::to_string(port)) ==
                take(ps::Footprint::all(
                    fixture.bindings.inputs[port].value.descriptor().shape)),
            "resample samples inherit complete Whole inputs");
      auto valid_queries = fixture.bindings.inputs[2].value;
      fixture.bindings.inputs[2].value =
          doubles({3}, {pchip ? .5 : 2., pchip ? 1.5 : .5, 99});
      auto remote = fixture.run(
          {{"samples", region(partial_shape, {ps::Region(dimensions)})}},
          false);
      require(!remote.ok() &&
                  remote.status().reason == ps::FailureReason::InvalidDomain &&
                  remote.status().detail.scope == ps::FailureScope::Run,
              "remote query fails resample samples Whole run");
      fixture.bindings.inputs[2].value = valid_queries;
      auto positions = take(fixture.run(
          {{"positions", region({3}, {ps::Region({{1, 1}})})}}, false));
      auto support = take(positions.dependencies.source_support());
      require(!support.count("input0") && !support.count("input1") &&
                  support.at("input2") == region({3}, {ps::Region({{1, 1}})}),
              "positions-only support");
    }
  std::cout
      << "four templates: linear samples [3,1,3], PCHIP [.3125,2.1875,.3125], "
         "independent positions and multi columns passed\n";
}
void independent_positions(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (bool multi : {false, true})
      for (bool narrow : {false, true}) {
        const auto type =
            narrow ? ps::ElementType::Float32 : ps::ElementType::Float64;
        const std::vector<std::uint64_t> bits =
            narrow ? std::vector<std::uint64_t>{0x7f800042, 0xff800000,
                                                0x80000000, 0x7fc00056}
                   : std::vector<std::uint64_t>{UINT64_C(0x7ff0000000000042),
                                                UINT64_C(0xfff0000000000000),
                                                UINT64_C(0x8000000000000000),
                                                UINT64_C(0x7ff8000000000056)};
        Fixture fixture(
            {1, "core.identity", {ps::WorkflowInputReference{3}}, {}},
            {doubles({2}, {0, 0}),
             doubles(multi ? std::vector<std::uint64_t>{2, 1}
                           : std::vector<std::uint64_t>{2},
                     {0, 0}),
             array(type, {4}, bits)});
        append(&fixture, pchip, multi, profile);
        auto result = take(
            fixture.run({{"positions", take(ps::Footprint::all({4}))}}, false));
        require(result.values.at("positions").descriptor().element_type == type,
                "positions preserves dtype despite sample Float64");
        for (unsigned i = 0; i < 4; ++i) {
          std::uint64_t actual = 0;
          require(result.values.at("positions")
                          .read({i}, &actual, narrow ? 4 : 8)
                          .ok() &&
                      actual == bits[i],
                  "positions preserves sNaN/Inf/zero payloads");
        }
        auto failed = fixture.run(
            {{"samples",
              take(ps::Footprint::all(multi ? std::vector<std::uint64_t>{4, 1}
                                            : std::vector<std::uint64_t>{4}))}},
            false);
        require(!failed.ok() &&
                    failed.status().reason == ps::FailureReason::InvalidDomain,
                "samples still validates old positions");
        fixture.bindings.inputs[0].value = doubles({2}, {0, 1});
        failed = fixture.run(
            {{"samples",
              take(ps::Footprint::all(multi ? std::vector<std::uint64_t>{4, 1}
                                            : std::vector<std::uint64_t>{4}))}},
            false);
        require(!failed.ok() &&
                    failed.status().reason == ps::FailureReason::InvalidDomain,
                "samples validates nonfinite query");
      }
  std::cout << "positions-only Float32/64 sNaN/Inf/-0 payloads, invalid old "
               "data isolation and sample rejection passed\n";
}
void authoring(ps::CpuNumericProfile profile) {
  ps::WorkflowDocument document;
  document.nodes = {
      {2, "core.identity", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"existing", 9, "value"}};
  auto result = take(ps::numeric::resample_linear(
      document, ps::WorkflowNodeOutput{3, "value"},
      ps::WorkflowInputReference{2}, ps::WorkflowInputReference{3},
      ps::ElementType::Float64, ps::numeric::CurveDomain::Clamp, profile));
  require(document.nodes.size() == 3 && result.samples.source_node == 4 &&
              result.positions.source_node == 5 &&
              document.outputs.size() == 1 &&
              document.outputs[0].name == "existing",
          "IDs reserve declarations/references/exports");
  auto failed = ps::numeric::resample_linear(
      document, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
      ps::WorkflowInputReference{3}, ps::ElementType::UInt8);
  require(!failed.ok() && document.nodes.size() == 3,
          "failed expansion transactional");
  auto registry = ps::make_default_operation_registry();
  require(!registry->find_traits("curve.resample_linear").ok(),
          "resampler is authoring template");
  std::cout << "transactional ordinary-node expansion and collision-free IDs "
               "passed\n";
}
void filtered_workflow(ps::CpuNumericProfile profile) {
  Fixture fixture(
      {1, "core.identity", {ps::WorkflowInputReference{3}}, {}},
      {doubles({8}, {0, 1, 2, 3, 4, 5, 6, 7}),
       doubles({8}, {1, -1, 1, -1, 1, -1, 1, -1}), doubles({4}, {0, 2, 4, 6})});
  fixture.document.nodes.clear();
  fixture.document.outputs.clear();
  fixture.document.nodes.push_back(
      take(ps::numeric::lowpass_uniform_hann_sinc_node(
          10, ps::WorkflowInputReference{2}, 0, 2, .25,
          ps::numeric::LowpassBoundary::Wrap, profile)));
  auto resampled = take(ps::numeric::resample_linear(
      fixture.document, ps::WorkflowInputReference{1},
      ps::WorkflowNodeOutput{10, "values"}, ps::WorkflowInputReference{3},
      ps::ElementType::Float64, ps::numeric::CurveDomain::Reject, profile));
  const auto exports = resampled.outputs();
  fixture.document.outputs.assign(exports.begin(), exports.end());
  auto result = take(fixture.run({{"samples", take(ps::Footprint::all({4}))},
                                  {"positions", take(ps::Footprint::all({4}))}},
                                 false));
  for (unsigned i = 0; i < 4; ++i) {
    std::uint64_t sample = 0, position = 0;
    require(result.values.at("samples").read({i}, &sample, 8).ok() &&
                numeric_accuracy(sample, UINT64_C(0x3fcc6b828682ab42), profile),
            "Nyquist residual after explicit Hann filter and downsample");
    require(result.values.at("positions").read({i}, &position, 8).ok() &&
                position == raw(2 * i),
            "downsample position export");
  }
  auto query =
      array(ps::ElementType::Float32, {3}, {0, 0x3f000000, 0x3f800000});
  ps::SemanticDescriptor signal;
  signal.kind = ps::SemanticKind::SampledSignal;
  signal.channels = {{"position", "position", "dimensionless"}};
  signal.sample_step = 1;
  signal.sample_axis_unit = "sample";
  const auto facet = take(ps::encode_semantic(signal));
  query =
      take(ps::Value::from_storage(query.descriptor(), query.region(),
                                   query.layout(), query.storage(), {facet}));
  Fixture typed({1, "core.identity", {ps::WorkflowInputReference{3}}, {}},
                {doubles({2}, {0, 1}), doubles({2}, {0, 1}), query});
  append(&typed, false, false, profile, ps::ElementType::Float64);
  auto independent =
      take(typed.run({{"positions", take(ps::Footprint::all({3}))}}, false));
  require(independent.values.at("positions").facets().size() == 1 &&
              independent.values.at("positions").facets()[0].key == facet.key &&
              independent.values.at("positions").facets()[0].version ==
                  facet.version &&
              independent.values.at("positions").facets()[0].payload ==
                  facet.payload &&
              independent.values.at("positions").descriptor().element_type ==
                  query.descriptor().element_type &&
              independent.values.at("positions").descriptor().shape ==
                  query.descriptor().shape,
          "positions preserves typed descriptor and facets");
  auto sampled =
      take(typed.run({{"samples", take(ps::Footprint::all({3}))}}, false));
  require(sampled.values.at("samples").facets().empty() &&
              sampled.values.at("samples").descriptor().element_type ==
                  ps::ElementType::Float64,
          "samples has independent generic dtype");
  std::cout
      << "public Hann filter -> resample workflow: four residual samples "
         "0x3fcc6b828682ab42, positions [0,2,4,6]; typed positions preserved\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    examples(profile);
    independent_positions(profile);
    authoring(profile);
    filtered_workflow(profile);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
