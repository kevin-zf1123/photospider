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
#include "common/domain/icc_validation.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/ops/format/transfer.hpp"
#include "photospider/ops/numeric/color_description.hpp"

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
  TensorInterpretation description;
  bool with_alpha = false, linear = false, square = false, srgb = false;
  std::uint64_t gamma = 0;
  std::optional<numeric::HueUnit> input_hue, output_hue;
  SequenceProfile profile;
  unsigned channels = 3;
  bool rational = false;
  unsigned knots = 0;
  std::shared_ptr<const SchemaTemplate> output_schema;
};
Result<OperationPreparation> prepare(
    const std::string& model, std::optional<numeric::HueUnit> hue,
    SequenceProfile profile, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<OperationPreparation>;
  auto parsed = tensor_description_from_parameter(
      std::get<std::string>(parameters.at("color_description")));
  if (!parsed.ok())
    return Answer(parsed.status());
  auto resolved = numeric::resolve_color_tuple(parsed.value());
  if (!resolved.ok())
    return Answer(resolved.status());
  const auto& group = resolved.value();
  const auto& description = group.interpretation;
  const bool rational = hue == numeric::HueUnit::RationalPi;
  if (description.model != model || (group.alpha && model != "rgb") ||
      (hue && group.components[model == "hsl" ? 0 : 2].unit !=
                  numeric::hue_unit_name(*hue)))
    return Answer(numeric_ops::array_parameter_error(
        "ramp color model/hue/alpha mismatch"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (domain != "clamp" && domain != "reject"))
    return Answer(
        numeric_ops::array_parameter_error("invalid ramp dtype/domain"));
  const unsigned channels = static_cast<unsigned>(
      group.components.size() + static_cast<bool>(group.alpha));
  if (inputs.size() != (rational ? 5U : 3U))
    return Answer(mismatch("color-ramp input count mismatch"));
  std::vector<TensorDescriptor> descriptors;
  for (std::size_t port = 0; port < inputs.size(); ++port) {
    auto valid = tensor_ops::check_tensor(inputs[port]);
    if (!valid.ok())
      return Answer(valid);
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
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  auto program = std::make_shared<RampProgram>();
  program->description = description;
  program->input_hue = hue;
  program->profile = profile;
  program->channels = channels;
  program->rational = rational;
  program->knots = knots;
  program->with_alpha = group.alpha.has_value();
  if (model == "rgb") {
    auto transfer = decode_transfer_definition(description.transfer);
    if (!transfer.ok())
      return Answer(transfer.status());
    const auto& definition = transfer.value();
    if (definition.gamma)
      std::memcpy(&program->gamma, &*definition.gamma, 8);
    program->linear = definition.curve == TransferCurve::Linear ||
                      (definition.curve == TransferCurve::PowerGamma &&
                       program->gamma == UINT64_C(0x3ff0000000000000));
    program->square = definition.curve == TransferCurve::PowerGamma &&
                      program->gamma == UINT64_C(0x4000000000000000);
    program->srgb = definition.curve == TransferCurve::Srgb;
  }
  if (hue) {
    const auto& output =
        std::get<std::string>(parameters.at("output_hue_unit"));
    if (output != "radian" && output != "pi_multiple")
      return Answer(
          numeric_ops::array_parameter_error("invalid ramp output hue unit"));
    program->output_hue = output == "radian" ? numeric::HueUnit::Radian
                                             : numeric::HueUnit::PiMultiple;
  }
  auto shape = descriptors[0].shape;
  shape.push_back(channels);
  OperationPreparation prepared;

  prepared.outputs.resize(1);
  auto& output = prepared.outputs[0];
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = {
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64, shape};
  schema.tensors[0].atomic_trailing_axes = 1;
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  program->output_schema = output.metadata.result_schema;
  if (model == "cmyk")
    prepared.resources.push_back(*program->description.profile);
  prepared.state = std::move(program);
  return Answer(std::move(prepared));
}
using RgbMath = numeric_ops::ExactRgb;
using CoordinateMath = numeric_ops::ExactColorCoordinate;
template <bool rgb>
using RampArithmetic = std::conditional_t<rgb, RgbMath, CoordinateMath>;
std::vector<Region> input_regions(const RampProgram& state,
                                  const Region& output) {
  auto query = output.dimensions();
  query.pop_back();
  std::vector<Region> regions{
      Region(std::move(query)), Region::whole({state.knots}),
      Region::whole({state.knots, state.rational ? 2U : state.channels})};
  if (state.rational) {
    regions.push_back(Region::whole({state.knots}));
    regions.push_back(Region::whole({state.knots}));
  }
  return regions;
}
Result<TileSet> required_tiles(const Region& region, const TileGrid& grid,
                               const ResourceBudget& budget,
                               std::uint64_t maximum) {
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < grid.rank(); ++axis) {
    auto d = region.dimensions()[axis];
    std::uint64_t extent = 1;
    if (grid.batch_axis_mask() & (1U << axis))
      extent = d.extent;
    else if (axis == grid.width_axis() || axis == grid.height_axis())
      extent = (d.offset + d.extent - 1) / grid.tile_extent() -
               d.offset / grid.tile_extent() + 1;
    if (extent > maximum / count)
      return Result<TileSet>(
          {ErrorCode::ResourceExhausted, "ramp read tile limit"});
    count *= extent;
  }
  return sdk::tiles_for_region(budget, grid, region);
}
constexpr const char* ramp_contract = "color-ramp-fixed-v2";
std::uint64_t scratch_bytes(const RampProgram& state) {
  return state.description.model == "rgb" ? sizeof(RampArithmetic<true>)
                                          : sizeof(RampArithmetic<false>);
}
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const RampProgram*>(query.static_state);
  auto output = TileGrid::from_schema(*state.output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::vector<TileGrid> grids;
  for (const auto& input : query.inputs) {
    auto grid = TileGrid::from_schema(*input.result_schema, query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    grids.push_back(grid.take_value());
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto index : member.second) {
      auto region = output.value().region(index);
      if (!region.ok())
        return Answer(region.status());
      auto task = builder.add_task("color-ramp", ramp_contract, {},
                                   {scratch_bytes(state), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      const auto regions = input_regions(state, region.value());
      for (unsigned port = 0; port < grids.size(); ++port) {
        auto tiles = required_tiles(regions[port], grids[port], budget,
                                    query.limits.maximum_records);
        if (!tiles.ok())
          return Answer(tiles.status());
        for (auto tile : tiles.value()) {
          auto status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), port, 0, tile);
          if (!status.ok())
            return Answer(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, index);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct PreparedRamp final {
  const RampProgram* program;
  bool narrow = false;
  std::array<bool, 5> input_narrow{};
  fixed_tensor::PointInputs<5> inputs;
};
template <bool rgb>
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const PreparedRamp*>(invocation->prepared_context);
  const auto& program = *state.program;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& result) {
    if (!result.ok())
      *status =
          kernel_failure(result.code, result.message.c_str(), result.reason);
    return result.ok();
  };
  if (!accept(consume(0)))
    return;
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "ramp numeric environment");
    return;
  }
  using Arithmetic = RampArithmetic<rgb>;
  static_assert(alignof(Arithmetic) <= alignof(std::max_align_t));
  auto* arithmetic =
      new (invocation->scratch[0].allocation) Arithmetic(program.profile);
  struct Lifetime {
    Arithmetic* value;
    ~Lifetime() { value->~Arithmetic(); }
  } lifetime{arithmetic};
  fixed_tensor::PointReader<5> reader{state.inputs, *invocation};
  const auto read = [&](unsigned port, const std::array<std::uint64_t, 8>& at) {
    auto bits = reader.bits(port, at, state.input_narrow[port] ? 4 : 8);
    if (state.input_narrow[port]) {
      auto raw = static_cast<std::uint32_t>(bits);
      float value;
      std::memcpy(&value, &raw, 4);
      const double wide = value;
      std::memcpy(&bits, &wide, 8);
    }
    return bits;
  };
  const auto key = [](std::uint64_t bits) {
    return BinaryParts::decode(bits, false).order_key();
  };
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  std::uint64_t points = 1;
  for (unsigned axis = 0; axis + 1 < layout.rank; ++axis)
    points *= layout.shape[axis];
  const unsigned width = state.narrow ? 4 : 8;
  for (std::uint64_t point = 0; point < points; ++point) {
    if (!accept(consume(1)))
      return;
    auto index = point;
    std::array<std::uint64_t, 8> at{};
    for (unsigned axis = layout.rank - 1; axis-- > 0;) {
      at[axis] = layout.origin[axis] + index % layout.shape[axis];
      index /= layout.shape[axis];
    }
    const auto query = read(0, at), wanted = key(query);
    unsigned lo = 0, hi = program.knots;
    while (lo < hi) {
      if (!accept(consume(1)))
        return;
      const auto middle = lo + (hi - lo) / 2;
      if (key(read(1, {middle})) < wanted)
        lo = middle + 1;
      else
        hi = middle;
    }
    unsigned first = 0, count = 1;
    if (lo < program.knots && key(read(1, {lo})) == wanted) {
      first = lo;
    } else if (!lo || lo == program.knots) {
      first = lo ? program.knots - 1 : 0;
    } else {
      first = lo - 1;
      count = 2;
    }
    // Reject's closed domain is a caller precondition. The endpoint path
    // implements Clamp; indices remain bounded independently of sample
    // legality.
    std::array<std::array<std::uint64_t, 4>, 2> rows{};
    std::array<std::int64_t, 2> numerator{}, denominator{};
    const unsigned source_channels = program.rational ? 2 : program.channels;
    for (unsigned row = 0; row < count; ++row) {
      for (unsigned channel = 0; channel < source_channels; ++channel)
        rows[row][channel] = read(2, {first + row, channel});
      if (program.rational) {
        const auto p = read(3, {first + row}), q = read(4, {first + row});
        std::memcpy(&numerator[row], &p, 8);
        std::memcpy(&denominator[row], &q, 8);
      }
    }
    const bool direct =
        count == 1 || (rows[0] == rows[1] && numerator[0] == numerator[1] &&
                       denominator[0] == denominator[1]);
    const std::array<std::uint64_t, 2> knots{read(1, {first}),
                                             read(1, {first + count - 1})};
    std::array<std::uint64_t, 4> result{};
    if constexpr (rgb) {
      auto value =
          direct || program.linear || program.square
              ? arithmetic->algebraic(knots, query, rows, program.with_alpha,
                                      direct, program.square, state.narrow,
                                      consume)
              : arithmetic->nonlinear(knots, query, rows, program.with_alpha,
                                      program.srgb, program.gamma, state.narrow,
                                      consume);
      if (!accept(value.status()))
        return;
      result = value.value();
    } else {
      const unsigned hue_channel = program.description.model == "hsl" ? 0 : 2;
      for (unsigned channel = 0; channel < program.channels; ++channel) {
        const bool hue = program.input_hue && channel == hue_channel;
        const bool rational = hue && program.rational;
        unsigned source = channel;
        if (program.rational && !hue && hue_channel == 0)
          --source;
        std::array<std::uint64_t, 2> values{};
        if (!rational)
          values = {rows[0][source], rows[1][source]};
        int pi_power = 0;
        if (hue && program.input_hue == numeric::HueUnit::Radian &&
            program.output_hue == numeric::HueUnit::PiMultiple)
          pi_power = -1;
        else if (hue && program.input_hue != numeric::HueUnit::Radian &&
                 program.output_hue == numeric::HueUnit::Radian)
          pi_power = 1;
        auto value = arithmetic->evaluate(knots, query, values, numerator,
                                          denominator, rational, direct,
                                          pi_power, state.narrow, consume);
        if (!accept(value.status()))
          return;
        result[channel] = value.value();
      }
    }
    for (unsigned channel = 0; channel < program.channels; ++channel)
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                      output.byte_offset +
                      (point * program.channels + channel) * width,
                  &result[channel], width);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid ramp signature"});
  };
  const auto* state = static_cast<const RampProgram*>(query.static_state);
  if (!state || query.inputs.size() != (state->rational ? 5U : 3U) ||
      query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() || !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema) ||
      query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(*state), 0, 0})
    return invalid();
  auto output = TileGrid::from_schema(*state->output_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  const auto& write = query.writes[0];
  auto region = output.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          state->output_schema->tensors[0].descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto prepared = std::make_unique<PreparedRamp>();
  prepared->program = state;
  prepared->narrow = write.layout.element_type == ElementType::Float32;
  const auto regions = input_regions(*state, region.value());
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  ResourceBudget budget(limits);
  std::size_t slot = 0;
  for (unsigned port = 0; port < query.inputs.size(); ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    prepared->inputs.grids[port] = grid.take_value();
    prepared->input_narrow[port] =
        query.inputs[port].result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
    auto tiles = required_tiles(regions[port], prepared->inputs.grids[port],
                                budget, query.reads.size() - slot);
    if (!tiles.ok())
      return Answer(tiles.status());
    for (auto index : tiles.value()) {
      auto charged = budget.consume({1});
      if (!charged.ok())
        return Answer(charged);
      const auto& read = query.reads[slot];
      auto expected = prepared->inputs.grids[port].region(index);
      if (!expected.ok())
        return Answer(expected.status());
      if (read.port != port || read.member || read.tile_index != index ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != query.inputs[port]
                                          .result_schema->tensors[0]
                                          .descriptor.element_type ||
          !fixed_tensor::matches_region(read.layout, expected.value()))
        return invalid();
      prepared->inputs.slots[port].push_back(
          {index, static_cast<std::uint32_t>(slot++)});
    }
  }
  if (slot != query.reads.size())
    return invalid();
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu =
      state->description.model == "rgb" ? invoke<true> : invoke<false>;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedRamp*>(context);
  };
  return Answer(kernel);
}

OperationDefinition operation(const std::string& name, const std::string& model,
                              std::optional<numeric::HueUnit> hue,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = name;
  auto& traits = result.traits;
  traits.input_count = hue == numeric::HueUnit::RationalPi ? 5 : 3;
  traits.input_schema.resize(traits.input_count);
  for (unsigned port = 0; port < traits.input_count; ++port) {
    traits.input_schema[port].element_type_mask = port < 3 ? 12 : 2;
  }
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"color_description", OperationParameterType::String},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  if (hue)
    traits.parameter_schema.push_back(
        {"output_hue_unit", OperationParameterType::String});
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  result.prepare_static = [model, hue, profile](const auto& inputs,
                                                const auto& params) {
    return prepare(model, hue, profile, inputs, params);
  };
  result.lower = lower;
  if (model == "cmyk") {
    result.validate_resources = [](const void* value,
                                   const ResourceSet& resources) -> Status {
      const auto& state = *static_cast<const RampProgram*>(value);
      auto selected = resources.find(*state.description.profile);
      if (!selected.ok())
        return selected.status();
      const auto bytes = selected.value().bytes();
      if (bytes.size() < 132 || std::memcmp(bytes.data() + 12, "prtrCMYK", 8))
        return numeric_ops::array_parameter_error(
            "CMYK ramp requires an ICC output-device profile");
      ResourceLimits limits;
      limits.maximum_work = 64 * 1024 * 1024;
      ResourceBudget budget(limits);
      const auto consume = [&budget](std::uint64_t amount) {
        return budget.consume({amount});
      };
      return color_internal::validate_icc_resource(bytes, budget, consume);
    };
  }
  result.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "color-ramp" || kernel.contract != ramp_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid ramp kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "ramp requires CPU Host"});
    return prepare_kernel(query);
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
    for (const auto* model :
         {"rgb", "xyz", "cmyk", "cielab", "oklab", "ycbcr"}) {
      auto status = registry->register_operation(
          operation(std::string("curve.color_ramp_") + model + profile.first,
                    model, {}, profile.second));
      if (!status.ok())
        return status;
    }
    for (const auto* model : {"cielch", "oklch", "hsl"}) {
      for (const auto& unit :
           {std::make_pair("", numeric::HueUnit::Radian),
            std::make_pair("_pi", numeric::HueUnit::PiMultiple),
            std::make_pair("_rational_pi", numeric::HueUnit::RationalPi)}) {
        auto status = registry->register_operation(
            operation(std::string("curve.color_ramp_") + model + unit.first +
                          profile.first,
                      model, unit.second, profile.second));
        if (!status.ok())
          return status;
      }
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
