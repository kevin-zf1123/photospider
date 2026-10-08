#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../../examples/numeric_workflow/icc_fixture.hpp"
#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
WorkflowNode ramp_node(ColorModel model, ColorHueUnit unit,
                       CpuNumericProfile profile = CpuNumericProfile::Strict,
                       bool reject = false) {
  const WorkflowInput q = WorkflowInputReference{11},
                      k = WorkflowInputReference{12},
                      c = WorkflowInputReference{13};
  numeric::ColorRampOptions options;
  options.dtype = ElementType::Float64;
  options.profile = profile;
  options.out_of_domain =
      reject ? numeric::CurveDomain::Reject : numeric::CurveDomain::Clamp;
  auto color = numeric::color_ramp_detail::description(model, unit);
  if (model == ColorModel::Rgb) {
    color = numeric::color_ramp_rgb_description();
    color.transfer = ColorTransfer{ColorTransferKind::Linear, {}};
    numeric::RgbRampOptions rgb;
    static_cast<numeric::ColorRampOptions&>(rgb) = options;
    return take(numeric::color_ramp_rgb_node(7, q, k, c, ElementType::Float64,
                                             color, rgb));
  }
  if (model == ColorModel::Ycbcr) {
    color = numeric::color_ramp_rgb_description();
    color.model = model;
    color.ncl_coefficients =
        take(color_ncl_coefficients(ColorNclPreset::Bt709));
  }
  if (model == ColorModel::Cielch || model == ColorModel::Oklch ||
      model == ColorModel::Hsl) {
    numeric::HueRampOptions hue;
    static_cast<numeric::ColorRampOptions&>(hue) = options;
    hue.output_hue_unit = ColorHueUnit::PiMultiple;
    if (unit == ColorHueUnit::RationalPi) {
      const auto helper = model == ColorModel::Cielch
                              ? numeric::color_ramp_cielch_rational_pi_node
                          : model == ColorModel::Oklch
                              ? numeric::color_ramp_oklch_rational_pi_node
                              : numeric::color_ramp_hsl_rational_pi_node;
      return take(helper(7, q, k, c, WorkflowInputReference{14},
                         WorkflowInputReference{15}, ElementType::Float64,
                         color, hue));
    }
    const auto helper =
        model == ColorModel::Cielch  ? (unit == ColorHueUnit::PiMultiple
                                            ? numeric::color_ramp_cielch_pi_node
                                            : numeric::color_ramp_cielch_node)
        : model == ColorModel::Oklch ? (unit == ColorHueUnit::PiMultiple
                                            ? numeric::color_ramp_oklch_pi_node
                                            : numeric::color_ramp_oklch_node)
                                     : (unit == ColorHueUnit::PiMultiple
                                            ? numeric::color_ramp_hsl_pi_node
                                            : numeric::color_ramp_hsl_node);
    return take(helper(7, q, k, c, ElementType::Float64, color, hue));
  }
  const auto helper =
      model == ColorModel::Xyz      ? numeric::color_ramp_xyz_node
      : model == ColorModel::Cielab ? numeric::color_ramp_cielab_node
      : model == ColorModel::Oklab  ? numeric::color_ramp_oklab_node
                                    : numeric::color_ramp_ycbcr_node;
  return take(helper(7, q, k, c, ElementType::Float64, color, options));
}
void color_ramp_workflows() {
  Driver d;
  ResultRef retained;
  for (auto profile :
       {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon}) {
    for (auto model : {ColorModel::Rgb, ColorModel::Xyz, ColorModel::Cielab,
                       ColorModel::Oklab, ColorModel::Ycbcr, ColorModel::Cielch,
                       ColorModel::Oklch, ColorModel::Hsl}) {
      const bool polar = model == ColorModel::Cielch ||
                         model == ColorModel::Oklch || model == ColorModel::Hsl;
      for (auto unit : {ColorHueUnit::Radian, ColorHueUnit::PiMultiple,
                        ColorHueUnit::RationalPi}) {
        if (!polar && unit != ColorHueUnit::Radian)
          continue;
        auto node = ramp_node(model, unit, profile);
        const bool split = polar && unit == ColorHueUnit::RationalPi;
        const unsigned channels = split ? 2 : 3;
        auto query =
            d.source({ElementType::Float32, {2}},
                     {float_bits(2), float_bits(.5), 0, float_bits(-1)},
                     {13, {-8, -4}}, {}, {2});
        auto stops = d.source({ElementType::Float64, {2}}, {double_bits(1), 0},
                              {9, {-8}});
        std::vector<uint64_t> values;
        const std::array<double, 3> low =
            model == ColorModel::Hsl ? std::array<double, 3>{0, .25, .5}
                                     : std::array<double, 3>{.25, .25, 0};
        const std::array<double, 3> high =
            model == ColorModel::Hsl ? std::array<double, 3>{4, .75, 1}
                                     : std::array<double, 3>{.75, .75, 4};
        for (const auto& row : {low, high})
          for (unsigned channel = 0; channel < 3; ++channel) {
            if (split && channel == (model == ColorModel::Hsl ? 0U : 2U))
              continue;
            // Radian fixtures use zero hue; pi/rational fixtures retain 4
            // turns.
            const auto value =
                polar && unit == ColorHueUnit::Radian &&
                        channel == (model == ColorModel::Hsl ? 0U : 2U)
                    ? 0.
                    : row[channel];
            values.push_back(double_bits(value));
          }
        std::reverse(values.begin(), values.end());
        auto colors = d.source({ElementType::Float64, {2, channels}}, values,
                               {1 + (2 * channels - 1) * 8,
                                {-static_cast<int64_t>(channels * 8), -8}});
        std::vector<ResultRef> inputs{query, stops, colors};
        if (split) {
          inputs.push_back(
              d.source({ElementType::Int64, {2}}, {0, 8}, {1, {8}}));
          inputs.push_back(d.source({ElementType::Int64, {2}}, {2}, {1, {0}}));
        }
        if (!math_profile_available(d, node, inputs))
          continue;
        auto prepared = d.prepare(node.operation, inputs, node.parameters);
        auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
        auto roi = take(Footprint::from_regions(
            {2, 2, 3}, {Region({{1, 1}, {0, 1}, {1, 1}})}));
        auto executed = d.context->execute_fragments(frozen, {{"out", roi}});
        if (!executed.ok() &&
            executed.status().code == ErrorCode::BackendUnavailable)
          continue;
        auto result = take(std::move(executed));
        retained = result.results.at("out");
        const auto& tensor = retained.schema().tensors[0];
        auto description = take(decode_color_array(tensor.facets[0]));
        require(retained.schema().id == "photospider.tensor" &&
                    tensor.key == "samples" &&
                    tensor.atomic_trailing_axes == 1 &&
                    tensor.sample_shape() == std::vector<uint64_t>({2, 2, 3}) &&
                    description.model == model &&
                    (!polar || description.hue == ColorHueUnit::PiMultiple) &&
                    take(retained.descriptor()).tensor_coverage(0) ==
                        take(Footprint::all({2, 2, 3})),
                "ramp publishes full color Result with batched global shape");
        for (unsigned i = 0; i < 4; ++i) {
          for (unsigned c = 0; c < 3; ++c) {
            double expected = i < 2    ? low[c]
                              : i == 3 ? high[c]
                                       : (low[c] + high[c]) * .5;
            if (polar && unit == ColorHueUnit::Radian &&
                c == (model == ColorModel::Hsl ? 0U : 2U))
              expected = 0;
            require(
                read_bits(retained, {i / 2, i % 2, c}) == double_bits(expected),
                "ramp exact interpolation/clamp uses original coordinates");
          }
        }
        for (unsigned port = 0; port < inputs.size(); ++port) {
          auto shape = inputs[port].schema().tensors[0].sample_shape();
          for (uint32_t role : {1U, 4U})
            require(take(result.dependencies.potential_dirty(
                             "input" + std::to_string(port),
                             take(Footprint::all(shape)), role))
                            .at("out") == take(tensor.close_samples(roi)),
                    "ramp retains complete data and validation dirty support");
        }
      }
    }
  }
  d.context.reset();
  require(read_bits(retained, {1, 0, 0}) == double_bits(2),
          "ramp Result outlives execution context");
}
void color_ramp_boundaries() {
  Driver d;
  auto node = ramp_node(ColorModel::Xyz, ColorHueUnit::Radian);
  auto query = d.source({ElementType::Float64, {1}}, {0}, {1, {0}});
  auto midpoint =
      d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
  auto stops =
      d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
  const std::vector<uint64_t> bad_rows{UINT64_C(0x8000000000000000),
                                       double_bits(1),
                                       double_bits(2),
                                       UINT64_C(0x7ff8000000000042),
                                       0,
                                       0};
  auto colors =
      d.source({ElementType::Float64, {2, 3}}, bad_rows, {1, {24, 8}});
  auto direct =
      take(d.run(node.operation, {query, stops, colors}, {}, node.parameters))
          .results.at("out");
  require(read_bits(direct, {0, 0}) == UINT64_C(0x8000000000000000),
          "ramp exact hit preserves -0 and skips unused generic NaN");
  auto typed = d.source({ElementType::Float64, {2, 3}}, bad_rows, {1, {24, 8}},
                        {take(encode_color_array(ColorArrayDescriptor{}))});
  auto failed =
      d.run(node.operation, {query, stops, typed}, {}, node.parameters);
  require(
      !failed.ok() && failed.status().detail.input_id == 13 &&
          failed.status().detail.origin == FailureOrigin::Domain,
      "ramp typed validation observes remote NaN before stencil arithmetic");
  auto empty = take(d.run(node.operation, {query, stops, typed},
                          take(Footprint::none({1, 3})), node.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty ramp avoids typed payload validation");
  const uint64_t nz = UINT64_C(0x8000000000000000);
  auto mixed = d.source(
      {ElementType::Float64, {2, 3}},
      {nz, double_bits(1), double_bits(2), nz, double_bits(3), double_bits(4)},
      {1, {24, 8}});
  auto blended =
      take(d.run(node.operation, {midpoint, stops, mixed}, {}, node.parameters))
          .results.at("out");
  require(read_bits(blended, {0, 0}) == 0 &&
              read_bits(blended, {0, 1}) == double_bits(2),
          "ramp uses whole-row identity and mixed exact zero is +0");
  auto identical = d.source({ElementType::Float64, {2, 3}},
                            {nz, double_bits(1), double_bits(2)}, {1, {0, 8}});
  auto kept = take(d.run(node.operation, {midpoint, stops, identical}, {},
                         node.parameters))
                  .results.at("out");
  require(read_bits(kept, {0, 0}) == nz,
          "ramp complete identical rows preserve direct negative zero");
  auto bad_stops =
      d.source({ElementType::Float64, {2}}, {double_bits(1), 0}, {1, {8}});
  auto nan_query = d.source({ElementType::Float64, {1}},
                            {UINT64_C(0x7ff8000000000042)}, {1, {0}});
  failed = d.run(node.operation, {nan_query, bad_stops, colors}, {},
                 node.parameters);
  require(!failed.ok() && failed.status().detail.scope == FailureScope::Run &&
              failed.status().message.find("port=1 row=1") != std::string::npos,
          "ramp validates all stops before query finiteness");
  auto rejecting = ramp_node(ColorModel::Xyz, ColorHueUnit::Radian,
                             CpuNumericProfile::Strict, true);
  auto queries =
      d.source({ElementType::Float64, {2}}, {0, double_bits(2)}, {1, {8}});
  auto point =
      take(Footprint::from_regions({2, 3}, {Region({{0, 1}, {0, 1}})}));
  failed = d.run(rejecting.operation, {queries, stops, colors}, point,
                 rejecting.parameters);
  require(!failed.ok() &&
              failed.status().message.find("port=0 row=1") != std::string::npos,
          "Whole ramp rejects an unrequested query before selected colors");
  for (auto model : {ColorModel::Cielch, ColorModel::Hsl}) {
    auto rational = ramp_node(model, ColorHueUnit::RationalPi);
    auto pairs = d.source(
        {ElementType::Float64, {2, 2}},
        {double_bits(.25), double_bits(.5), double_bits(.75), double_bits(-1)},
        {1, {16, 8}});
    auto p = d.source({ElementType::Int64, {2}},
                      {UINT64_C(0x8000000000000000), INT64_MAX}, {1, {8}});
    auto q = d.source({ElementType::Int64, {2}}, {1, 0}, {1, {8}});
    auto result = take(d.run(rational.operation, {query, stops, pairs, p, q},
                             {}, rational.parameters))
                      .results.at("out");
    require(read_bits(result, {0, model == ColorModel::Hsl ? 0U : 2U}) ==
                double_bits(-0x1p63),
            "rational ramp retains INT64_MIN bits and ignores unused q<=0");
    failed = d.run(rational.operation, {midpoint, stops, pairs, p, q}, {},
                   rational.parameters);
    require(!failed.ok() && failed.status().detail.scope == FailureScope::Run,
            "rational ramp validates selected chroma/denominator");
  }
}
void color_ramp_icc() {
  Driver d;
  ResultRef retained, empty, generic;
  ColorProfileIdentity identity;
  {
    auto bytes = numeric_fixture::fixture();
    auto profile =
        take(IccProfile::import({bytes.data(), bytes.size()}, d.root));
    identity = profile.identity();
    auto resources = take(ResourceBindings::create({profile}, d.root));
    auto query =
        d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
    auto stops =
        d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
    auto colors = d.source(
        {ElementType::Float64, {2, 4}},
        {0, 0, 0, 0, double_bits(1), double_bits(.5), 0, double_bits(.25)},
        {1, {32, 8}});
    auto node = take(numeric::color_ramp_cmyk_node(
        7, WorkflowInputReference{11}, WorkflowInputReference{12},
        WorkflowInputReference{13}, ElementType::Float64,
        numeric::color_ramp_cmyk_description(identity)));
    auto prepared = d.prepare(node.operation, {query, stops, colors},
                              node.parameters, "values", resources);
    auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
    retained = take(d.context->execute_fragments(
                        frozen, {{"out", take(Footprint::all({1, 4}))}}))
                   .results.at("out");
    empty = take(d.context->execute_fragments(
                     frozen, {{"out", take(Footprint::none({1, 4}))}}))
                .results.at("out");
    auto numeric =
        d.prepare("numeric.clamp_strict", {retained, retained, retained}, {},
                  "values", resources);
    generic = take(d.context->execute_fragments(
                       take(d.context->freeze(numeric.plan, numeric.bindings)),
                       {{"out", take(Footprint::all({1, 4}))}}))
                  .results.at("out");
  }
  d.context.reset();
  require(empty.resources().icc_profile(identity).ok() &&
              take(empty.descriptor()).tensor_coverage(0).empty() &&
              generic.resources().size() == 0 &&
              generic.schema().tensors[0].facets.empty() &&
              read_bits(generic, {0, 0}) == double_bits(.5),
          "Empty CMYK owns profile while generic math drops unreferenced ICC");
  require(retained.resources().icc_profile(identity).ok() &&
              read_bits(retained, {0, 0}) == double_bits(.5) &&
              read_bits(retained, {0, 1}) == double_bits(.25) &&
              read_bits(retained, {0, 3}) == double_bits(.125),
          "CMYK Result retains exact colors and ICC owner after all producers "
          "retire");
}
void color_ramp_active_cancel() {
  for (auto model : {ColorModel::Rgb, ColorModel::Xyz, ColorModel::Hsl}) {
    Driver d;
    const auto original = d.registry;
    auto node =
        ramp_node(model, model == ColorModel::Hsl ? ColorHueUnit::RationalPi
                                                  : ColorHueUnit::Radian);
    auto count = std::make_shared<unsigned>(0);
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                ->register_operation(observed_numeric(
                    original, node.operation, count, stop, triggered, false,
                    model == ColorModel::Rgb ? 640 : 144))
                .ok(),
            "ramp arithmetic cancellation observer registration");
    require(d.registry->freeze().ok(), "ramp cancellation observer freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto q = d.source({ElementType::Float64, {1}}, {double_bits(.5)}, {1, {0}});
    auto k =
        d.source({ElementType::Float64, {2}}, {0, double_bits(1)}, {1, {8}});
    const unsigned channels = model == ColorModel::Hsl ? 2 : 3;
    std::vector<uint64_t> colors(channels * 2);
    std::fill(colors.begin() + channels, colors.end(), double_bits(1));
    auto c = d.source({ElementType::Float64, {2, channels}}, colors,
                      {1, {static_cast<int64_t>(channels * 8), 8}});
    std::vector<ResultRef> inputs{q, k, c};
    if (model == ColorModel::Hsl) {
      inputs.push_back(d.source({ElementType::Int64, {2}}, {0, 4}, {1, {8}}));
      inputs.push_back(d.source({ElementType::Int64, {2}}, {1}, {1, {0}}));
    }
    const auto baseline = d.root.statistics().live;
    auto failed =
        d.run(node.operation, inputs, {}, node.parameters, stop->token());
    require(*triggered && !failed.ok() &&
                failed.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "ramp active exact slot cancellation discards writer and Root stop "
            "index");
    d.context.reset();
    require(d.root.statistics().live.values == baseline.values,
            "ramp cancelled arithmetic releases all context resources");
  }
}
}  // namespace
int main() {
  try {
    color_ramp_workflows();
    color_ramp_icc();
    color_ramp_boundaries();
    color_ramp_active_cancel();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
