#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_color_coordinate.hpp"
#include "01-numeric/exact_rgb.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
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
  std::vector<ValueDescriptor> descriptors;
  for (std::size_t port = 0; port < inputs.size(); ++port) {
    if (!inputs[port].result_schema ||
        !inputs[port].result_schema->fields.empty() ||
        inputs[port].result_schema->tensors.size() != 1)
      return Answer(mismatch("color-ramp requires one tensor per Result"));
    const auto& tensor = inputs[port].result_schema->tensors[0];
    descriptors.push_back(
        {tensor.descriptor.element_type, tensor.sample_shape()});
    const auto& d = descriptors.back();
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
  const auto knots = descriptors[1].shape[0];
  if (!knots || knots > 65536 || descriptors[2].shape[0] != knots ||
      descriptors[2].shape[1] != (rational ? 2 : channels) ||
      (rational &&
       (descriptors[3].shape[0] != knots || descriptors[4].shape[0] != knots)))
    return Answer(
        mismatch("ramp requires matching K=1..65536 and color channels"));
  for (const auto& facet : inputs[2].result_schema->tensors[0].facets) {
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
  auto shape = descriptors[0].shape;
  shape.push_back(channels);
  OperationPreparation prepared;
  prepared.state = std::move(program);
  prepared.outputs.resize(1);
  auto& output = prepared.outputs[0];
  auto schema = numeric_ops::numeric_tensor_schema(
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64, shape);
  schema.tensors[0].facets = {facet.take_value()};
  schema.tensors[0].atomic_trailing_axes = 1;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  return Answer(std::move(prepared));
}
struct RampPoint {
  std::uint64_t query = 0;
  unsigned first = 0, count = 0;
};
template <bool rgb>
struct RampState {
  const RampProgram* program;
  std::conditional_t<rgb, numeric_ops::ExactRgb,
                     numeric_ops::ExactColorCoordinate>
      arithmetic;
  const ResultProgramPhase& phase;
  std::function<Status(std::uint64_t)> consume;
  std::vector<std::uint64_t> at;
  ResourceVector<std::uint64_t> stops;
  std::array<std::optional<numeric_ops::MathTensorReader>, 5> inputs;
  RampState(const RampProgram* value, const ResultProgramPhase& invocation)
      : program(value),
        arithmetic(value->profile),
        phase(invocation),
        consume(invocation.consume_work),
        at(invocation.query.inputs[0]
               .result_schema->tensors[0]
               .sample_shape()
               .size(),
           0),
        stops(ResourceAllocator<std::uint64_t>(invocation.resources)) {
    for (unsigned port = 0; port < (program->rational ? 5U : 3U); ++port)
      inputs[port].emplace(phase.tensors->at({port, 0}),
                           phase.query.cancellation);
  }
  Status work(std::uint64_t amount) const { return consume(amount); }
  void advance() {
    numeric_ops::math_next(
        at, phase.query.inputs[0].result_schema->tensors[0].sample_shape());
  }
  Status failure(unsigned port, std::uint64_t row, const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status status{ErrorCode::OperationFailed,
                  std::string(message) + "; port=" + std::to_string(port) +
                      " row=" + std::to_string(row),
                  reason,
                  {FailureOrigin::Domain, FailureScope::Run}};
    return status;
  }
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& coordinate) {
    auto charged = work(coordinate.size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto type =
        phase.tensors->at({port, 0}).spec().descriptor.element_type;
    const bool narrow = type == ElementType::Float32;
    auto bits = inputs[port]->bits(coordinate);
    if (type == ElementType::Int64)
      return Result<std::uint64_t>(bits);
    const auto parts = BinaryParts::decode(bits, narrow);
    if (parts.nan || parts.infinite)
      return Result<std::uint64_t>(
          failure(port, coordinate[0], "nonfinite ramp input"));
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
  Status classify(RampPoint* point) {
    auto value = read(0, at);
    if (!value.ok())
      return value.status();
    point->query = value.value();
    const auto key = BinaryParts::decode(value.value(), false).order_key();
    unsigned lo = 0, hi = stops.size();
    while (lo < hi) {
      auto status = consume(1);
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
        return failure(0, at[0], "ramp query outside stops");
      point->first = lo ? stops.size() - 1 : 0;
      point->count = 1;
    } else {
      point->first = lo - 1;
      point->count = 2;
    }
    return Status::success();
  }
  Status write(const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    stops.resize(
        phase.query.inputs[1].result_schema->tensors[0].sample_shape()[0]);
    for (unsigned row = 0; row < stops.size(); ++row) {
      auto value = read(1, {row});
      if (!value.ok())
        return value.status();
      stops[row] = value.value();
      if (row && BinaryParts::decode(stops[row - 1], false).order_key() >=
                     BinaryParts::decode(stops[row], false).order_key())
        return failure(1, row, "ramp stops require strict increase");
    }
    const auto count = math_take(
        phase.query.inputs[0].result_schema->tensors[0].sample_count());
    const auto lower = BinaryParts::decode(stops.front(), false).order_key();
    const auto upper = BinaryParts::decode(stops.back(), false).order_key();
    for (std::uint64_t i = 0; i < count; ++i, advance()) {
      auto query = read(0, at);
      if (!query.ok())
        return query.status();
      const auto key = BinaryParts::decode(query.value(), false).order_key();
      if (program->reject && (key < lower || key > upper))
        return failure(0, at[0], "ramp query outside stops");
    }
    const auto& resolved = phase.query.output.result_schema->tensors[0];
    const bool narrow =
        resolved.descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    MathTensorWriter output(writers[0]);
    auto coordinate = at;
    coordinate.push_back(0);
    const unsigned source_channels = program->rational ? 2 : program->channels;
    const unsigned hue_channel =
        program->description.model == ColorModel::Hsl ? 0 : 2;
    for (std::uint64_t i = 0; i < count; ++i, advance()) {
      RampPoint point;
      auto status = classify(&point);
      if (!status.ok())
        return status;
      std::array<std::array<std::uint64_t, 4>, 2> rows{};
      std::array<std::int64_t, 2> numerator{}, denominator{};
      for (unsigned row = 0; row < point.count; ++row) {
        for (unsigned channel = 0; channel < source_channels; ++channel) {
          auto value = read(2, {point.first + row, channel});
          if (!value.ok())
            return value.status();
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
            return failure(2, point.first + row,
                           "ramp color component outside model domain");
        }
        if (program->rational) {
          auto p = read(3, {point.first + row});
          auto q = read(4, {point.first + row});
          if (!p.ok())
            return p.status();
          if (!q.ok())
            return q.status();
          const auto pbits = p.value(), qbits = q.value();
          std::memcpy(&numerator[row], &pbits, 8);
          std::memcpy(&denominator[row], &qbits, 8);
          if (denominator[row] <= 0)
            return failure(4, point.first + row,
                           "ramp hue denominator must be positive");
        }
        if constexpr (rgb) {
          if (program->channels == 4) {
            const auto alpha = BinaryParts::decode(rows[row][3], false);
            if ((alpha.negative && alpha.magnitude) ||
                alpha.order_key() >
                    BinaryParts::decode(UINT64_C(0x3ff0000000000000), false)
                        .order_key())
              return failure(2, point.first + row, "RGB alpha outside [0,1]",
                             FailureReason::InvalidAssociation);
            if (!alpha.magnitude && program->description.association ==
                                        ColorAssociation::Premultiplied) {
              for (unsigned channel = 0; channel < 3; ++channel)
                if (BinaryParts::decode(rows[row][channel], false).magnitude)
                  return failure(2, point.first + row,
                                 "zero alpha requires zero premultiplied RGB",
                                 FailureReason::InvalidAssociation);
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
                                       square, narrow, consume)
                : arithmetic.nonlinear(knots, point.query, rows,
                                       program->description.association,
                                       program->output_association,
                                       transfer.kind == ColorTransferKind::Srgb,
                                       gamma, narrow, consume);
        if (!result.ok()) {
          auto status = result.status();
          if (status.code == ErrorCode::OperationFailed)
            return failure(2, point.first, status.message.c_str(),
                           status.reason);
          return status;
        }
        for (unsigned channel = 0; channel < program->channels; ++channel) {
          const auto bits = result.value()[channel];
          coordinate.assign(at.begin(), at.end());
          coordinate.push_back(channel);
          std::memcpy(output.address(coordinate), &bits, width);
        }
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
          auto result = arithmetic.evaluate(knots, point.query, values,
                                            numerator, denominator, rational,
                                            direct, pi_power, narrow, consume);
          if (!result.ok())
            return result.status();
          if (BinaryParts::decode(result.value(), narrow).infinite)
            return failure(2, point.first, "ramp output overflow",
                           FailureReason::ArithmeticOverflow);
          const auto bits = result.value();
          coordinate.assign(at.begin(), at.end());
          coordinate.push_back(channel);
          std::memcpy(output.address(coordinate), &bits, width);
        }
      }
    }
    return work(1);
  }
};
template <bool rgb>
struct RampKernel final {
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using State = RampState<rgb>;
    auto buffer =
        numeric_ops::math_take(phase.allocator.allocate(sizeof(State)));
    static_assert(alignof(State) <= alignof(std::max_align_t));
    std::unique_ptr<State, void (*)(State*)> state(
        new (buffer.data()) State(
            static_cast<const RampProgram*>(phase.query.prepared->state()),
            phase),
        [](auto* value) { value->~State(); });
    return state->write(writers);
  }
};
template <bool rgb>
using RampResultProgram = numeric_ops::WholeTensorProgram<RampKernel<rgb>>;

OperationDefinition operation(const std::string& name, ColorModel model,
                              std::optional<ColorHueUnit> hue,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = name;
  auto& traits = result.traits;
  traits.input_count = hue == ColorHueUnit::RationalPi ? 5 : 3;
  traits.input_schema.resize(traits.input_count);
  for (unsigned port = 0; port < traits.input_count; ++port) {
    traits.input_schema[port].kind = OperationPortKind::Result;
    traits.input_schema[port].element_type_mask = port < 3 ? 12 : 2;
  }
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
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       model == ColorModel::Rgb
                                           ? sizeof(RampResultProgram<true>)
                                           : sizeof(RampResultProgram<false>));
  traits.workspace_bytes = model == ColorModel::Rgb ? sizeof(RampState<true>)
                                                    : sizeof(RampState<false>);
  result.prepare_static = [model, hue, profile](const auto& inputs,
                                                const auto& params) {
    return prepare(model, hue, profile, inputs, params);
  };
  result.start_result = [model](const auto&, const auto& allocator) {
    return model == ColorModel::Rgb
               ? ResultContinuation::make<RampResultProgram<true>>(allocator)
               : ResultContinuation::make<RampResultProgram<false>>(allocator);
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
