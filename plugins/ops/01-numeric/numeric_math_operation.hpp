#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/certified_math.hpp"
#include "01-numeric/exact_elementary.hpp"
#include "01-numeric/exp_simd.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/tensor_program.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::numeric_ops {
struct FloatMathWorkspace final {
  static constexpr std::size_t kCount = 64;
  static_assert(kCount <= 64);
  std::array<float, kCount> input{}, output{};
  std::array<std::uint32_t, kCount> bits{};
};
struct MathBatchWorkspace final {
  static constexpr std::size_t kCount = 64;
  std::array<double, kCount> a{}, b{}, output{};
  std::array<std::array<std::uint64_t, 2>, kCount> bits{};
  std::array<bool, kCount> eligible{};
  std::array<std::uint8_t*, kCount> destination{};
};
inline bool sleef_batch_kind(CertifiedKind kind) {
  return (kind >= CertifiedKind::Exp && kind <= CertifiedKind::Tan) ||
         kind == CertifiedKind::Pow || kind == CertifiedKind::Atan2;
}
template <class Arithmetic, class Kind>
Status compute_math_batch(Arithmetic& arithmetic, Kind kind,
                          SequenceProfile profile, ElementType dtype,
                          MathBatchWorkspace& block, FloatMathWorkspace& fast,
                          std::size_t size,
                          const std::function<Status(std::uint64_t)>& consume,
                          const plugin_internal::FloatingEnvironment& env) {
  const auto width = TensorSpan::element_size(dtype);
  const auto store = [&](std::size_t lane, std::uint64_t bits) {
    std::memcpy(block.destination[lane], &bits, width);
  };
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if (profile != SequenceProfile::Strict && env.active() &&
        dtype == ElementType::Float32 && accelerated_math_available() &&
        (kind == CertifiedKind::Exp ||
         trig_simd_kind(static_cast<unsigned>(kind)))) {
      std::uint64_t rejected = 0, fast_count = size;
      for (std::size_t lane = 0; lane < size; ++lane) {
        fast.bits[lane] = static_cast<std::uint32_t>(block.bits[lane][0]);
        const bool eligible =
            kind == CertifiedKind::Exp
                ? (fast.bits[lane] & UINT32_C(0x7fffffff)) <=
                      UINT32_C(0x42a00000)
                : trig_simd_domain(static_cast<unsigned>(kind),
                                   fast.bits[lane]);
        if (!eligible) {
          rejected |= UINT64_C(1) << lane;
          --fast_count;
          fast.input[lane] = 0;
        } else {
          std::memcpy(&fast.input[lane], &fast.bits[lane], 4);
        }
      }
      auto status = consume(fast_count * DirectedInterval::kSlots *
                            DirectedInterval::kWords);
      if (!status.ok())
        return status;
      if (kind == CertifiedKind::Exp)
        exp_simd_f32(fast.input.data(), fast.output.data(), size);
      else
        trig_simd_f32(static_cast<unsigned>(kind), fast.input.data(),
                      fast.output.data(), size);
      for (std::size_t lane = 0; lane < size; ++lane) {
        if (rejected & (UINT64_C(1) << lane)) {
          auto calculated = arithmetic.evaluate(
              kind, dtype, fast.bits[lane], 0, consume,
              [] { return Status::success(); }, true);
          if (!calculated.ok())
            return calculated.status();
          store(lane, calculated.value());
        } else {
          std::uint32_t word;
          std::memcpy(&word, &fast.output[lane], 4);
          store(lane, word);
        }
      }
      return Status::success();
    }
    if (sleef_batch_kind(kind) && profile != SequenceProfile::Strict &&
        env.active() && accelerated_math_available()) {
      const bool narrow = dtype == ElementType::Float32;
      for (std::size_t lane = 0; lane < size; ++lane) {
        bool finite = true;
        for (const auto bits : block.bits[lane]) {
          const auto parts = BinaryParts::decode(bits, narrow);
          finite = finite && !parts.nan && !parts.infinite;
        }
        const auto a = finite ? numeric_double(block.bits[lane][0], narrow) : 1;
        const auto b = finite ? numeric_double(block.bits[lane][1], narrow) : 1;
        block.eligible[lane] = finite && accelerated_math_domain(
                                             static_cast<unsigned>(kind), a, b);
        block.a[lane] = block.eligible[lane] ? a : 1;
        block.b[lane] = block.eligible[lane] ? b : 1;
      }
      auto status =
          consume(size * DirectedInterval::kSlots * DirectedInterval::kWords);
      if (!status.ok())
        return status;
      photospider_sleef_evaluate(static_cast<unsigned>(kind), block.a.data(),
                                 block.b.data(), block.output.data(), size);
      for (std::size_t lane = 0; lane < size; ++lane) {
        std::optional<std::uint64_t> candidate;
        if (block.eligible[lane])
          candidate = accelerated_math_enclosure(block.output[lane])
                          .accepted(block.output[lane], narrow);
        auto calculated = arithmetic.evaluate(
            kind, dtype, block.bits[lane][0], block.bits[lane][1], consume,
            [] { return Status::success(); }, true, &candidate, true);
        if (!calculated.ok())
          return calculated.status();
        store(lane, calculated.value());
      }
      return Status::success();
    }
  }
  for (std::size_t lane = 0; lane < size; ++lane) {
    auto calculated = [&] {
      if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
        return arithmetic.evaluate(
            kind, dtype, block.bits[lane][0], block.bits[lane][1], consume,
            [] { return Status::success(); }, env.active());
      } else {
        return arithmetic.evaluate(kind, dtype, block.bits[lane][0],
                                   block.bits[lane][1], consume, &env);
      }
    }();
    if (!calculated.ok())
      return calculated.status();
    store(lane, calculated.value());
  }
  return Status::success();
}

template <class Arithmetic>
struct PointMathScratch final {
  Arithmetic arithmetic;
  MathBatchWorkspace block;
  FloatMathWorkspace fast;
  explicit PointMathScratch(SequenceProfile profile) : arithmetic(profile) {}
};
template <class Kind, std::size_t Ports>
struct PointMathState final {
  Kind kind;
  SequenceProfile profile;
  ElementType output_type;
  std::size_t input_width = 0;
  fixed_tensor::PointInputs<Ports> inputs;
};
template <class Arithmetic, class Kind, std::size_t Ports>
void point_math_invoke(const ps_kernel_invocation_v8* invocation,
                       ps_kernel_status_v8* status) noexcept {
  plugin_internal::FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state = *static_cast<const PointMathState<Kind, Ports>*>(
      invocation->prepared_context);
  using Scratch = PointMathScratch<Arithmetic>;
  static_assert(alignof(Scratch) <= alignof(std::max_align_t));
  try {
    auto* scratch =
        new (invocation->scratch[0].allocation) Scratch(state.profile);
    const auto destroy = [](Scratch* value) { value->~Scratch(); };
    std::unique_ptr<Scratch, decltype(destroy)> guard(scratch, destroy);
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
    const auto width = TensorSpan::element_size(state.output_type);
    std::array<std::uint64_t, 8> at{};
    std::copy(layout.origin, layout.origin + layout.rank, at.begin());
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis < layout.rank; ++axis)
      count *= layout.shape[axis];
    for (std::uint64_t offset = 0; offset < count;
         offset += MathBatchWorkspace::kCount) {
      auto cancelled = consume(1);
      if (!cancelled.ok()) {
        *status = kernel_failure(cancelled.code);
        return;
      }
      const auto size =
          std::min<std::uint64_t>(MathBatchWorkspace::kCount, count - offset);
      for (std::size_t lane = 0; lane < size; ++lane) {
        scratch->block.bits[lane][0] = reader.bits(0, at, state.input_width);
        if constexpr (Ports == 2)
          scratch->block.bits[lane][1] = reader.bits(1, at, state.input_width);
        scratch->block.destination[lane] =
            static_cast<std::uint8_t*>(output.allocation) + output.byte_offset +
            (offset + lane) * width;
        for (unsigned axis = layout.rank; axis-- > 0;) {
          if (++at[axis] < layout.origin[axis] + layout.shape[axis])
            break;
          at[axis] = layout.origin[axis];
        }
      }
      auto calculated = compute_math_batch(
          scratch->arithmetic, state.kind, state.profile, state.output_type,
          scratch->block, scratch->fast, size, consume, environment);
      if (!calculated.ok()) {
        *status = kernel_failure(calculated.code, calculated.message.c_str(),
                                 calculated.reason);
        return;
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "numeric arithmetic failed");
  }
}
template <class Arithmetic, class Kind, std::size_t Ports>
Result<ps_prepared_kernel_v8> prepare_point_math(
    Kind kind, SequenceProfile profile, const KernelPreparation& query) {
  using State = PointMathState<Kind, Ports>;
  using Answer = Result<ps_prepared_kernel_v8>;
  auto state = std::make_unique<State>();
  auto valid = fixed_tensor::prepare_point_inputs(query, &state->inputs);
  if (!valid.ok())
    return Answer(valid);
  state->kind = kind;
  state->profile = profile;
  state->output_type = query.writes[0].layout.element_type;
  state->input_width = TensorSpan::element_size(
      query.inputs[0].result_schema->tensors[0].descriptor.element_type);
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.invoke_cpu = point_math_invoke<Arithmetic, Kind, Ports>;
  result.context = state.release();
  result.release = [](const void* context) {
    delete static_cast<const State*>(context);
  };
  return Answer(result);
}

template <class Arithmetic, class Kind>
OperationDefinition point_math_operation(const std::string& key, Kind kind,
                                         SequenceProfile profile,
                                         unsigned ports, unsigned dtype_mask,
                                         bool rational = false) {
  using Scratch = PointMathScratch<Arithmetic>;
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = ports;
  traits.input_schema.resize(ports);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = dtype_mask;
  }
  traits.requires_metadata_specialization = true;
  if (rational)
    traits.parameter_schema = {{"dtype", OperationParameterType::String}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [profile, rational](const auto& inputs,
                                                      const auto& parameters)
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
    const auto& first = inputs[0].result_schema->tensors[0];
    const auto shape = first.sample_shape();
    if (shape.empty() || shape.size() > 8)
      return mismatch("numeric math requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("numeric math exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs) {
      const auto& member = input.result_schema->tensors[0];
      if (member.sample_shape() != shape ||
          member.descriptor.element_type != first.descriptor.element_type)
        return mismatch("numeric math inputs must match shape and dtype");
    }
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    SchemaTemplate schema;
    schema.id = "photospider.tensor";
    ResultTensorSpec tensor;
    tensor.key = "samples";
    tensor.descriptor = {first.descriptor.element_type, shape};
    if (rational) {
      const auto& dtype = std::get<std::string>(parameters.at("dtype"));
      if (dtype != "float32" && dtype != "float64")
        return Answer(
            array_parameter_error("rational pi dtype must be float32/float64"));
      tensor.descriptor.element_type =
          dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
    }
    schema.tensors.push_back(std::move(tensor));
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [](const OperationLowering& query) {
    return fixed_tensor::lower_pointwise(
        query, "point-math", "numeric-unchecked-v1", sizeof(Scratch));
  };
  operation.prepare_kernel =
      [kind, profile, ports](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != "point-math" ||
        kernel.contract != "numeric-unchecked-v1" ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{sizeof(Scratch), 0, 0})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid point-math kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "point-math requires CPU Host placement"});
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    if (ports == 1)
      return prepare_point_math<Arithmetic, Kind, 1>(kind, profile, query);
    if (ports == 2)
      return prepare_point_math<Arithmetic, Kind, 2>(kind, profile, query);
    return Answer({ErrorCode::TypeMismatch, "unsupported point-math arity"});
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
