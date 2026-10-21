#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_aggregate.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct ScanMetadata {
  unsigned mask = 0, axis = 0;
  TensorDescriptor output;
};
Result<ScanMetadata> metadata(
    bool integral, const ResultTensorSpec& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ScanMetadata>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto shape = input.sample_shape();
  if (shape.size() < (integral ? 2U : 1U) || shape.size() > 8)
    return mismatch("scan requires rank 1..8 (integral rank >=2)");
  ScanMetadata result;
  result.output = {input.descriptor.element_type, shape};
  std::vector<std::uint64_t> axes;
  if (integral) {
    auto parsed = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("axes")), false);
    if (!parsed.ok())
      return Answer(parsed.status());
    axes = parsed.take_value();
    if (axes.size() != 2)
      return Answer(numeric_ops::array_parameter_error("require two axes"));
  } else {
    const auto axis = std::get<std::int64_t>(parameters.at("axis"));
    if (axis < 0)
      return Answer(numeric_ops::array_parameter_error("negative scan axis"));
    axes.push_back(static_cast<std::uint64_t>(axis));
  }
  for (auto axis : axes) {
    if (axis >= shape.size() || (result.mask & (1U << axis)))
      return Answer(numeric_ops::array_parameter_error("invalid scan axes"));
    result.mask |= 1U << axis;
    result.axis = static_cast<unsigned>(axis);
  }
  std::uint64_t source_count = 1, output_count = 1;
  for (std::size_t j = 0; j < shape.size(); ++j) {
    if (!shape[j] || shape[j] > (UINT64_C(1) << 40) / source_count)
      return mismatch("scan input exceeds 2^40 elements");
    source_count *= shape[j];
    result.output.shape[j] += (result.mask >> j) & 1;
    if (result.output.shape[j] > (UINT64_C(1) << 40) / output_count)
      return mismatch("scan output exceeds 2^40 elements");
    output_count *= result.output.shape[j];
  }
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  if (dtype == "uint8")
    result.output.element_type = ElementType::UInt8;
  else if (dtype == "int64")
    result.output.element_type = ElementType::Int64;
  else if (dtype == "float32")
    result.output.element_type = ElementType::Float32;
  else if (dtype == "float64")
    result.output.element_type = ElementType::Float64;
  else
    return Answer(numeric_ops::array_parameter_error("unsupported scan dtype"));
  const auto floating = [](ElementType type) {
    return type == ElementType::Float32 || type == ElementType::Float64;
  };
  if (floating(input.descriptor.element_type) !=
      floating(result.output.element_type))
    return mismatch("scan source/destination numeric domains differ");
  return Answer(std::move(result));
}
// Store only exact carry, never a rounded output or a complete ratio workspace.
// Plane traversal is logical row-major: previous rows precede the current row,
// preserving first-NaN priority while exact finite sums are merged.
struct SumCarry final {
  numeric_ops::RatioWorkspace::Integer magnitude;
  std::uint64_t first_nan = 0;
  bool negative = false, has_nan = false, positive_inf = false,
       negative_inf = false, all_negative_zero = true;
  void load(numeric_ops::ExactAggregate* target) const {
    target->reset();
    target->ratio.numerator = magnitude;
    target->ratio.negative = negative;
    target->first_nan = first_nan;
    target->has_nan = has_nan;
    target->positive_inf = positive_inf;
    target->negative_inf = negative_inf;
    target->all_negative_zero = all_negative_zero;
  }
  void save(const numeric_ops::ExactAggregate& source) {
    magnitude = source.ratio.numerator;
    negative = source.ratio.negative;
    first_nan = source.first_nan;
    has_nan = source.has_nan;
    positive_inf = source.positive_inf;
    negative_inf = source.negative_inf;
    all_negative_zero = source.all_negative_zero;
  }
};
struct ScanState final {
  numeric_ops::ExactAggregate accumulator, conversion;
  ScanState(SequenceProfile profile, ElementType source)
      : accumulator(profile, numeric_ops::AggregateKind::Sum, source),
        conversion(profile, numeric_ops::AggregateKind::Sum, source) {}
};
using GroupKey = std::array<std::uint64_t, 16>;
GroupKey group_key(const Region& region, unsigned mask) {
  GroupKey key{};
  for (unsigned axis = 0; axis < region.rank(); ++axis)
    if (!(mask & (1U << axis))) {
      key[axis] = region.dimensions()[axis].offset;
      key[8 + axis] = region.dimensions()[axis].extent;
    }
  return key;
}
struct ScanGroup final {
  std::vector<TileIndex> tiles;
  std::vector<RegionDimension> source;
  void include(const Region& output, unsigned mask) {
    if (source.empty()) {
      source = output.dimensions();
      for (unsigned axis = 0; axis < source.size(); ++axis)
        if (mask & (1U << axis))
          source[axis] = {0, 0};
    }
    // A tile touching only zero boundaries requires no source samples. Such
    // tiles also must not enlarge the prefix needed by nonempty rectangles.
    for (unsigned axis = 0; axis < source.size(); ++axis)
      if ((mask & (1U << axis)) &&
          output.dimensions()[axis].offset + output.dimensions()[axis].extent ==
              1)
        return;
    for (unsigned axis = 0; axis < source.size(); ++axis)
      if (mask & (1U << axis))
        source[axis].extent = std::max(
            source[axis].extent, output.dimensions()[axis].offset +
                                     output.dimensions()[axis].extent - 1);
  }
};
std::uint64_t scan_scratch(bool integral, const ScanMetadata& description,
                           const Region& source) {
  if (source.empty())
    return 0;
  const auto inner =
      integral ? 31U - static_cast<unsigned>(__builtin_clz(description.mask))
               : description.axis;
  // Metadata caps both logical counts at 2^40; sizeof(SumCarry) fits this
  // checked fixed-width capacity without multiplying by output coordinates.
  static_assert(sizeof(SumCarry) < (UINT64_C(1) << 20));
  static_assert(sizeof(ScanState) % alignof(SumCarry) == 0);
  return sizeof(ScanState) +
         (integral ? source.dimensions()[inner].extent * sizeof(SumCarry) : 0);
}
struct PreparedScan final {
  bool integral = false;
  SequenceProfile profile;
  ScanMetadata description;
  ElementType source_type = ElementType::UInt8;
  Region source;
  TileGrid output;
  fixed_tensor::PointInputs<1> inputs;
  std::vector<std::pair<TileIndex, std::uint32_t>> writes;
};
struct ScanWriter final {
  const PreparedScan& state;
  const ps_kernel_invocation_v8& invocation;
  TileIndex cached = UINT64_MAX;
  const ps_kernel_mutable_view_v8* view = nullptr;
  std::uint8_t* address(const std::array<std::uint64_t, 8>& at) {
    auto tile = fixed_tensor::input_tile(state.output, at);
    if (tile != cached) {
      cached = tile;
      auto found = std::lower_bound(
          state.writes.begin(), state.writes.end(), tile,
          [](const auto& item, TileIndex id) { return item.first < id; });
      view = found != state.writes.end() && found->first == tile
                 ? &invocation.outputs[found->second]
                 : nullptr;
    }
    if (!view)
      return nullptr;
    std::uint64_t offset = view->byte_offset;
    for (unsigned axis = 0; axis < view->layout->rank; ++axis)
      offset +=
          (at[axis] - view->layout->origin[axis]) * view->layout->strides[axis];
    return static_cast<std::uint8_t*>(view->allocation) + offset;
  }
};
void scan_invoke(const ps_kernel_invocation_v8* invocation,
                 ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& prepared =
        *static_cast<const PreparedScan*>(invocation->prepared_context);
    const auto& description = prepared.description;
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    // Initialize selected zero boundaries only in owned output tiles. Positive
    // prefixes overwrite their locations after exact accumulation/conversion.
    for (unsigned slot = 0; slot < invocation->output_count; ++slot) {
      const auto& output = invocation->outputs[slot];
      auto bytes = TensorSpan::element_size(description.output.element_type);
      std::uint64_t size = bytes;
      for (unsigned axis = 0; axis < output.layout->rank; ++axis)
        size *= output.layout->shape[axis];
      for (std::uint64_t offset = 0; offset < size; offset += 65536) {
        auto stopped = consume(1);
        if (!stopped.ok()) {
          *status = kernel_failure(stopped.code);
          return;
        }
        std::memset(
            static_cast<std::uint8_t*>(output.allocation) + output.byte_offset +
                offset,
            0,
            static_cast<std::size_t>(std::min(UINT64_C(65536), size - offset)));
      }
    }
    if (prepared.source.empty()) {
      *status = {};
      return;
    }
    static_assert(alignof(ScanState) <= alignof(std::max_align_t));
    auto* state = new (invocation->scratch[0].allocation)
        ScanState(prepared.profile, prepared.source_type);
    const auto destroy = [](ScanState* value) { value->~ScanState(); };
    std::unique_ptr<ScanState, decltype(destroy)> guard(state, destroy);
    const auto outer = static_cast<unsigned>(__builtin_ctz(description.mask));
    const auto inner =
        prepared.integral
            ? 31U - static_cast<unsigned>(__builtin_clz(description.mask))
            : outer;
    const auto& dimensions = prepared.source.dimensions();
    const auto height = dimensions[outer].extent,
               width = dimensions[inner].extent;
    static_assert(std::is_trivially_destructible<SumCarry>::value);
    auto* columns =
        static_cast<std::uint8_t*>(invocation->scratch[0].allocation) +
        sizeof(ScanState);
    const auto column = [columns](std::uint64_t index) -> SumCarry& {
      return *std::launder(
          reinterpret_cast<SumCarry*>(columns + index * sizeof(SumCarry)));
    };
    if (prepared.integral) {
      for (std::uint64_t x = 0; x < width; ++x) {
        auto stopped = consume(1);
        if (!stopped.ok()) {
          *status = kernel_failure(stopped.code);
          return;
        }
        new (columns + x * sizeof(SumCarry)) SumCarry{};
      }
    }
    fixed_tensor::PointReader<1> reader{prepared.inputs, *invocation};
    ScanWriter writer{prepared, *invocation};
    const auto in_width = TensorSpan::element_size(prepared.source_type);
    const auto out_width =
        TensorSpan::element_size(description.output.element_type);
    std::array<std::uint64_t, 8> at{};
    std::uint64_t planes = 1;
    for (unsigned axis = 0; axis < dimensions.size(); ++axis) {
      at[axis] = dimensions[axis].offset;
      if (!(description.mask & (1U << axis)))
        planes *= dimensions[axis].extent;
    }
    const auto store = [&]() {
      auto* output = writer.address(at);
      if (!output)
        return Status::success();
      auto value = state->conversion.finish_as(description.output.element_type,
                                               1, consume);
      if (!value.ok())
        return value.status();
      auto bits = value.value();
      std::memcpy(output, &bits, out_width);
      return Status::success();
    };
    const auto require = [](Status result) {
      if (!result.ok())
        throw result;
    };
    for (std::uint64_t plane = 0; plane < planes; ++plane) {
      if (!prepared.integral) {
        state->accumulator.reset();
        for (std::uint64_t i = 0; i < height; ++i) {
          require(consume(1));
          at[outer] = i;
          require(
              state->accumulator.add(reader.bits(0, at, in_width), consume));
          at[outer] = i + 1;
          if (writer.address(at)) {
            state->conversion = state->accumulator;
            require(store());
          }
        }
      } else {
        for (std::uint64_t x = 0; x < width; ++x) {
          require(consume(1));
          column(x) = SumCarry{};
        }
        for (std::uint64_t y = 0; y < height; ++y) {
          state->accumulator.reset();
          for (std::uint64_t x = 0; x < width; ++x) {
            require(consume(1));
            at[outer] = y;
            at[inner] = x;
            require(
                state->accumulator.add(reader.bits(0, at, in_width), consume));
            auto& row = state->accumulator;
            auto& combined = state->conversion;
            column(x).load(&combined);
            combined.ratio.term = row.ratio.numerator;
            combined.ratio.add_term(row.ratio.negative);
            if (!combined.has_nan && row.has_nan) {
              combined.first_nan = row.first_nan;
              combined.has_nan = true;
            }
            combined.positive_inf |= row.positive_inf;
            combined.negative_inf |= row.negative_inf;
            combined.all_negative_zero &= row.all_negative_zero;
            column(x).save(combined);
            at[outer] = y + 1;
            at[inner] = x + 1;
            require(store());
          }
        }
      }
      for (unsigned axis = 0; axis < dimensions.size(); ++axis)
        if (description.mask & (1U << axis))
          at[axis] = 0;
      for (unsigned axis = static_cast<unsigned>(dimensions.size());
           axis-- > 0;)
        if (!(description.mask & (1U << axis))) {
          if (++at[axis] < dimensions[axis].offset + dimensions[axis].extent)
            break;
          at[axis] = dimensions[axis].offset;
        }
    }
    *status = {};
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "scan arithmetic failed");
  }
}
Result<OperationProgram> lower_scan(bool integral,
                                    const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(integral, query.inputs[0].result_schema->tensors[0],
                         query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  const auto& description = parsed.value();
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  if (!input.ok())
    return Answer(input.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  std::map<GroupKey, ScanGroup> groups;
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto charged = budget.consume({1});
      if (!charged.ok())
        return Answer(charged);
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      auto& group = groups[group_key(region.value(), description.mask)];
      group.tiles.push_back(tile);
      group.include(region.value(), description.mask);
    }
  OperationProgramBuilder builder(query);
  for (const auto& item : groups) {
    const auto& group = item.second;
    Region source(group.source);
    auto task = builder.add_task(
        integral ? "integral" : "prefix", "exact-scan-unchecked-v1", {},
        {scan_scratch(integral, description, source), 0, 0});
    if (!task.ok())
      return Answer(task.status());
    auto status = builder.read_input_schema(task.value(), 0);
    if (!status.ok())
      return Answer(status);
    auto reads = sdk::tiles_for_region(budget, input.value(), source);
    if (!reads.ok())
      return Answer(reads.status());
    for (auto tile : reads.value()) {
      status = budget.consume({1});
      if (status.ok())
        status = builder.read_input(task.value(), 0, 0, tile);
      if (!status.ok())
        return Answer(status);
    }
    for (auto tile : group.tiles) {
      status = builder.write_output(task.value(), 0, 0, tile);
      if (!status.ok())
        return Answer(status);
    }
  }
  return builder.finish();
}
Result<ps_prepared_kernel_v8> prepare_scan(bool integral,
                                           SequenceProfile profile,
                                           const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid scan invocation"});
  };
  if (query.inputs.size() != 1 || query.outputs.size() != 1 ||
      query.writes.empty())
    return invalid();
  auto parsed = metadata(integral, query.inputs[0].result_schema->tensors[0],
                         query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto state = std::make_unique<PreparedScan>();
  state->integral = integral;
  state->profile = profile;
  state->description = parsed.take_value();
  const auto& target = query.outputs[0].result_schema->tensors[0];
  if (target.sample_shape() != state->description.output.shape ||
      target.descriptor.element_type != state->description.output.element_type)
    return invalid();
  state->source_type =
      query.inputs[0].result_schema->tensors[0].descriptor.element_type;
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  state->output = output.take_value();
  ScanGroup group;
  GroupKey key{};
  for (std::size_t slot = 0; slot < query.writes.size(); ++slot) {
    const auto& write = query.writes[slot];
    auto region = state->output.region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    auto current = group_key(region.value(), state->description.mask);
    if (slot == 0)
      key = current;
    if (write.port || write.member || write.domain != physical::Domain::Host ||
        write.layout.element_type != state->description.output.element_type ||
        !fixed_tensor::matches_region(write.layout, region.value()) ||
        current != key ||
        (slot && query.writes[slot - 1].tile_index >= write.tile_index))
      return invalid();
    group.include(region.value(), state->description.mask);
    state->writes.push_back(
        {write.tile_index, static_cast<std::uint32_t>(slot)});
  }
  state->source = Region(std::move(group.source));
  if (!query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{
              scan_scratch(integral, state->description, state->source), 0, 0})
    return invalid();
  auto valid = fixed_tensor::prepare_input_regions(
      query, &state->inputs, std::array<Region, 1>{state->source});
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.context = state.release();
  result.invoke_cpu = scan_invoke;
  result.release = [](const void* context) {
    delete static_cast<const PreparedScan*>(context);
  };
  return Answer(result);
}
OperationDefinition scan_operation(const std::string& key, bool integral,
                                   SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);

  traits.input_schema[0].element_type_mask = 15;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {integral ? "axes" : "axis", integral ? OperationParameterType::String
                                            : OperationParameterType::Int64},
      {"dtype", OperationParameterType::String}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [integral, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto valid = tensor_ops::check_tensor(inputs[0]);
    if (!valid.ok())
      return Answer(valid);
    auto resolved =
        metadata(integral, inputs[0].result_schema->tensors[0], parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = resolved.value().output;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [integral](const OperationLowering& query) {
    return lower_scan(integral, query);
  };
  operation.prepare_kernel =
      [integral, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != (integral ? "integral" : "prefix") ||
        kernel.contract != "exact-scan-unchecked-v1")
      return Answer({ErrorCode::TypeMismatch, "invalid scan kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer(
          {ErrorCode::BackendUnavailable, "scan requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    return prepare_scan(integral, profile, query);
  };
  return operation;
}
}  // namespace
Status register_numeric_scans(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool integral : {false, true}) {
      auto status = registry->register_operation(scan_operation(
          std::string("numeric.") +
              (integral ? "integral_image" : "prefix_sum") + profile.first,
          integral, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
