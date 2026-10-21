#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "common/exact_binary_sum.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using core_internal::ExactBinarySum;
using numeric_ops::SequenceProfile;
enum class Sequence { Linspace, Arange };
struct SequenceMath {
  SequenceProfile profile;
  ExactBinarySum first, second;
  std::array<std::uint64_t, 68> products{};
  explicit SequenceMath(SequenceProfile selected) : profile(selected) {}
  std::uint64_t rounded(double a, double b, std::uint32_t wa, std::uint32_t wb,
                        std::uint32_t divisor, bool binary32,
                        bool subtract = false) {
    first.set(a);
    second.set(b);
    numeric_ops::sequence_multiply(&first, wa, profile, products.data());
    numeric_ops::sequence_multiply(&second, wb, profile, products.data());
    if (subtract)
      second.negative = !second.negative;
    first.add(second);
    const bool zero_sign =
        !subtract && std::signbit(a) && (!wb || std::signbit(b));
    return first.rounded_bits(divisor, binary32, zero_sign);
  }
};
struct SequenceMetadata final {
  std::uint32_t count = 1;
  ElementType dtype = ElementType::Float64;
  bool integer = false;
};
Result<SequenceMetadata> metadata(
    Sequence kind, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<SequenceMetadata>;
  if (inputs.size() != 2)
    return Answer(
        {ErrorCode::TypeMismatch, "sequence requires two scalar schemas"});
  const auto count = std::get<std::int64_t>(parameters.at("count"));
  if (count < 1 || count > 1048576)
    return Answer(
        {ErrorCode::InvalidArgument, "sequence count outside bounds"});
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  SequenceMetadata result;
  result.count = static_cast<std::uint32_t>(count);
  result.integer = kind == Sequence::Arange && dtype == "int64";
  if (!result.integer && dtype != "float32" && dtype != "float64")
    return Answer({dtype == "uint8" || dtype == "int64"
                       ? ErrorCode::TypeMismatch
                       : ErrorCode::InvalidArgument,
                   "invalid sequence dtype"});
  result.dtype = result.integer       ? ElementType::Int64
                 : dtype == "float32" ? ElementType::Float32
                                      : ElementType::Float64;
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
    const auto& member = input.result_schema->tensors[0];
    if (member.sample_shape() != std::vector<std::uint64_t>{1} ||
        (result.integer
             ? member.descriptor.element_type != ElementType::Int64
             : member.descriptor.element_type != ElementType::Float32 &&
                   member.descriptor.element_type != ElementType::Float64))
      return Answer({ErrorCode::TypeMismatch,
                     "sequence requires matching numeric-kind scalars"});
  }
  return Answer(result);
}
SchemaTemplate output_schema(const SequenceMetadata& description, bool axis) {
  auto schema = tensor_ops::scalar_schema();
  auto& tensor = schema.tensors[0];
  tensor.descriptor = {
      axis && !description.integer ? ElementType::Float64 : description.dtype,
      {axis ? UINT64_C(3) : description.count}};
  tensor.atomic_trailing_axes = axis ? 1 : 0;
  return schema;
}
Result<OperationProgram> lower_sequence(const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  const auto count = std::get<std::int64_t>(query.parameters.at("count"));
  OperationProgramBuilder builder(query);
  for (unsigned port = 0; port < query.requests.size(); ++port)
    for (const auto& member : query.requests[port].members)
      for (auto tile : member.second) {
        auto task =
            builder.add_task(port ? "axis" : "values", "sequence-unchecked-v1",
                             {}, {sizeof(SequenceMath), 0, 0});
        if (!task.ok())
          return Answer(task.status());
        auto status = builder.read_input(task.value(), 0, 0, 0);
        if (status.ok())
          status = count == 1 ? builder.read_input_schema(task.value(), 1)
                              : builder.read_input(task.value(), 1, 0, 0);
        if (status.ok())
          status = builder.write_output(task.value(), port, member.first, tile);
        if (!status.ok())
          return Answer(status);
      }
  return builder.finish();
}
struct PreparedSequence final {
  Sequence kind;
  SequenceProfile profile;
  SequenceMetadata description;
  bool axis = false;
  std::array<ElementType, 2> types{};
  fixed_tensor::PointInputs<2> inputs;
};
void sequence_invoke(const ps_kernel_invocation_v8* invocation,
                     ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::BackendUnavailable,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& state =
        *static_cast<const PreparedSequence*>(invocation->prepared_context);
    const auto stopped = [&] {
      return invocation->cancellation.is_cancelled &&
             invocation->cancellation.is_cancelled(
                 invocation->cancellation.context);
    };
    if (stopped()) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    const auto count = state.description.count;
    const bool integer = state.description.integer, axis = state.axis;
    const auto type =
        axis && !integer ? ElementType::Float64 : state.description.dtype;
    const auto width = TensorSpan::element_size(type);
    static_assert(alignof(SequenceMath) <= alignof(std::max_align_t));
    auto* math =
        new (invocation->scratch[0].allocation) SequenceMath(state.profile);
    const auto destroy = [](SequenceMath* value) { value->~SequenceMath(); };
    std::unique_ptr<SequenceMath, decltype(destroy)> guard(math, destroy);
    fixed_tensor::PointReader<2> reader{state.inputs, *invocation};
    std::array<std::uint64_t, 2> words{};
    double start = 0, other = 0;
    for (unsigned port = 0; port < (count == 1 ? 1U : 2U); ++port) {
      const auto input_width = TensorSpan::element_size(state.types[port]);
      words[port] = reader.bits(port, {}, input_width);
      if (!integer) {
        double value = 0;
        if (input_width == 4) {
          float small = 0;
          std::memcpy(&small, &words[port], 4);
          value = small;
        } else {
          std::memcpy(&value, &words[port], 8);
        }
        (port ? other : start) = value;
      }
    }
    const auto& output = invocation->outputs[0];
    auto* destination =
        static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
    const auto samples = axis ? 1 : output.layout->shape[0];
    for (std::uint64_t local = 0; local < samples; ++local) {
      if (stopped()) {
        *status = kernel_failure(ErrorCode::Cancelled);
        return;
      }
      const auto index =
          axis ? count - 1
               : static_cast<std::uint32_t>(output.layout->origin[0] + local);
      std::array<std::uint64_t, 3> result{};
      if (integer) {
        std::int64_t base = 0, step = 0;
        std::memcpy(&base, &words[0], 8);
        std::memcpy(&step, &words[1], 8);
        math->first.set_integer(base);
        math->second.set_integer(step);
        numeric_ops::sequence_multiply(&math->second, index, state.profile,
                                       math->products.data());
        math->first.add(math->second);
        // Final Int64 representability is a caller precondition. The exact
        // product and addition retain their full range until conversion.
        const auto magnitude =
            static_cast<std::uint64_t>(math->first.words[0]) |
            (static_cast<std::uint64_t>(math->first.words[1]) << 32);
        result[0] = math->first.negative ? UINT64_C(0) - magnitude : magnitude;
        if (axis)
          result = {words[0], result[0], words[1]};
      } else if (axis) {
        result[0] = math->rounded(start, 0, 1, 0, 1, false);
        result[1] = result[0];
        if (count > 1 && state.kind == Sequence::Linspace) {
          result[1] = math->rounded(other, 0, 1, 0, 1, false);
          result[2] = math->rounded(other, start, 1, 1, count - 1, false, true);
        } else if (count > 1) {
          result[1] = math->rounded(start, other, 1, count - 1, 1, false);
          result[2] = math->rounded(other, 0, 1, 0, 1, false);
        }
      } else {
        const bool narrow = type == ElementType::Float32;
        if (state.kind == Sequence::Linspace && count > 1 && index > 0 &&
            index + 1 < count)
          result[0] = math->rounded(start, other, count - 1 - index, index,
                                    count - 1, narrow);
        else if (state.kind == Sequence::Arange && index > 0)
          result[0] = math->rounded(start, other, 1, index, 1, narrow);
        else
          result[0] = math->rounded(state.kind == Sequence::Linspace &&
                                            count > 1 && index + 1 == count
                                        ? other
                                        : start,
                                    0, 1, 0, 1, narrow);
      }
      std::memcpy(destination + local * width, result.data(),
                  axis ? 24 : width);
    }
    *status = {};
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "sequence invocation failed");
  }
}
OperationDefinition sequence(const std::string& key, Sequence kind,
                             SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.rank = 1;
    port.element_type_mask = kind == Sequence::Linspace ? 12 : 14;
  }
  traits.parameter_schema = {
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  traits.outputs.resize(2);
  for (unsigned port = 0; port < 2; ++port) {
    traits.outputs[port].key = port ? "axis" : "values";
    traits.outputs[port].output_schema = tensor_ops::port();
    traits.outputs[port].result_schema =
        output_schema(SequenceMetadata{}, port != 0);
  }
  traits.requires_metadata_specialization = true;
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto description = metadata(kind, inputs, parameters);
    if (!description.ok())
      return Answer(description.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> result(2);
    for (unsigned port = 0; port < 2; ++port)
      result[port].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(
              output_schema(description.value(), port != 0));
    return Answer(std::move(result));
  };
  operation.lower = lower_sequence;
  operation.prepare_kernel =
      [kind, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    const auto invalid = [] {
      return Answer({ErrorCode::TypeMismatch, "invalid sequence invocation"});
    };
    if ((kernel.symbol != "values" && kernel.symbol != "axis") ||
        kernel.contract != "sequence-unchecked-v1" ||
        query.outputs.size() != 2 || query.writes.size() != 1 ||
        !query.task_parameters.empty() ||
        query.scratch !=
            std::array<std::uint64_t, 3>{sizeof(SequenceMath), 0, 0})
      return invalid();
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "sequence requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto parsed = metadata(kind, query.inputs, query.parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto state = std::make_unique<PreparedSequence>();
    state->kind = kind;
    state->profile = profile;
    state->description = parsed.take_value();
    state->axis = kernel.symbol == "axis";
    for (unsigned port = 0; port < 2; ++port) {
      if (!query.outputs[port].result_schema->same_schema(
              output_schema(state->description, port != 0)))
        return invalid();
      state->types[port] =
          query.inputs[port].result_schema->tensors[0].descriptor.element_type;
    }
    const auto port = state->axis ? 1U : 0U;
    const auto& write = query.writes[0];
    auto grid = TileGrid::from_schema(*query.outputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    auto region = grid.value().region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    if (write.port != port || write.member ||
        write.domain != physical::Domain::Host ||
        write.layout.element_type != (state->axis && !state->description.integer
                                          ? ElementType::Float64
                                          : state->description.dtype) ||
        !fixed_tensor::matches_region(write.layout, region.value()))
      return invalid();
    std::array<Region, 2> regions{
        Region::whole({1}),
        state->description.count == 1 ? Region({{0, 0}}) : Region::whole({1})};
    auto valid =
        fixed_tensor::prepare_input_regions(query, &state->inputs, regions);
    if (!valid.ok())
      return Answer(valid);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = state.release();
    result.invoke_cpu = sequence_invoke;
    result.release = [](const void* context) {
      delete static_cast<const PreparedSequence*>(context);
    };
    return Answer(result);
  };
  return operation;
}
}  // namespace
Status register_numeric_sequences(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("numeric.linspace", Sequence::Linspace),
        std::make_pair("numeric.arange", Sequence::Arange)})
    for (const auto& variant :
         {std::make_pair("_strict", SequenceProfile::Strict),
          std::make_pair("_accelerated_apple_silicon",
                         SequenceProfile::AppleSilicon),
          std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
      auto status = registry->register_operation(
          sequence(std::string(entry.first) + variant.first, entry.second,
                   variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
