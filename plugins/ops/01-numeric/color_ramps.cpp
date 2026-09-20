#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_color_coordinate.hpp"
#include "01-numeric/exact_rgb.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
Status mismatch(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
struct RampProgram {
  ColorArrayDescriptor description;
  std::optional<ColorHueUnit> input_hue, output_hue;
  ColorAssociation output_association = ColorAssociation::None;
  SequenceProfile profile;
  unsigned channels = 3;
  bool rational = false, reject = false;
};
Result<OperationPreparation> prepare(
    ColorModel model, std::optional<ColorHueUnit> hue, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<OperationPreparation>;
  auto parsed = color_array_from_parameter(
      std::get<std::string>(parameters.at("color_description")));
  if (!parsed.ok())
    return Answer(parsed.status());
  auto description = parsed.take_value();
  const bool rational = hue == ColorHueUnit::RationalPi;
  if (description.model != model || description.hue != hue ||
      description.source_layout != (rational
                                        ? ColorSourceLayout::RationalHueSplit
                                        : ColorSourceLayout::Interleaved))
    return Answer(
        numeric_ops::array_parameter_error("ramp color model/hue mismatch"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (domain != "clamp" && domain != "reject"))
    return Answer(
        numeric_ops::array_parameter_error("invalid ramp dtype/domain"));
  const unsigned channels =
      model == ColorModel::Cmyk ||
              (model == ColorModel::Rgb &&
               description.association != ColorAssociation::None)
          ? 4
          : 3;
  if (inputs.size() != (rational ? 5U : 3U))
    return Answer(mismatch("color-ramp input count mismatch"));
  for (std::size_t port = 0; port < inputs.size(); ++port) {
    const auto& d = inputs[port].descriptor;
    if ((port < 3 && d.element_type != ElementType::Float32 &&
         d.element_type != ElementType::Float64) ||
        (port >= 3 && d.element_type != ElementType::Int64) ||
        d.shape.empty() ||
        d.shape.size() > (port == 0   ? 7U
                          : port == 2 ? 2U
                                      : 1U) ||
        (port && d.shape.size() != (port == 2 ? 2U : 1U)))
      return Answer(mismatch("invalid color-ramp port rank/dtype"));
    std::uint64_t count = 1;
    for (auto extent : d.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(mismatch("color-ramp logical product exceeds 2^40"));
      count *= extent;
    }
    if (!port && count > (UINT64_C(1) << 40) / channels)
      return Answer(mismatch("color-ramp output product exceeds 2^40"));
  }
  const auto knots = inputs[1].descriptor.shape[0];
  if (!knots || knots > 65536 || inputs[2].descriptor.shape[0] != knots ||
      inputs[2].descriptor.shape[1] != (rational ? 2 : channels) ||
      (rational && (inputs[3].descriptor.shape[0] != knots ||
                    inputs[4].descriptor.shape[0] != knots)))
    return Answer(
        mismatch("ramp requires matching K=1..65536 and color channels"));
  for (const auto& facet : inputs[2].facets) {
    if (facet.key != "photospider.color-array")
      continue;
    if (rational)
      return Answer(
          mismatch("split hue coordinates are not a complete ColorArray"));
    auto expected = encode_color_array(description);
    if (!expected.ok())
      return Answer(expected.status());
    if (facet.version != expected.value().version ||
        facet.payload != expected.value().payload)
      return Answer(mismatch("attached colors differ from ramp description"));
  }
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  auto program = std::make_shared<RampProgram>();
  program->description = description;
  program->input_hue = hue;
  program->profile = profile;
  program->channels = channels;
  program->rational = rational;
  program->reject = domain == "reject";
  if (model == ColorModel::Rgb) {
    const auto& association =
        std::get<std::string>(parameters.at("output_association"));
    if ((channels == 3 && association != "none") ||
        (channels == 4 && association != "straight" &&
         association != "premultiplied"))
      return Answer(numeric_ops::array_parameter_error(
          "RGB output association disagrees with channel arity"));
    program->output_association = association == "none" ? ColorAssociation::None
                                  : association == "straight"
                                      ? ColorAssociation::Straight
                                      : ColorAssociation::Premultiplied;
    description.association = program->output_association;
  }
  if (hue) {
    const auto& output =
        std::get<std::string>(parameters.at("output_hue_unit"));
    if (output != "radian" && output != "pi_multiple")
      return Answer(
          numeric_ops::array_parameter_error("invalid ramp output hue unit"));
    program->output_hue =
        output == "radian" ? ColorHueUnit::Radian : ColorHueUnit::PiMultiple;
    description.hue = program->output_hue;
    description.source_layout = ColorSourceLayout::Interleaved;
  }
  auto facet = encode_color_array(description);
  if (!facet.ok())
    return Answer(facet.status());
  auto shape = inputs[0].descriptor.shape;
  shape.push_back(channels);
  OperationPreparation prepared;
  prepared.state = std::move(program);
  prepared.outputs.resize(1);
  auto& output = prepared.outputs[0];
  output.metadata.descriptor = {
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      std::move(shape)};
  output.metadata.facets = {facet.take_value()};
  output.metadata.atomic_trailing_axes = 1;
  output.regional_atomic = true;
  return Answer(std::move(prepared));
}
struct RampPoint {
  std::array<std::uint64_t, 7> coordinate{};
  std::uint64_t query = 0, offset = 0;
  std::size_t fragment = 0;
  unsigned first = 0, count = 0;
};
template <bool rgb>
struct RampState {
  const RampProgram* program;
  std::conditional_t<rgb, numeric_ops::ExactRgb,
                     numeric_ops::ExactColorCoordinate>
      arithmetic;
  unsigned stage = 0, rank = 0;
  ResourceVector<std::uint64_t> stops;
  ResourceVector<RampPoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  explicit RampState(const RampProgram* value)
      : program(value), arithmetic(value->profile) {}
  std::vector<std::uint64_t> coordinate(const RampPoint& point) const {
    return {point.coordinate.begin(), point.coordinate.begin() + rank};
  }
  Status failure(const DependencyPhase& phase, const RampPoint& point,
                 unsigned port, std::uint64_t row, const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status status{ErrorCode::OperationFailed,
                  std::string(message) + "; port=" + std::to_string(port) +
                      " row=" + std::to_string(row),
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = rank;
    std::copy_n(point.coordinate.begin(), rank, atom.coordinate.begin());
    status.detail.atom = atom;
    return status;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const RampPoint& point) const {
    auto charged =
        phase.consume_work(phase.inputs[port].fragments().size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    std::uint64_t bits = 0;
    const auto type = phase.query.inputs[port].descriptor.element_type;
    const bool narrow = type == ElementType::Float32;
    auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    if (type == ElementType::Int64)
      return Result<std::uint64_t>(bits);
    const auto parts = BinaryParts::decode(bits, narrow);
    if (parts.nan || parts.infinite)
      return Result<std::uint64_t>(
          failure(phase, point, port, at[0], "nonfinite ramp input"));
    if (narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!parts.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(parts.significand);
        bits = sign |
               (static_cast<std::uint64_t>(parts.exponent + top + 1023) << 52) |
               ((parts.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
      }
    }
    return Result<std::uint64_t>(bits);
  }
  Status initialize(const DependencyPhase& phase) {
    rank = phase.query.inputs[0].descriptor.shape.size();
    auto count = phase.query.observations.element_count();
    if (!count.ok())
      return count.status();
    auto status = phase.consume_work(count.value() * (rank + 2) + 1);
    if (!status.ok())
      return status;
    stops.resize(phase.query.inputs[1].descriptor.shape[0]);
    points.reserve(count.value());
    outputs.reserve(phase.query.outputs.boxes().size());
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), rank + 1);
    for (const auto& region : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              region, phase.allocator);
      if (!allocated.ok())
        return allocated.status();
      RampPoint point;
      point.fragment = outputs.size();
      std::uint64_t count = 1;
      for (unsigned axis = 0; axis < rank; ++axis) {
        point.coordinate[axis] = region.dimensions()[axis].offset;
        count *= region.dimensions()[axis].extent;
      }
      for (std::uint64_t i = 0; i < count; ++i) {
        status = phase.consume_work(1);
        if (!status.ok())
          return status;
        point.offset = i * program->channels;
        points.push_back(point);
        for (unsigned axis = rank; axis; --axis) {
          const auto dim = region.dimensions()[axis - 1];
          if (++point.coordinate[axis - 1] < dim.offset + dim.extent)
            break;
          point.coordinate[axis - 1] = dim.offset;
        }
      }
      outputs.push_back(allocated.take_value());
    }
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase,
                              unsigned stage_port) {
    using Answer = Result<DependencyPoll>;
    request_capacity = dependency_internal::metadata_owner(
        4096 + points.size() * (program->rational ? 24576 : 8192));
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto charged = phase.consume_work(8);
      if (!charged.ok())
        return Answer(charged);
      AtomCertificate certificate{coordinate(point), {}};
      const unsigned last =
          stage_port == 2 && program->rational ? 5 : stage_port + 1;
      for (unsigned port = stage_port; port < last; ++port) {
        std::vector<RegionDimension> dimensions;
        if (port == 0) {
          for (unsigned axis = 0; axis < rank; ++axis)
            dimensions.push_back({point.coordinate[axis], 1});
        } else if (port == 1) {
          dimensions = {{0, stops.size()}};
        } else {
          dimensions = {{point.first, point.count}};
          if (port == 2)
            dimensions.push_back(
                {0, program->rational ? 2 : program->channels});
        }
        auto support =
            Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                    {Region(dimensions)}, phase.sets);
        if (!support.ok())
          return Answer(support.status());
        auto validation = input_internal::validation_closure(
            phase.query.inputs[port], support.value(), phase.sets,
            phase.consume_work);
        if (!validation.ok())
          return Answer(validation.status());
        certificate.inputs.push_back(
            {port,
             static_cast<std::uint8_t>(port < 2 ? 2 : 1),
             support.take_value(),
             {}});
        certificate.inputs.push_back({port, 4, validation.take_value(), {}});
      }
      certificates.push_back(std::move(certificate));
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  Status classify(const DependencyPhase& phase, RampPoint* point) {
    auto value = read(phase, 0, coordinate(*point), *point);
    if (!value.ok())
      return value.status();
    point->query = value.value();
    const auto key = BinaryParts::decode(value.value(), false).order_key();
    unsigned lo = 0, hi = stops.size();
    while (lo < hi) {
      auto status = phase.consume_work(1);
      if (!status.ok())
        return status;
      const auto mid = lo + (hi - lo) / 2;
      if (BinaryParts::decode(stops[mid], false).order_key() < key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < stops.size() &&
        BinaryParts::decode(stops[lo], false).order_key() == key) {
      point->first = lo;
      point->count = 1;
    } else if (!lo || lo == stops.size()) {
      if (program->reject)
        return failure(phase, *point, 0, point->coordinate[0],
                       "ramp query outside stops");
      point->first = lo ? stops.size() - 1 : 0;
      point->count = 1;
    } else {
      point->first = lo - 1;
      point->count = 2;
    }
    return Status::success();
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics diagnostic;
    diagnostic.profile = static_cast<CpuNumericProfile>(
        static_cast<unsigned>(program->profile) + 1);
    const auto length = std::snprintf(
        diagnostic.implementation.data(), diagnostic.implementation.size(),
        "photospider.color-ramp/1;model-%u;%s;%s",
        static_cast<unsigned>(program->description.model),
        rgb ? "exact-linear-light" : "exact-rational-pi",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostic.implementation.size())
      return {ErrorCode::Internal, "color ramp diagnostic identity"};
    diagnostic.evaluated_values = evaluated;
    diagnostic.copied_elements = copied;
    return phase.report_numeric(diagnostic);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(32768);
    request_capacity.reset();
    if (!stage) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
      stage = 1;
      return need(phase, 1);
    }
    if (stage == 1) {
      for (unsigned row = 0; row < stops.size(); ++row) {
        auto value = read(phase, 1, {row}, points.front());
        if (!value.ok())
          return Answer(value.status());
        stops[row] = value.value();
        if (row && BinaryParts::decode(stops[row - 1], false).order_key() >=
                       BinaryParts::decode(stops[row], false).order_key())
          return Answer(failure(phase, points.front(), 1, row,
                                "ramp stops require strict increase"));
      }
      stage = 2;
      return need(phase, 0);
    }
    if (stage == 2) {
      for (auto& point : points) {
        auto status = classify(phase, &point);
        if (!status.ok())
          return Answer(status);
      }
      stage = 3;
      return need(phase, 2);
    }
    const bool narrow =
        phase.query.output.descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    const unsigned source_channels = program->rational ? 2 : program->channels;
    const unsigned hue_channel =
        program->description.model == ColorModel::Hsl ? 0 : 2;
    for (const auto& point : points) {
      std::array<std::array<std::uint64_t, 4>, 2> rows{};
      std::array<std::int64_t, 2> numerator{}, denominator{};
      for (unsigned row = 0; row < point.count; ++row) {
        for (unsigned channel = 0; channel < source_channels; ++channel) {
          auto value = read(phase, 2, {point.first + row, channel}, point);
          if (!value.ok())
            return Answer(value.status());
          rows[row][channel] = value.value();
          const auto decoded = BinaryParts::decode(value.value(), false);
          if ((program->description.model == ColorModel::Cmyk &&
               ((decoded.negative && decoded.magnitude) ||
                decoded.order_key() >
                    BinaryParts::decode(UINT64_C(0x3ff0000000000000), false)
                        .order_key())) ||
              ((program->description.model == ColorModel::Cielch ||
                program->description.model == ColorModel::Oklch) &&
               channel == 1 && decoded.negative && decoded.magnitude))
            return Answer(failure(phase, point, 2, point.first + row,
                                  "ramp color component outside model domain"));
        }
        if (program->rational) {
          auto p = read(phase, 3, {point.first + row}, point);
          auto q = read(phase, 4, {point.first + row}, point);
          if (!p.ok())
            return Answer(p.status());
          if (!q.ok())
            return Answer(q.status());
          const auto pbits = p.value(), qbits = q.value();
          std::memcpy(&numerator[row], &pbits, 8);
          std::memcpy(&denominator[row], &qbits, 8);
          if (denominator[row] <= 0)
            return Answer(failure(phase, point, 4, point.first + row,
                                  "ramp hue denominator must be positive"));
        }
        if constexpr (rgb) {
          if (program->channels == 4) {
            const auto alpha = BinaryParts::decode(rows[row][3], false);
            if ((alpha.negative && alpha.magnitude) ||
                alpha.order_key() >
                    BinaryParts::decode(UINT64_C(0x3ff0000000000000), false)
                        .order_key())
              return Answer(failure(phase, point, 2, point.first + row,
                                    "RGB alpha outside [0,1]",
                                    FailureReason::InvalidAssociation));
            if (!alpha.magnitude && program->description.association ==
                                        ColorAssociation::Premultiplied) {
              for (unsigned channel = 0; channel < 3; ++channel)
                if (BinaryParts::decode(rows[row][channel], false).magnitude)
                  return Answer(
                      failure(phase, point, 2, point.first + row,
                              "zero alpha requires zero premultiplied RGB",
                              FailureReason::InvalidAssociation));
            }
          }
        }
      }
      const bool direct = point.count == 1 ||
                          (rows[0] == rows[1] && numerator[0] == numerator[1] &&
                           denominator[0] == denominator[1]);
      const std::array<std::uint64_t, 2> knots{
          stops[point.first], stops[point.first + point.count - 1]};
      if constexpr (rgb) {
        auto recorded = report(phase, program->channels, 0);
        if (!recorded.ok())
          return Answer(recorded);
        const auto transfer = *program->description.transfer;
        std::uint64_t gamma = 0;
        if (transfer.gamma)
          std::memcpy(&gamma, &*transfer.gamma, 8);
        const bool linear = transfer.kind == ColorTransferKind::Linear ||
                            (transfer.kind == ColorTransferKind::Gamma &&
                             gamma == UINT64_C(0x3ff0000000000000));
        const bool square = transfer.kind == ColorTransferKind::Gamma &&
                            gamma == UINT64_C(0x4000000000000000);
        auto result =
            direct || linear || square
                ? arithmetic.algebraic(knots, point.query, rows,
                                       program->description.association,
                                       program->output_association, direct,
                                       square, narrow, phase.consume_work)
                : arithmetic.nonlinear(knots, point.query, rows,
                                       program->description.association,
                                       program->output_association,
                                       transfer.kind == ColorTransferKind::Srgb,
                                       gamma, narrow, phase.consume_work);
        if (!result.ok()) {
          auto status = result.status();
          if (status.code == ErrorCode::OperationFailed)
            return Answer(failure(phase, point, 2, point.first,
                                  status.message.c_str(), status.reason));
          return Answer(status);
        }
        for (unsigned channel = 0; channel < program->channels; ++channel) {
          const auto bits = result.value()[channel];
          std::memcpy(
              static_cast<std::uint8_t*>(outputs[point.fragment].data()) +
                  (point.offset + channel) * width,
              &bits, width);
        }
        recorded = report(phase, 0, program->channels);
        if (!recorded.ok())
          return Answer(recorded);
      } else {
        for (unsigned channel = 0; channel < program->channels; ++channel) {
          const bool hue = program->input_hue && channel == hue_channel;
          const bool rational = hue && program->rational;
          unsigned source = channel;
          if (program->rational && !hue && hue_channel == 0)
            --source;
          std::array<std::uint64_t, 2> values{};
          if (!rational)
            values = {rows[0][source], rows[1][source]};
          int pi_power = 0;
          if (hue && program->input_hue == ColorHueUnit::Radian &&
              program->output_hue == ColorHueUnit::PiMultiple)
            pi_power = -1;
          else if (hue && program->input_hue != ColorHueUnit::Radian &&
                   program->output_hue == ColorHueUnit::Radian)
            pi_power = 1;
          auto recorded = report(phase, 1, 0);
          if (!recorded.ok())
            return Answer(recorded);
          auto result = arithmetic.evaluate(
              knots, point.query, values, numerator, denominator, rational,
              direct, pi_power, narrow, phase.consume_work);
          if (!result.ok())
            return Answer(result.status());
          if (BinaryParts::decode(result.value(), narrow).infinite)
            return Answer(failure(phase, point, 2, point.first,
                                  "ramp output overflow",
                                  FailureReason::ArithmeticOverflow));
          const auto bits = result.value();
          std::memcpy(
              outputs[point.fragment].data() + (point.offset + channel) * width,
              &bits, width);
          recorded = report(phase, 0, 1);
          if (!recorded.ok())
            return Answer(recorded);
        }
      }
    }
    ResourceVector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto status = phase.consume_work(1);
      if (!status.ok())
        return Answer(status);
      auto published = std::move(output).publish(phase.query.output.facets,
                                                 phase.query.resources);
      if (!published.ok())
        return Answer(published.status());
      auto retained = publication->retain(published.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    auto result =
        publication->finish(phase.query.output.descriptor, phase.query.outputs,
                            values.data(), values.size(), phase.sets,
                            phase.query.output.facets, phase.query.resources);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition operation(const std::string& name, ColorModel model,
                              std::optional<ColorHueUnit> hue,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = name;
  auto& traits = result.traits;
  traits.input_count = hue == ColorHueUnit::RationalPi ? 5 : 3;
  traits.input_schema.resize(traits.input_count);
  for (unsigned port = 0; port < traits.input_count; ++port)
    traits.input_schema[port].element_type_mask = port < 3 ? 12 : 2;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"color_description", OperationParameterType::String},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  if (model == ColorModel::Rgb)
    traits.parameter_schema.push_back(
        {"output_association", OperationParameterType::String});
  if (hue)
    traits.parameter_schema.push_back(
        {"output_hue_unit", OperationParameterType::String});
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = model == ColorModel::Rgb
                                  ? sizeof(RampState<true>)
                                  : sizeof(RampState<false>);
  output.maximum_dependency_stages = 4;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  result.prepare_static = [model, hue, profile](const auto& inputs,
                                                const auto& params) {
    return prepare(model, hue, profile, inputs, params);
  };
  result.start_dependency = [model](const auto& query, const auto& allocator) {
    if (model == ColorModel::Rgb)
      return DependencyContinuation::make<RampState<true>>(
          allocator, static_cast<const RampProgram*>(query.prepared->state()));
    return DependencyContinuation::make<RampState<false>>(
        allocator, static_cast<const RampProgram*>(query.prepared->state()));
  };
  return result;
}
}  // namespace
Status register_color_ramps(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& model : {std::make_pair("rgb", ColorModel::Rgb),
                              std::make_pair("xyz", ColorModel::Xyz),
                              std::make_pair("cmyk", ColorModel::Cmyk),
                              std::make_pair("cielab", ColorModel::Cielab),
                              std::make_pair("oklab", ColorModel::Oklab),
                              std::make_pair("ycbcr", ColorModel::Ycbcr)}) {
      auto status = registry->register_operation(operation(
          std::string("curve.color_ramp_") + model.first + profile.first,
          model.second, {}, profile.second));
      if (!status.ok())
        return status;
    }
    for (const auto& model : {std::make_pair("cielch", ColorModel::Cielch),
                              std::make_pair("oklch", ColorModel::Oklch),
                              std::make_pair("hsl", ColorModel::Hsl)}) {
      for (const auto& unit :
           {std::make_pair("", ColorHueUnit::Radian),
            std::make_pair("_pi", ColorHueUnit::PiMultiple),
            std::make_pair("_rational_pi", ColorHueUnit::RationalPi)}) {
        auto status = registry->register_operation(
            operation(std::string("curve.color_ramp_") + model.first +
                          unit.first + profile.first,
                      model.second, unit.second, profile.second));
        if (!status.ok())
          return status;
      }
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
