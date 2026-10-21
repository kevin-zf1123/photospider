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
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/lowpass_uniform_math.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::LowpassKernel;
using numeric_ops::LowpassParameters;
using numeric_ops::SequenceProfile;
std::uint64_t raw(double value) {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, 8);
  return result;
}
LowpassParameters parameters(
    LowpassKernel kernel, const std::map<std::string, ParameterValue>& input) {
  LowpassParameters result{kernel};
  result.radius =
      raw(static_cast<double>(std::get<std::int64_t>(input.at("radius"))));
  if (kernel == LowpassKernel::Gaussian)
    result.sigma = raw(std::get<double>(input.at("sigma")));
  else
    result.cutoff = raw(std::get<double>(input.at("cutoff")));
  if (kernel == LowpassKernel::Kaiser)
    result.beta = raw(std::get<double>(input.at("beta")));
  return result;
}
enum class Boundary { Reflect, Replicate, Zero, Wrap };
struct UniformPrepared final {
  LowpassParameters parameters;
  unsigned axis, radius;
  Boundary boundary;
  SequenceProfile profile;
  std::shared_ptr<const SchemaTemplate> output_schema;
};
std::optional<std::uint64_t> mapped(Boundary boundary, std::uint64_t center,
                                    int offset, std::uint64_t size) {
  const auto at = static_cast<std::int64_t>(center) + offset;
  if (at >= 0 && static_cast<std::uint64_t>(at) < size)
    return static_cast<std::uint64_t>(at);
  if (boundary == Boundary::Zero)
    return {};
  if (boundary == Boundary::Replicate)
    return at < 0 ? 0 : size - 1;
  if (size == 1)
    return 0;
  const auto period = static_cast<std::int64_t>(
      boundary == Boundary::Wrap ? size : 2 * (size - 1));
  auto reduced = at % period;
  if (reduced < 0)
    reduced += period;
  return boundary == Boundary::Reflect &&
                 static_cast<std::uint64_t>(reduced) >= size
             ? period - reduced
             : reduced;
}
struct UniformScratch final {
  numeric_ops::UniformLowpassMath arithmetic;
  numeric_ops::FastInterval normalizer;
};
std::uint64_t scratch_bytes(const UniformPrepared& state) {
  static_assert(sizeof(UniformScratch) % alignof(std::uint64_t) == 0);
  static_assert(alignof(numeric_ops::FastInterval) <= alignof(std::uint64_t));
  return sizeof(UniformScratch) +
         (2 * state.radius + 1) * sizeof(std::uint64_t) +
         (state.profile == SequenceProfile::Strict
              ? 0
              : (state.radius + 1) * sizeof(numeric_ops::FastInterval));
}
// Map complete integer intervals, then close and merge them on the source
// grid before tile enumeration. Atomic source axes collapse to one interval.
Result<std::vector<Region>> source_regions(const UniformPrepared& state,
                                           const Region& output,
                                           const TileGrid& grid,
                                           const ResourceBudget& budget) {
  using Answer = Result<std::vector<Region>>;
  using Range = std::pair<std::uint64_t, std::uint64_t>;
  std::vector<Range> ranges;
  const auto size = grid.shape()[state.axis];
  const auto dimension = output.dimensions()[state.axis];
  const auto mod = [](std::int64_t n, std::int64_t period) {
    const auto r = n % period;
    return r < 0 ? r + period : r;
  };
  const auto add = [&](std::uint64_t begin, std::uint64_t end) {
    if (begin >= end)
      return;
    if (state.axis == grid.width_axis() || state.axis == grid.height_axis()) {
      begin = begin / grid.tile_extent() * grid.tile_extent();
      end = std::min(size,
                     ((end - 1) / grid.tile_extent() + 1) * grid.tile_extent());
    } else if (!(grid.batch_axis_mask() & (1U << state.axis))) {
      begin = 0;
      end = size;
    }
    ranges.emplace_back(begin, end);
  };
  for (int offset = -static_cast<int>(state.radius);
       offset <= static_cast<int>(state.radius); ++offset) {
    auto status = budget.consume({1});
    if (!status.ok())
      return Answer(status);
    if (!numeric_ops::uniform_tap_sign(state.parameters, state.radius,
                                       offset < 0 ? -offset : offset))
      continue;
    const auto lo = static_cast<std::int64_t>(dimension.offset) + offset;
    const auto hi = lo + static_cast<std::int64_t>(dimension.extent) - 1;
    const auto length = hi - lo + 1;
    if (state.boundary == Boundary::Zero) {
      if (hi >= 0 && lo < static_cast<std::int64_t>(size))
        add(std::max<std::int64_t>(0, lo),
            std::min<std::int64_t>(size - 1, hi) + 1);
    } else if (state.boundary == Boundary::Replicate) {
      add(std::clamp<std::int64_t>(lo, 0, size - 1),
          std::clamp<std::int64_t>(hi, 0, size - 1) + 1);
    } else if (size == 1) {
      add(0, 1);
    } else if (state.boundary == Boundary::Wrap) {
      if (static_cast<std::uint64_t>(length) >= size) {
        add(0, size);
      } else {
        const auto begin = static_cast<std::uint64_t>(mod(lo, size));
        add(begin, std::min(size, begin + length));
        if (begin + length > size)
          add(0, begin + length - size);
      }
    } else {
      const auto period = static_cast<std::int64_t>(2 * (size - 1));
      if (length >= period) {
        add(0, size);
      } else {
        const auto reflect = [&](std::int64_t x) {
          const auto r = mod(x, period);
          return static_cast<std::uint64_t>(
              r < static_cast<std::int64_t>(size) ? r : period - r);
        };
        auto begin = std::min(reflect(lo), reflect(hi));
        auto end = std::max(reflect(lo), reflect(hi));
        if (lo + mod(-lo, period) <= hi)
          begin = 0;
        if (lo + mod(static_cast<std::int64_t>(size - 1) - lo, period) <= hi)
          end = size - 1;
        add(begin, end + 1);
      }
    }
  }
  std::sort(ranges.begin(), ranges.end());
  std::vector<Range> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.first <= merged.back().second)
      merged.back().second = std::max(merged.back().second, range.second);
    else
      merged.push_back(range);
  }
  std::vector<Region> regions;
  for (const auto& range : merged) {
    auto dimensions = output.dimensions();
    dimensions[state.axis] = {range.first, range.second - range.first};
    regions.emplace_back(std::move(dimensions));
  }
  return Answer(std::move(regions));
}
Result<TileSet> required_tiles(const UniformPrepared& state,
                               const Region& output, const TileGrid& grid,
                               const ResourceBudget& budget,
                               std::uint64_t maximum) {
  using Answer = Result<TileSet>;
  auto regions = source_regions(state, output, grid, budget);
  if (!regions.ok())
    return Answer(regions.status());
  std::uint64_t total = 0;
  for (const auto& region : regions.value()) {
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < grid.rank(); ++axis) {
      const auto d = region.dimensions()[axis];
      std::uint64_t extent = 1;
      if (grid.batch_axis_mask() & (1U << axis))
        extent = d.extent;
      else if (axis == grid.width_axis() || axis == grid.height_axis())
        extent = (d.offset + d.extent - 1) / grid.tile_extent() -
                 d.offset / grid.tile_extent() + 1;
      if (extent > (maximum - total) / count)
        return Answer(
            {ErrorCode::ResourceExhausted, "uniform read tile limit"});
      count *= extent;
    }
    total += count;
  }
  std::vector<TileIndex> indices;
  indices.reserve(total);
  for (const auto& region : regions.value()) {
    auto tiles = sdk::tiles_for_region(budget, grid, region);
    if (!tiles.ok())
      return Answer(tiles.status());
    for (auto tile : tiles.value())
      indices.push_back(tile);
  }
  return TileSet::from_indices(budget, std::move(indices));
}
constexpr const char* uniform_contract = "uniform-lowpass-unchecked-v2";
Result<OperationProgram> lower(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto& state = *static_cast<const UniformPrepared*>(query.static_state);
  auto output = TileGrid::from_schema(*state.output_schema, query.tile_extent);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  if (!input.ok())
    return Answer(input.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto index : member.second) {
      auto region = output.value().region(index);
      if (!region.ok())
        return Answer(region.status());
      auto tiles = required_tiles(state, region.value(), input.value(), budget,
                                  query.limits.maximum_records);
      if (!tiles.ok())
        return Answer(tiles.status());
      auto task = builder.add_task("uniform-lowpass", uniform_contract, {},
                                   {scratch_bytes(state), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      for (auto tile : tiles.value()) {
        auto status = budget.consume({1});
        if (status.ok())
          status = builder.read_input(task.value(), 0, 0, tile);
        if (!status.ok())
          return Answer(status);
      }
      auto status = builder.write_output(task.value(), 0, member.first, index);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct UniformKernel final {
  const UniformPrepared* program;
  fixed_tensor::PointInputs<1> input;
  bool narrow;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const UniformKernel*>(invocation->prepared_context);
  const auto& program = *state.program;
  const auto consume = [invocation](std::uint64_t) {
    return invocation->cancellation.is_cancelled &&
                   invocation->cancellation.is_cancelled(
                       invocation->cancellation.context)
               ? Status{ErrorCode::Cancelled, {}}
               : Status::success();
  };
  const auto accept = [status](const Status& value) {
    if (!value.ok())
      *status = kernel_failure(value.code, value.message.c_str(), value.reason);
    return value.ok();
  };
  if (!accept(consume(0)))
    return;
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "uniform numeric environment");
    return;
  }
  auto* work = new (invocation->scratch[0].allocation) UniformScratch;
  struct Lifetime {
    UniformScratch* value;
    ~Lifetime() { value->~UniformScratch(); }
  } lifetime{work};
  auto* samples = reinterpret_cast<std::uint64_t*>(work + 1);
  for (unsigned i = 0; i < 2 * program.radius + 1; ++i)
    new (samples + i) std::uint64_t;
  auto* coefficients = reinterpret_cast<numeric_ops::FastInterval*>(
      samples + 2 * program.radius + 1);
  bool ready = false;
  if (program.profile != SequenceProfile::Strict) {
    for (unsigned i = 0; i <= program.radius; ++i)
      new (coefficients + i) numeric_ops::FastInterval{};
    auto result =
        work->arithmetic.prepare(program.parameters, program.radius,
                                 coefficients, &work->normalizer, consume);
    if (!accept(result.status()))
      return;
    ready = result.value();
  }
  fixed_tensor::PointReader<1> reader{state.input, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  const unsigned width = state.narrow ? 4 : 8;
  const auto size = state.input.grids[0].shape()[program.axis];
  for (std::uint64_t point = 0; point < count; ++point) {
    std::array<std::uint64_t, 8> at{};
    auto index = point;
    for (unsigned axis = layout.rank; axis-- > 0;) {
      at[axis] = layout.origin[axis] + index % layout.shape[axis];
      index /= layout.shape[axis];
    }
    const auto center = at[program.axis];
    for (int offset = -static_cast<int>(program.radius);
         offset <= static_cast<int>(program.radius); ++offset) {
      if (!accept(consume(1)))
        return;
      auto& bits = samples[offset + program.radius];
      bits = 0;
      if (!numeric_ops::uniform_tap_sign(program.parameters, program.radius,
                                         offset < 0 ? -offset : offset))
        continue;
      const auto source = mapped(program.boundary, center, offset, size);
      if (source) {
        at[program.axis] = *source;
        bits = reader.bits(0, at, width);
      }
    }
    std::optional<std::uint64_t> fast;
    if (ready) {
      if (!accept(consume(1)))
        return;
      auto accelerated = work->arithmetic.fast(
          program.parameters, program.radius, samples, state.narrow,
          coefficients, work->normalizer, consume, true);
      if (!accept(accelerated.status()))
        return;
      fast = accelerated.value();
    }
    auto value =
        fast ? Result<std::uint64_t>(*fast)
             : work->arithmetic.evaluate(program.parameters, program.radius,
                                         samples, state.narrow, consume);
    if (!accept(value.status()) || !accept(consume(1)))
      return;
    const auto bits = value.value();
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + point * width,
                &bits, width);
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(
        {ErrorCode::TypeMismatch, "invalid uniform lowpass signature"});
  };
  const auto* state = static_cast<const UniformPrepared*>(query.static_state);
  if (!state || query.inputs.size() != 1 || query.outputs.size() != 1 ||
      query.writes.size() != 1 || !query.task_parameters.empty() ||
      !query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(*state->output_schema) ||
      query.scratch !=
          std::array<std::uint64_t, 3>{scratch_bytes(*state), 0, 0})
    return invalid();
  auto output = TileGrid::from_schema(*state->output_schema, query.tile_extent);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  if (!input.ok())
    return Answer(input.status());
  const auto& write = query.writes[0];
  auto region = output.value().region(write.tile_index);
  if (!region.ok())
    return Answer(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type !=
          state->output_schema->tensors[0].descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  ResourceLimits limits;
  limits.maximum_work = physical::ValidationLimits{}.maximum_work;
  auto tiles = required_tiles(*state, region.value(), input.value(),
                              ResourceBudget(limits), query.reads.size());
  if (!tiles.ok())
    return Answer(tiles.status());
  if (tiles.value().size() != query.reads.size())
    return invalid();
  auto prepared = std::make_unique<UniformKernel>();
  prepared->program = state;
  prepared->input.grids[0] = input.take_value();
  prepared->narrow = write.layout.element_type == ElementType::Float32;
  std::size_t slot = 0;
  for (auto index : tiles.value()) {
    const auto& read = query.reads[slot];
    auto expected = prepared->input.grids[0].region(index);
    if (!expected.ok())
      return Answer(expected.status());
    if (read.port || read.member || read.tile_index != index ||
        read.domain != physical::Domain::Host ||
        read.layout.element_type != write.layout.element_type ||
        !fixed_tensor::matches_region(read.layout, expected.value()))
      return invalid();
    prepared->input.slots[0].push_back(
        {index, static_cast<std::uint32_t>(slot++)});
  }
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = prepared.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const UniformKernel*>(context);
  };
  return Answer(kernel);
}

OperationDefinition operation(const std::string& name, LowpassKernel kernel,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);

  traits.input_schema[0].element_type_mask = 12;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64},
                             {"radius", OperationParameterType::Int64},
                             {"boundary", OperationParameterType::String}};
  traits.parameter_schema.push_back(
      {kernel == LowpassKernel::Gaussian ? "sigma" : "cutoff",
       OperationParameterType::Float64});
  if (kernel == LowpassKernel::Kaiser)
    traits.parameter_schema.push_back(
        {"beta", OperationParameterType::Float64});
  traits.requires_metadata_specialization = true;
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  definition.prepare_static = [kernel, profile](const auto& inputs,
                                                const auto& p) {
    using Answer = Result<OperationPreparation>;
    if (inputs.size() != 1 || !inputs[0].result_schema ||
        inputs[0].result_schema->tensors.size() != 1 ||
        (inputs[0].result_schema->tensors[0].descriptor.element_type !=
             ElementType::Float32 &&
         inputs[0].result_schema->tensors[0].descriptor.element_type !=
             ElementType::Float64))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "lowpass input requires Float32/64",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    auto valid = tensor_ops::check_tensor(inputs[0]);
    if (!valid.ok())
      return Answer(valid);
    const auto& tensor = inputs[0].result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
    std::uint64_t count = 1;
    if (shape.empty() || shape.size() > 8)
      return Answer(numeric_ops::array_parameter_error("lowpass rank 1..8"));
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(numeric_ops::array_parameter_error(
            "lowpass positive shape within 2^40"));
      count *= extent;
    }
    const auto axis = std::get<std::int64_t>(p.at("axis")),
               radius = std::get<std::int64_t>(p.at("radius"));
    const auto& boundary = std::get<std::string>(p.at("boundary"));
    if (axis < 0 || static_cast<std::uint64_t>(axis) >= shape.size() ||
        radius < 1 || radius > 4096 ||
        (boundary != "reflect" && boundary != "replicate" &&
         boundary != "zero" && boundary != "wrap"))
      return Answer(
          numeric_ops::array_parameter_error("lowpass axis/radius/boundary"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = {tensor.descriptor.element_type, shape};
    resolved.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    OperationPreparation prepared;
    prepared.outputs.push_back(std::move(resolved));
    prepared.state = std::make_shared<const UniformPrepared>(
        UniformPrepared{parameters(kernel, p), static_cast<unsigned>(axis),
                        static_cast<unsigned>(radius),
                        boundary == "reflect"     ? Boundary::Reflect
                        : boundary == "replicate" ? Boundary::Replicate
                        : boundary == "wrap"      ? Boundary::Wrap
                                                  : Boundary::Zero,
                        profile, prepared.outputs[0].metadata.result_schema});
    return Answer(std::move(prepared));
  };
  definition.lower = lower;
  definition.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "uniform-lowpass" ||
        kernel.contract != uniform_contract)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid uniform lowpass kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "uniform lowpass requires CPU Host"});
    return prepare_kernel(query);
  };
  return definition;
}
}  // namespace
Status register_uniform_lowpass(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& kernel :
         {std::make_pair("hann_sinc", LowpassKernel::Hann),
          std::make_pair("hamming_sinc", LowpassKernel::Hamming),
          std::make_pair("blackman_sinc", LowpassKernel::Blackman),
          std::make_pair("kaiser_sinc", LowpassKernel::Kaiser),
          std::make_pair("gaussian", LowpassKernel::Gaussian)}) {
      auto status = registry->register_operation(operation(
          std::string("curve.lowpass_uniform_") + kernel.first + profile.first,
          kernel.second, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
