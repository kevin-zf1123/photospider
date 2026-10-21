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

#include "01-numeric/exact_interpolation.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
enum class InterpolationKind { Mix, Smoothstep };
struct InterpolationMath final {
  InterpolationKind kind;
  numeric_ops::InterpolationWorkspace exact;
  std::array<std::uint64_t, 3> bits{};
  std::array<BinaryParts, 3> parts{};
  InterpolationMath(InterpolationKind operation, SequenceProfile profile)
      : kind(operation), exact(profile) {}
  Result<std::uint64_t> evaluate(
      ElementType type, const std::function<Status(std::uint64_t)>& consume) {
    using Answer = Result<std::uint64_t>;
    auto status = consume(128);
    if (!status.ok())
      return Answer(status);
    const bool narrow = type == ElementType::Float32;
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    const auto infinity =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    for (unsigned port = 0; port < 3; ++port)
      parts[port] = BinaryParts::decode(bits[port], narrow);
    if (kind == InterpolationKind::Mix) {
      if (!parts[2].magnitude || bits[2] == one)
        return Answer(bits[bits[2] == one ? 1 : 0]);
      if (parts[0].nan || parts[1].nan)
        return Answer(bits[parts[0].nan ? 0 : 1] | quiet);
      if (parts[0].infinite || parts[1].infinite)
        return Answer(parts[0].infinite && parts[1].infinite &&
                              parts[0].negative != parts[1].negative
                          ? infinity | quiet
                          : bits[parts[0].infinite ? 0 : 1]);
      return exact.mix(parts[0], parts[1], parts[2], narrow, consume);
    }
    if (parts[0].nan)
      return Answer(bits[0] | quiet);
    if (parts[0].order_key() <= parts[1].order_key())
      return Answer(UINT64_C(0));
    if (parts[0].order_key() >= parts[2].order_key())
      return Answer(one);
    return exact.smoothstep(parts[0], parts[1], parts[2], narrow, consume);
  }
};
struct InterpolationState final {
  InterpolationKind kind;
  SequenceProfile profile;
  fixed_tensor::PointInputs<3> inputs;
};
void interpolation_invoke(const ps_kernel_invocation_v8* invocation,
                          ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state =
      *static_cast<const InterpolationState*>(invocation->prepared_context);
  static_assert(alignof(InterpolationMath) <= alignof(std::max_align_t));
  auto* math = new (invocation->scratch[0].allocation)
      InterpolationMath(state.kind, state.profile);
  const auto destroy = [](InterpolationMath* value) {
    value->~InterpolationMath();
  };
  std::unique_ptr<InterpolationMath, decltype(destroy)> guard(math, destroy);
  try {
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto& output = invocation->outputs[0];
    const auto& layout = *output.layout;
    const auto dtype = static_cast<ElementType>(layout.element_type);
    const auto width = TensorSpan::element_size(dtype);
    fixed_tensor::PointReader<3> reader{state.inputs, *invocation};
    std::array<std::uint64_t, 8> at{};
    std::copy(layout.origin, layout.origin + layout.rank, at.begin());
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < layout.rank; ++axis)
      count *= layout.shape[axis];
    for (std::uint64_t sample = 0; sample < count; ++sample) {
      for (unsigned port = 0; port < 3; ++port)
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
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "interpolation arithmetic failed");
  }
}
OperationDefinition interpolation_operation(const std::string& key,
                                            InterpolationKind kind,
                                            SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 3;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = 12;
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
    const auto& member = inputs[0].result_schema->tensors[0];
    const TensorDescriptor first{member.descriptor.element_type,
                                 member.sample_shape()};
    if (first.shape.empty() || first.shape.size() > 8)
      return mismatch("interpolation requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("interpolation input exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.result_schema->tensors[0].sample_shape() != first.shape ||
          input.result_schema->tensors[0].descriptor.element_type !=
              first.element_type)
        return mismatch(
            "interpolation operands require identical shape and dtype");
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
  const std::string symbol =
      kind == InterpolationKind::Mix ? "mix" : "smoothstep";
  operation.lower = [symbol](const OperationLowering& query) {
    return fixed_tensor::lower_pointwise(query, symbol, "exact-unchecked-v1",
                                         sizeof(InterpolationMath));
  };
  operation.prepare_kernel =
      [kind, profile, symbol](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != symbol || kernel.contract != "exact-unchecked-v1" ||
        !query.task_parameters.empty() ||
        query.scratch !=
            std::array<std::uint64_t, 3>{sizeof(InterpolationMath), 0, 0})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid interpolation kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "interpolation requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto state = std::make_unique<InterpolationState>();
    state->kind = kind;
    state->profile = profile;
    auto valid = fixed_tensor::prepare_point_inputs(query, &state->inputs);
    if (!valid.ok())
      return Answer(valid);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.invoke_cpu = interpolation_invoke;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const InterpolationState*>(context);
    };
    return Answer(result);
  };
  return operation;
}
}  // namespace
Status register_numeric_interpolation(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& item :
         {std::make_pair("mix", InterpolationKind::Mix),
          std::make_pair("smoothstep", InterpolationKind::Smoothstep)}) {
      auto status = registry->register_operation(interpolation_operation(
          std::string("numeric.") + item.first + variant.first, item.second,
          variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
