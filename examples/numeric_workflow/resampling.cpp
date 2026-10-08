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
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
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
  std::vector<ps::Value> backing;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs)
      : backing(inputs) {
    rf::declare_sources(&document, inputs);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bindings(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
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
    auto snapshot = context.freeze(plan.value().plan,
                                   bindings(take(context.resource_budget())));
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
          require(rf::read(result.results.at("samples"), at, &actual, 8).ok() &&
                      actual == raw(c ? 4 - expected : expected),
                  "resampling public samples");
        }
        std::uint64_t actual = 0, source = 0;
        std::memcpy(&source, fixture.backing[2].bytes().data() + i * 8, 8);
        require(
            rf::read(result.results.at("positions"), {i}, &actual, 8).ok() &&
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
      const auto requested = region(partial_shape, {ps::Region(dimensions)});
      require(
          take(partial.results.at("samples").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all(yshape)),
          "sparse samples publish full Whole coverage");
      auto full_support = take(partial.dependencies.source_support());
      for (unsigned port = 0; port < 3; ++port)
        require(full_support.at(("input" + std::to_string(port)).c_str()) ==
                    take(ps::Footprint::all(
                        fixture.backing[port].descriptor().shape)),
                "resample samples inherit complete Whole inputs");
      for (unsigned port = 0; port < 3; ++port) {
        const auto shape = fixture.backing[port].descriptor().shape;
        require(take(partial.dependencies.potential_dirty(
                         "input" + std::to_string(port),
                         take(ps::Footprint::all(shape))))
                        .at("samples") == requested,
                "Whole edits dirty the recorded sample request");
      }
      auto valid_queries = fixture.backing[2];
      fixture.backing[2] =
          doubles({3}, {pchip ? .5 : 2., pchip ? 1.5 : .5, 99});
      auto remote = fixture.run(
          {{"samples", region(partial_shape, {ps::Region(dimensions)})}},
          false);
      require(!remote.ok() &&
                  remote.status().reason == ps::FailureReason::InvalidDomain &&
                  remote.status().detail.scope == ps::FailureScope::Run,
              "remote query fails resample samples Whole run");
      fixture.backing[2] = valid_queries;
      auto positions = take(fixture.run(
          {{"positions", region({3}, {ps::Region({{1, 1}})})}}, false));
      auto support = take(positions.dependencies.source_support());
      require(!support.count("input0") && !support.count("input1") &&
                  support.at("input2") == region({3}, {ps::Region({{1, 1}})}),
              "positions-only support");
      require(
          take(positions.dependencies.potential_dirty(
                   "input0", take(ps::Footprint::all({3}))))
                  .at("positions")
                  .empty() &&
              take(positions.dependencies.potential_dirty(
                       "input2", region({3}, {ps::Region({{1, 1}})})))
                      .at("positions") == region({3}, {ps::Region({{1, 1}})}),
          "independent positions dirty support");
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
        require(result.results.at("positions")
                        .schema()
                        .tensors[0]
                        .descriptor.element_type == type,
                "positions preserves dtype despite sample Float64");
        for (unsigned i = 0; i < 4; ++i) {
          std::uint64_t actual = 0;
          require(rf::read(result.results.at("positions"), {i}, &actual,
                           narrow ? 4 : 8)
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
        fixture.backing[0] = doubles({2}, {0, 1});
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
    require(rf::read(result.results.at("samples"), {i}, &sample, 8).ok() &&
                numeric_accuracy(sample, UINT64_C(0x3fcc6b828682ab42), profile),
            "Nyquist residual after explicit Hann filter and downsample");
    require(rf::read(result.results.at("positions"), {i}, &position, 8).ok() &&
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
  Fixture typed({1, "core.identity", {ps::WorkflowInputReference{3}}, {}},
                {doubles({2}, {0, 1}), doubles({2}, {0, 1}), query});
  auto typed_schema = *typed.document.inputs[2].result_schema;
  typed_schema.tensors[0].facets = {facet};
  typed.document.inputs[2].result_schema =
      std::make_shared<ps::SchemaTemplate>(std::move(typed_schema));
  append(&typed, false, false, profile, ps::ElementType::Float64);
  auto independent =
      take(typed.run({{"positions", take(ps::Footprint::all({3}))}}, false));
  require(
      independent.results.at("positions").schema().tensors[0].facets.size() ==
              1 &&
          independent.results.at("positions")
                  .schema()
                  .tensors[0]
                  .facets[0]
                  .key == facet.key &&
          independent.results.at("positions")
                  .schema()
                  .tensors[0]
                  .facets[0]
                  .version == facet.version &&
          independent.results.at("positions")
                  .schema()
                  .tensors[0]
                  .facets[0]
                  .payload == facet.payload &&
          independent.results.at("positions")
                  .schema()
                  .tensors[0]
                  .descriptor.element_type == query.descriptor().element_type &&
          independent.results.at("positions")
                  .schema()
                  .tensors[0]
                  .descriptor.shape == query.descriptor().shape,
      "positions preserves typed descriptor and facets");
  auto sampled =
      take(typed.run({{"samples", take(ps::Footprint::all({3}))}}, false));
  require(sampled.results.at("samples").schema().tensors[0].facets.empty() &&
              sampled.results.at("samples")
                      .schema()
                      .tensors[0]
                      .descriptor.element_type == ps::ElementType::Float64,
          "samples has independent generic dtype");
  typed.backing[2] =
      array(ps::ElementType::Float32, {3}, {0, 0x3f000000, 0x7fc00000});
  const auto valid_position = take(
      typed.run({{"positions", region({3}, {ps::Region({{0, 1}})})}}, false));
  require(take(valid_position.results.at("positions").descriptor())
                  .tensor_coverage(0) == region({3}, {ps::Region({{0, 1}})}),
          "typed positions validation remains local to requested samples");
  for (const auto* output : {"positions", "samples"}) {
    const auto index = std::string(output) == "positions" ? 2U : 0U;
    const auto invalid =
        typed.run({{output, region({3}, {ps::Region({{index, 1}})})}}, false);
    require(!invalid.ok() &&
                invalid.status().code == ps::ErrorCode::InvalidArgument &&
                invalid.status().detail.input_id == 3,
            "positions validate requested typed sample; samples validate Whole "
            "query");
  }
  std::cout
      << "public Hann filter -> resample workflow: four residual samples "
         "0x3fcc6b828682ab42, positions [0,2,4,6]; typed positions preserved\n";
}

ps::Value reversed_unaligned(const ps::Value& value) {
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = value.bytes().size() / width;
  auto storage = take(ps::BufferAllocator{}.allocate(count * width + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(storage.data() + 1 + (count - 1 - i) * width,
                value.bytes().data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (std::size_t i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(storage).freeze()));
}
void layouts_and_resources(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (bool multi : {false, true}) {
      const std::vector<std::uint64_t> shape =
          multi ? std::vector<std::uint64_t>{3, 2}
                : std::vector<std::uint64_t>{3};
      const std::vector<ps::Value> dense{
          doubles({3}, {0, 1, 2}),
          doubles(shape, multi ? std::vector<double>{0, 4, 1, 3, 4, 0}
                               : std::vector<double>{0, 1, 4}),
          doubles({2}, {.5, 1.5})};
      for (const auto dtype :
           {ps::ElementType::Float32, ps::ElementType::Float64}) {
        for (unsigned reversed = 0; reversed < 8; ++reversed) {
          auto inputs = dense;
          for (unsigned port = 0; port < 3; ++port)
            if (reversed & (1U << port))
              inputs[port] = reversed_unaligned(inputs[port]);
          Fixture fixture({1, "core.identity", {}, {}}, inputs);
          append(&fixture, pchip, multi, profile, dtype);
          fixture.registry = ps::make_default_operation_registry(false);
          auto control = std::make_shared<point_math_checks::Control>();
          control->rounding = reversed % 2 ? FE_UPWARD : FE_DOWNWARD;
          fixture.document.nodes[0] = point_math_checks::checked_node(
              fixture.registry, fixture.document.nodes[0], control);
          require(fixture.registry->freeze().ok(), "freeze layout registry");
          fenv_t saved;
          require(fegetenv(&saved) == 0 &&
                      fesetround(*control->rounding) == 0 &&
                      feclearexcept(FE_ALL_EXCEPT) == 0 &&
                      feraiseexcept(FE_DIVBYZERO) == 0,
                  "set resampling caller fenv");
          auto result = fixture.run(
              {{"samples", take(ps::Footprint::all(
                               multi ? std::vector<std::uint64_t>{2, 2}
                                     : std::vector<std::uint64_t>{2}))},
               {"positions", take(ps::Footprint::all({2}))}},
              false);
          const bool restored = fegetround() == *control->rounding &&
                                fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
          require(fesetenv(&saved) == 0, "restore resampling caller fenv");
          require(result.ok() && restored && control->computation_polls > 0,
                  "resampling layout and real worker/caller fenv");
          for (unsigned row = 0; row < 2; ++row) {
            const double expected =
                pchip ? (row ? 2.1875 : .3125) : (row ? 2.5 : .5);
            for (unsigned column = 0; column < (multi ? 2U : 1U); ++column) {
              std::vector<std::uint64_t> at{row};
              if (multi)
                at.push_back(column);
              const double reference = column ? 4 - expected : expected;
              std::uint64_t expected_bits = raw(reference);
              if (dtype == ps::ElementType::Float32) {
                const float narrow = static_cast<float>(reference);
                expected_bits = 0;
                std::memcpy(&expected_bits, &narrow, 4);
              }
              std::uint64_t bits = 0;
              require(rf::read(result.value().results.at("samples"), at, &bits,
                               ps::Value::element_size(dtype))
                              .ok() &&
                          bits == expected_bits,
                      "resampling reversed/unaligned analytic samples");
            }
            std::uint64_t bits = 0;
            require(rf::read(result.value().results.at("positions"), {row},
                             &bits, 8)
                            .ok() &&
                        bits == raw(row ? 1.5 : .5),
                    "positions reads reversed source without changing order");
          }
        }
      }
      Fixture repeated({1, "core.identity", {}, {}}, dense);
      repeated.backing[1] = take(ps::Value::from_storage(
          dense[1].descriptor(), dense[1].region(),
          {0, std::vector<std::int64_t>(shape.size(), 0)}, dense[1].storage()));
      append(&repeated, pchip, multi, profile);
      const auto repeated_result = take(repeated.run(
          {{"samples",
            take(ps::Footprint::all(multi ? std::vector<std::uint64_t>{2, 2}
                                          : std::vector<std::uint64_t>{2}))}},
          false));
      require(rf::bytes(repeated_result.results.at("samples")) ==
                  std::vector<std::uint8_t>(multi ? 32 : 16, 0),
              "resampling accepts zero-stride ordinates");
      auto large = dense;
      large[2] = doubles({256}, std::vector<double>(256, .5));
      point_math_checks::resources(repeated.document.nodes[0], large,
                                   256 * (multi ? 2 : 1) * 8);
    }
  std::cout << "four templates: all reversed/unaligned source layouts, zero "
               "strides, worker/caller fenv and work/cancel/resources passed\n";
}
struct FailedSamplesSource final {
  unsigned* calls;
  explicit FailedSamplesSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(ps::Status{
        ps::ErrorCode::OperationFailed, "required resampling values producer"});
  }
};
void cache_and_upstream(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (bool multi : {false, true}) {
      const std::vector<std::uint64_t> yshape =
          multi ? std::vector<std::uint64_t>{3, 2}
                : std::vector<std::uint64_t>{3};
      Fixture fixture(
          {1, "core.identity", {}, {}},
          {doubles({3}, {0, 1, 2}),
           doubles(yshape, multi ? std::vector<double>{0, 4, 1, 3, 4, 0}
                                 : std::vector<double>{0, 1, 4}),
           doubles({2}, {.5, 1.5})});
      append(&fixture, pchip, multi, profile);
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph)).plan;
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 1048576;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      const auto root = take(context.resource_budget());
      auto bindings = fixture.bindings(root);
      auto demand = take(context.open_demand(plan, bindings));
      const auto prepared = plan.steps()[0].prepared;
      require(prepared != nullptr,
              "resampling static interpolation preparation");
      const auto output_shape = multi ? std::vector<std::uint64_t>{2, 2}
                                      : std::vector<std::uint64_t>{2};
      const ps::DemandQuery query{
          {"samples", take(ps::Footprint::all(output_shape))},
          {"positions", take(ps::Footprint::all({2}))}};
      ps::ExecutionOptions options;
      options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
      options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
      options.maximum_dependency_cache_work = 128 * 1024 * 1024;
      const auto cold = take(demand.request(query, {}, options));
      const auto repeated = take(demand.request(query, {}, options));
      for (const auto* name : {"samples", "positions"})
        require(cold.results.at(name).object_id() ==
                    repeated.results.at(name).object_id(),
                "repeated resampling demand retains completed Results");
      const auto fresh = fixture.bindings(root);
      const auto warm = take(context.execute_fragments(
          take(context.freeze(plan, fresh)), query, {}, options));
      require(
          warm.diagnostics.cache_hits == 2 &&
              rf::bytes(warm.results.at("samples")) ==
                  rf::bytes(cold.results.at("samples")),
          "fresh resampling content reuses interpolation and identity caches");
      const auto associations = warm.results.at("samples").association();
      for (unsigned port = 0; port < 3; ++port)
        require(std::find(associations.begin(), associations.end(),
                          fresh.inputs[port].result.object_id()) !=
                        associations.end() &&
                    std::find(associations.begin(), associations.end(),
                              bindings.inputs[port].result.object_id()) ==
                        associations.end(),
                "cached samples associate all current source Results");
      const auto position_associations =
          warm.results.at("positions").association();
      require(
          std::find(position_associations.begin(), position_associations.end(),
                    fresh.inputs[2].result.object_id()) !=
                  position_associations.end() &&
              std::find(position_associations.begin(),
                        position_associations.end(),
                        bindings.inputs[2].result.object_id()) ==
                  position_associations.end(),
          "cached positions associate the current query source");
      bindings.inputs[2].result = point_math_checks::source(
          root, doubles({2}, {1., 2.}),
          fixture.document.inputs[2].result_schema.get());
      require(demand.replace_bindings(bindings).ok(),
              "replace resampling query");
      const auto moved = take(demand.request(query, {}, options));
      std::uint64_t bits = 0;
      require(
          rf::read(moved.results.at("samples"),
                   multi ? std::vector<std::uint64_t>{0, 0}
                         : std::vector<std::uint64_t>{0},
                   &bits, 8)
                  .ok() &&
              bits == raw(1.) && plan.steps()[0].prepared == prepared,
          "replaced query reuses static preparation and recomputes samples");
      bindings.inputs[0].result = point_math_checks::source(
          root, doubles({3}, {0, 0, 2}),
          fixture.document.inputs[0].result_schema.get());
      require(demand.replace_bindings(bindings).ok(),
              "replace resampling topology");
      auto failed = demand.request(query, {}, options);
      require(
          !failed.ok() &&
              failed.status().reason == ps::FailureReason::InvalidDomain &&
              plan.steps()[0].prepared == prepared,
          "invalid topology invalidates cached sample and retains preparation");

      auto registry = ps::make_default_operation_registry(false);
      unsigned calls = 0;
      ps::OperationDefinition producer;
      producer.key = "manual.resampling_values";
      producer.traits.input_count = 0;
      producer.traits.input_schema.clear();
      auto& output = producer.traits.outputs[0];
      output.region_rule = ps::OperationRegionRule::Whole;
      output.output_schema.kind = ps::OperationPortKind::Result;
      auto schema = rf::source_schema(fixture.backing[1]);
      output.output_schema.result_schema_id = schema.id;
      output.output_schema.result_schema_version = schema.version;
      output.result_schema = std::move(schema);
      output.continuation_bytes = sizeof(FailedSamplesSource);
      output.maximum_dependency_stages = 8;
      producer.start_result = [&](const auto&, const auto& allocator) {
        return ps::ResultContinuation::make<FailedSamplesSource>(allocator,
                                                                 &calls);
      };
      require(registry->register_operation(std::move(producer)).ok() &&
                  registry->freeze().ok(),
              "register resampling failure source");
      Fixture upstream({1, "core.identity", {}, {}}, fixture.backing);
      append(&upstream, pchip, multi, profile);
      upstream.registry = registry;
      upstream.document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{3, "value"};
      upstream.document.nodes.push_back(
          {3, "manual.resampling_values", {}, {}});
      upstream.document.inputs.erase(upstream.document.inputs.begin() + 1);
      upstream.backing.erase(upstream.backing.begin() + 1);
      const auto empty = take(
          upstream.run({{"samples", take(ps::Footprint::none(output_shape))},
                        {"positions", take(ps::Footprint::none({2}))}},
                       false));
      require(calls == 0 &&
                  take(empty.results.at("samples").descriptor())
                      .tensor_coverage(0)
                      .empty() &&
                  take(empty.results.at("positions").descriptor())
                      .tensor_coverage(0)
                      .empty(),
              "Empty resampling never polls failing source");
      take(upstream.run({{"positions", take(ps::Footprint::all({2}))}}, false));
      require(calls == 0,
              "positions output isolates failed old values producer");
      failed = upstream.run(query, false);
      require(!failed.ok() &&
                  failed.status().message ==
                      "required resampling values producer" &&
                  calls == 1,
              "samples propagate actual upstream failure");
    }
  std::cout << "four templates: fresh-content cache, current associations, "
               "preparation/query/topology reuse and Empty/upstream isolation "
               "passed\n";
}
void maximum_positions(ps::CpuNumericProfile profile) {
  const auto count = UINT64_C(1) << 40;
  const auto scalar = doubles({1}, {.5});
  const auto query = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {count}}, ps::Region::whole({count}), {0, {0}},
      scalar.storage()));
  for (bool pchip : {false, true}) {
    Fixture fixture({1, "core.identity", {}, {}},
                    {doubles({2}, {0, 1}), doubles({2}, {0, 1}), query});
    const auto exports = append(&fixture, pchip, false, profile);
    const auto tiny = region({count}, {ps::Region({{count - 1, 1}})});
    const auto forwarded = take(fixture.run({{"positions", tiny}}, false));
    const auto result = forwarded.results.at("positions");
    const auto window = take(result.acquire_tensor(
        take(result.descriptor()), 0, ps::Region({{count - 1, 1}})));
    std::uint64_t bits = 0;
    std::memcpy(&bits, take(window.row_run({count - 1})).data, 8);
    require(
        bits == raw(.5) &&
            take(window.row_run({count - 1})).data == scalar.bytes().data() &&
            result.schema().tensors[0].sample_shape() ==
                std::vector<std::uint64_t>{count},
        "maximum legal position shape remains a zero-copy view");
    const auto failed = fixture.run({{"samples", tiny}}, false);
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::ResourceExhausted &&
                failed.status().reason == ps::FailureReason::CapacityLimit &&
                failed.status().detail.node_id == exports.samples.source_node,
            "maximum Whole samples reject complete output allocation budget");
  }
  std::vector<double> knots(65536);
  std::vector<double> values(65536);
  for (unsigned i = 0; i < knots.size(); ++i) {
    knots[i] = i;
    values[i] = i * .5;
  }
  for (bool pchip : {false, true}) {
    Fixture fixture({1, "core.identity", {}, {}},
                    {doubles({65536}, knots), doubles({65536}, values),
                     doubles({1}, {65535})});
    append(&fixture, pchip, false, profile);
    const auto sampled =
        take(fixture.run({{"samples", take(ps::Footprint::all({1}))}}, false));
    std::uint64_t bits = 0;
    require(rf::read(sampled.results.at("samples"), {0}, &bits, 8).ok() &&
                bits == raw(32767.5),
            "maximum K=65536 executes exact endpoint sample");
  }
  std::cout
      << "maximum N=2^40 positions view succeeds without materialization; "
         "Whole samples report CapacityLimit; max K=65536 executes\n";
}
void retained_outputs(ps::CpuNumericProfile profile) {
  for (bool pchip : {false, true})
    for (const auto dtype :
         {ps::ElementType::Float32, ps::ElementType::Float64}) {
      ps::ResourceBudget root;
      ps::ResultRef samples, positions;
      ps::ResultTensorReadWindow sample_window, position_window;
      std::vector<std::weak_ptr<const ps::CpuStorage>> owners;
      const auto width = ps::Value::element_size(dtype);
      const auto half =
          dtype == ps::ElementType::Float32 ? UINT64_C(0x3f000000) : raw(.5);
      {
        Fixture fixture(
            {1, "core.identity", {}, {}},
            {doubles({3}, {0, 1, 2}), doubles({3}, {0, 1, 2}),
             array(dtype, {3},
                   {0, half,
                    dtype == ps::ElementType::Float32 ? UINT64_C(0x3f800000)
                                                      : raw(1.)})});
        for (const auto& backing : fixture.backing)
          owners.push_back(backing.storage());
        append(&fixture, pchip, false, profile, dtype);
        ps::GraphContext graph(fixture.document);
        const auto plan =
            take(ps::Compiler(fixture.registry).compile(graph)).plan;
        ps::ExecutionContextConfig config;
        config.cpu_workers = 1;
        config.result_cache_bytes = 0;
        config.managed_resources = ps::ResourceLimits{};
        ps::ExecutionContext context(fixture.registry, config);
        root = take(context.resource_budget());
        auto bindings = fixture.bindings(root);
        const auto output = take(context.execute_fragments(
            take(context.freeze(plan, bindings)),
            {{"samples", take(ps::Footprint::all({3}))},
             {"positions", take(ps::Footprint::all({3}))}}));
        samples = output.results.at("samples");
        positions = output.results.at("positions");
        sample_window = take(samples.acquire_tensor(take(samples.descriptor()),
                                                    0, ps::Region::whole({3})));
        position_window = take(positions.acquire_tensor(
            take(positions.descriptor()), 0, ps::Region::whole({3})));
        require(take(position_window.row_run({0})).data ==
                    fixture.backing[2].bytes().data(),
                "positions maps source storage without a payload copy");
      }
      require(
          owners[0].expired() && owners[1].expired() && !owners[2].expired(),
          "samples retire old curve owners; positions retain query owner");
      require(root.statistics().live[ps::ResourceKind::Payload] == 3 * width,
              "escaped samples/window share packed Payload owner");
      std::uint64_t bits = 0;
      require(rf::read(samples, {1}, &bits, width).ok() && bits == half &&
                  rf::read(positions, {1}, &bits, width).ok() && bits == half,
              "both resampling Results survive context retirement");
      samples = {};
      positions = {};
      bits = 0;
      std::memcpy(&bits, take(sample_window.row_run({1})).data, width);
      require(bits == half && !owners[2].expired(),
              "windows retain independent owners after Result release");
      bits = 0;
      std::memcpy(&bits, take(position_window.row_run({1})).data, width);
      require(bits == half,
              "authorized mapped position window survives Result release");
      sample_window = {};
      require(root.statistics().live[ps::ResourceKind::Payload] == 0 &&
                  !owners[2].expired(),
              "sample release does not retire mapped positions owner");
      position_window = {};
      require(owners[2].expired(),
              "final mapped window retires query source owner");
      point_math_checks::released(root);
    }
  std::cout
      << "Float32/64 packed samples, zero-copy positions, escaped windows, "
         "source retirement and all-Root release passed\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "resampling profile must be strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    examples(profile);
    independent_positions(profile);
    authoring(profile);
    filtered_workflow(profile);
    layouts_and_resources(profile);
    cache_and_upstream(profile);
    maximum_positions(profile);
    retained_outputs(profile);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
