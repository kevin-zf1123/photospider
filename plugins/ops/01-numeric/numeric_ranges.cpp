#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_ratio.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
enum class RangeKind { Clamp, Remap };
struct RangeMath final {
  RangeKind kind;
  SequenceProfile profile;
  numeric_ops::RatioWorkspace ratio;
  std::array<std::uint64_t, 5> bits{};
  std::array<BinaryParts, 5> parts{};
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  RangeMath(RangeKind operation, SequenceProfile selected)
      : kind(operation), profile(selected), ratio(selected) {}
  Result<std::uint64_t> evaluate(
      ElementType type, const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    const auto count = kind == RangeKind::Clamp ? 3U : 5U;
    auto status = consume(128);
    if (!status.ok())
      return Answer(status);
    const bool narrow = type == ElementType::Float32;
    const bool floating = narrow || type == ElementType::Float64;
    if (floating) {
      for (unsigned port = 0; port < count; ++port)
        parts[port] = BinaryParts::decode(bits[port], narrow);
    }
    const auto key = [&](std::uint32_t port) {
      return floating                     ? parts[port].order_key()
             : type == ElementType::Int64 ? bits[port] ^ (UINT64_C(1) << 63)
                                          : bits[port];
    };
    left = {key(0), key(0), key(1), 0};
    right = {key(1), key(2), key(2), 0};
    numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                              less.data(), profile);
    std::uint64_t output = 0;
    if (kind == RangeKind::Clamp) {
      output = floating && parts[0].nan
                   ? bits[0] | (UINT64_C(1) << (narrow ? 22 : 51))
               : less[0]    ? bits[1]
               : greater[1] ? bits[2]
                            : bits[0];
    } else {
      if (parts[0].nan) {
        output = bits[0] | (UINT64_C(1) << (narrow ? 22 : 51));
      } else if (key(3) == key(4)) {
        output = bits[3];
      } else if (key(0) == key(1)) {
        output = bits[3];
      } else if (key(0) == key(2)) {
        output = bits[4];
      } else if (parts[0].infinite) {
        const bool negative = parts[0].negative != (key(4) < key(3));
        output = (static_cast<std::uint64_t>(negative) << (narrow ? 31 : 63)) |
                 (narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
      } else {
        status = consume(2048);
        if (!status.ok())
          return Answer(status);
        // Form a strictly positive denominator in units 2^-1074.
        ratio.numerator.set(parts[2], 1074);
        ratio.negative = parts[2].negative;
        ratio.term.set(parts[1], 1074);
        ratio.add_term(!parts[1].negative);
        ratio.denominator = ratio.numerator;
        ratio.numerator.words.fill(0);
        ratio.negative = false;
        // Algebraically expand the whole formula before its single rounding.
        ratio.product_term(parts[3], parts[2]);
        ratio.product_term(parts[0], parts[4]);
        ratio.product_term(parts[0], parts[3], true);
        ratio.product_term(parts[1], parts[4], true);
        auto rounded = ratio.round(narrow, consume, -1074, true);
        if (!rounded.ok())
          return Answer(rounded.status());
        output = rounded.take_value();
      }
    }
    return Answer(output);
  }
};
template <std::size_t Ports>
struct RangeState final {
  SequenceProfile profile;
  fixed_tensor::PointInputs<Ports> inputs;
};
template <std::size_t Ports>
void range_invoke(const ps_kernel_invocation_v8* invocation,
                  ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state =
      *static_cast<const RangeState<Ports>*>(invocation->prepared_context);
  static_assert(alignof(RangeMath) <= alignof(std::max_align_t));
  try {
    auto* math = new (invocation->scratch[0].allocation) RangeMath(
        Ports == 3 ? RangeKind::Clamp : RangeKind::Remap, state.profile);
    const auto destroy = [](RangeMath* value) { value->~RangeMath(); };
    std::unique_ptr<RangeMath, decltype(destroy)> guard(math, destroy);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    fixed_tensor::PointReader<Ports> reader{state.inputs, *invocation};
    const auto& output = invocation->outputs[0];
    const auto& layout = *output.layout;
    const auto dtype = static_cast<ElementType>(layout.element_type);
    const auto width = TensorSpan::element_size(dtype);
    std::array<std::uint64_t, 8> at{};
    std::copy(layout.origin, layout.origin + layout.rank, at.begin());
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < layout.rank; ++axis)
      count *= layout.shape[axis];
    for (std::uint64_t sample = 0; sample < count; ++sample) {
      for (unsigned port = 0; port < Ports; ++port)
        math->bits[port] = reader.bits(port, at, width);
      auto value = math->evaluate(dtype, consume);
      if (!value.ok()) {
        *status =
            kernel_failure(value.status().code, value.status().message.c_str(),
                           value.status().reason);
        return;
      }
      const auto bits = value.value();
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                      output.byte_offset + sample * width,
                  &bits, width);
      for (unsigned axis = layout.rank; axis-- > 0;) {
        if (++at[axis] < layout.origin[axis] + layout.shape[axis])
          break;
        at[axis] = layout.origin[axis];
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "range arithmetic failed");
  }
}
template <std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare_range(SequenceProfile profile,
                                            const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  auto state = std::make_unique<RangeState<Ports>>();
  auto valid = fixed_tensor::prepare_point_inputs(query, &state->inputs);
  if (!valid.ok())
    return Answer(valid);
  state->profile = profile;
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.invoke_cpu = range_invoke<Ports>;
  result.context = state.release();
  result.release = [](const void* context) {
    delete static_cast<const RangeState<Ports>*>(context);
  };
  return Answer(result);
}
OperationDefinition range_operation(const std::string& key, RangeKind kind,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = kind == RangeKind::Clamp ? 3 : 5;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = kind == RangeKind::Clamp ? 15 : 12;
  }
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [profile](const auto& inputs, const auto&)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Answer(Status{ErrorCode::TypeMismatch,
                           message,
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    };
    for (const auto& input : inputs) {
      auto valid = tensor_ops::check_tensor(input);
      if (!valid.ok())
        return Answer(valid);
    }
    const auto& selected = inputs[0].result_schema->tensors[0];
    const TensorDescriptor first{selected.descriptor.element_type,
                                 selected.sample_shape()};
    if (first.shape.empty() || first.shape.size() > 8)
      return mismatch("range requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("range input exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.result_schema->tensors[0].sample_shape() != first.shape ||
          input.result_schema->tensors[0].descriptor.element_type !=
              first.element_type)
        return mismatch("range operands require identical shape and dtype");
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = first;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  const std::string symbol = kind == RangeKind::Clamp ? "clamp" : "remap";
  operation.lower = [symbol](const OperationLowering& query) {
    return fixed_tensor::lower_pointwise(query, symbol, "range-unchecked-v1",
                                         sizeof(RangeMath));
  };
  operation.prepare_kernel =
      [kind, profile, symbol](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != symbol || kernel.contract != "range-unchecked-v1" ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{sizeof(RangeMath), 0, 0})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid range kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer(
          {ErrorCode::BackendUnavailable, "range requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    return kind == RangeKind::Clamp ? prepare_range<3>(profile, query)
                                    : prepare_range<5>(profile, query);
  };
  return operation;
}
}  // namespace
Status register_numeric_ranges(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(
        range_operation(std::string("numeric.clamp") + variant.first,
                        RangeKind::Clamp, variant.second));
    if (!status.ok())
      return status;
    status = registry->register_operation(
        range_operation(std::string("numeric.remap_range") + variant.first,
                        RangeKind::Remap, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
