#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::PredicateInteger;
using numeric_ops::SequenceProfile;
enum class Comparison {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  IsClose
};
struct ComparisonMath final {
  Comparison kind;
  SequenceProfile profile;
  PredicateInteger difference, temporary, threshold, product;
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  std::array<bool, 4> unordered{};
  ComparisonMath(Comparison operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}
  bool close(std::uint64_t a, std::uint64_t b, bool narrow,
             const BinaryParts& absolute, const BinaryParts& relative) {
    const auto x = BinaryParts::decode(a, narrow),
               y = BinaryParts::decode(b, narrow);
    if (x.nan || y.nan)
      return false;
    if (x.infinite || y.infinite)
      return x.infinite && y.infinite && x.negative == y.negative;
    const auto high = x.magnitude >= y.magnitude ? x : y;
    const auto low = x.magnitude >= y.magnitude ? y : x;
    difference.set(high);
    temporary.set(low);
    if (x.negative != y.negative)
      difference.add(temporary);
    else
      difference.subtract(temporary);
    threshold.set(absolute);
    product.set_product(relative, high);
    threshold.add(product);
    for (std::size_t end = difference.words.size(); end; end -= 4) {
      numeric_ops::compare_keys(difference.words.data() + end - 4,
                                threshold.words.data() + end - 4,
                                greater.data(), less.data(), profile);
      for (unsigned i = 4; i; --i) {
        if (greater[i - 1])
          return false;
        if (less[i - 1])
          return true;
      }
    }
    return true;
  }
  void evaluate(ElementType type, unsigned count, const BinaryParts& absolute,
                const BinaryParts& relative, std::uint8_t* output) {
    if (kind == Comparison::IsClose) {
      for (unsigned i = 0; i < count; ++i)
        output[i] = close(left[i], right[i], type == ElementType::Float32,
                          absolute, relative);
      return;
    }
    const bool floating =
        type == ElementType::Float32 || type == ElementType::Float64;
    for (unsigned i = 0; i < count; ++i) {
      unordered[i] = false;
      if (floating) {
        const auto x =
            BinaryParts::decode(left[i], type == ElementType::Float32);
        const auto y =
            BinaryParts::decode(right[i], type == ElementType::Float32);
        unordered[i] = x.nan || y.nan;
        left[i] = x.order_key();
        right[i] = y.order_key();
      } else if (type == ElementType::Int64) {
        left[i] ^= UINT64_C(1) << 63;
        right[i] ^= UINT64_C(1) << 63;
      }
    }
    numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                              less.data(), profile);
    for (unsigned i = 0; i < count; ++i) {
      bool result = false;
      switch (kind) {
        case Comparison::Equal:
          result = !greater[i] && !less[i];
          break;
        case Comparison::NotEqual:
          result = greater[i] || less[i];
          break;
        case Comparison::Less:
          result = less[i];
          break;
        case Comparison::LessEqual:
          result = !greater[i];
          break;
        case Comparison::Greater:
          result = greater[i];
          break;
        case Comparison::GreaterEqual:
          result = !less[i];
          break;
        case Comparison::IsClose:
          break;
      }
      output[i] = unordered[i] ? kind == Comparison::NotEqual : result;
    }
  }
};
struct ComparisonState final {
  Comparison kind;
  SequenceProfile profile;
  ElementType dtype;
  BinaryParts absolute, relative;
  fixed_tensor::PointInputs<2> inputs;
};
void advance(std::array<std::uint64_t, 8>* at,
             const ps_kernel_layout_v8& layout) noexcept {
  for (unsigned axis = layout.rank; axis-- > 0;) {
    if (++(*at)[axis] < layout.origin[axis] + layout.shape[axis])
      break;
    (*at)[axis] = layout.origin[axis];
  }
}
bool cancelled(const ps_kernel_invocation_v8& invocation) noexcept {
  return invocation.cancellation.is_cancelled &&
         invocation.cancellation.is_cancelled(invocation.cancellation.context);
}
void comparison_invoke(const ps_kernel_invocation_v8* invocation,
                       ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const ComparisonState*>(invocation->prepared_context);
  static_assert(alignof(ComparisonMath) <= alignof(std::max_align_t));
  auto* arithmetic = new (invocation->scratch[0].allocation)
      ComparisonMath(state.kind, state.profile);
  const auto destroy = [](ComparisonMath* value) { value->~ComparisonMath(); };
  std::unique_ptr<ComparisonMath, decltype(destroy)> guard(arithmetic, destroy);
  fixed_tensor::PointReader<2> reader{state.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  std::array<std::uint64_t, 8> at{};
  std::copy(layout.origin, layout.origin + layout.rank, at.begin());
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  const auto width = TensorSpan::element_size(state.dtype);
  for (std::uint64_t begin = 0; begin < count; begin += 4) {
    if (cancelled(*invocation)) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    const auto lanes =
        static_cast<unsigned>(std::min<std::uint64_t>(4, count - begin));
    for (unsigned lane = 0; lane < lanes; ++lane) {
      arithmetic->left[lane] = reader.bits(0, at, width);
      arithmetic->right[lane] = reader.bits(1, at, width);
      advance(&at, layout);
    }
    arithmetic->evaluate(state.dtype, lanes, state.absolute, state.relative,
                         static_cast<std::uint8_t*>(output.allocation) +
                             output.byte_offset + begin);
  }
  *status = {};
}
OperationDefinition comparison(const std::string& key, Comparison kind,
                               SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = kind == Comparison::IsClose ? 12 : 15;
  }
  if (kind == Comparison::IsClose)
    traits.parameter_schema = {{"atol", OperationParameterType::Float64},
                               {"rtol", OperationParameterType::Float64}};
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    for (const auto& input : inputs) {
      auto status = tensor_ops::check_tensor(input);
      if (!status.ok())
        return Answer(status);
    }
    const auto& first = inputs[0].result_schema->tensors[0];
    const auto& second = inputs[1].result_schema->tensors[0];
    const auto shape = first.sample_shape();
    if (shape != second.sample_shape() ||
        first.descriptor.element_type != second.descriptor.element_type)
      return Answer(Status{ErrorCode::TypeMismatch,
                           "comparison inputs must match shape and dtype",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    std::uint64_t count = 1;
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "comparison input exceeds 2^40 elements",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
      count *= extent;
    }
    if (kind == Comparison::IsClose) {
      for (const auto* name : {"atol", "rtol"}) {
        const auto value = std::get<double>(parameters.at(name));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        const auto parts = BinaryParts::decode(bits, false);
        if (parts.nan || parts.infinite || (parts.negative && parts.magnitude))
          return Answer(numeric_ops::array_parameter_error(
              "is_close tolerances must be finite and nonnegative"));
      }
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = {ElementType::UInt8, shape};
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [](const OperationLowering& query) {
    return fixed_tensor::lower_pointwise(query, "compare", "exact-predicate-v1",
                                         sizeof(ComparisonMath));
  };
  operation.prepare_kernel =
      [kind, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != "compare" || kernel.contract != "exact-predicate-v1" ||
        !query.task_parameters.empty() ||
        query.scratch !=
            std::array<std::uint64_t, 3>{sizeof(ComparisonMath), 0, 0})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid comparison kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "comparison requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto state = std::make_unique<ComparisonState>();
    auto valid = fixed_tensor::prepare_point_inputs(query, &state->inputs);
    if (!valid.ok())
      return Answer(valid);
    state->kind = kind;
    state->profile = profile;
    state->dtype =
        query.inputs[0].result_schema->tensors[0].descriptor.element_type;
    if (kind == Comparison::IsClose) {
      const auto decode = [](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        return BinaryParts::decode(bits, false);
      };
      state->absolute = decode(std::get<double>(query.parameters.at("atol")));
      state->relative = decode(std::get<double>(query.parameters.at("rtol")));
    }
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.invoke_cpu = comparison_invoke;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const ComparisonState*>(context);
    };
    return Answer(result);
  };
  return operation;
}

struct SelectionState final {
  fixed_tensor::PointInputs<3> inputs;
  std::size_t width = 0;
};
void selection_invoke(const ps_kernel_invocation_v8* invocation,
                      ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const SelectionState*>(invocation->prepared_context);
  fixed_tensor::PointReader<3> reader{state.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  std::array<std::uint64_t, 8> at{};
  std::copy(layout.origin, layout.origin + layout.rank, at.begin());
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  for (std::uint64_t sample = 0; sample < count; ++sample) {
    if (!(sample & 1023) && cancelled(*invocation)) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    // The caller supplies 0/1 conditions; both branches are fixed dependencies.
    const auto condition = reader.bits(0, at, 1);
    const auto selected = reader.bits(condition ? 1 : 2, at, state.width);
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + sample * state.width,
                &selected, state.width);
    advance(&at, layout);
  }
  *status = {};
}
OperationDefinition selection(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = 15;
  }
  traits.input_schema[0].element_type_mask = 1;
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  traits.requires_metadata_specialization = true;
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
      auto status = tensor_ops::check_tensor(input);
      if (!status.ok())
        return Answer(status);
    }
    const auto shape = inputs[0].result_schema->tensors[0].sample_shape();
    const auto& left = inputs[1].result_schema->tensors[0];
    const auto& right = inputs[2].result_schema->tensors[0];
    if (shape.empty() || shape.size() > 8)
      return mismatch("select requires rank 1..8");
    if (left.descriptor.element_type != right.descriptor.element_type ||
        left.sample_shape() != shape || right.sample_shape() != shape)
      return mismatch("select branch dtypes and all shapes must match");
    std::uint64_t count = 1;
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("select input exceeds 2^40 elements");
      count *= extent;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = {left.descriptor.element_type, shape};
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [](const OperationLowering& query) {
    return fixed_tensor::lower_pointwise(query, "select", "bits-unchecked-v1",
                                         0);
  };
  operation.prepare_kernel =
      [profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != "select" || kernel.contract != "bits-unchecked-v1" ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid select kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "select requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto state = std::make_unique<SelectionState>();
    auto valid = fixed_tensor::prepare_point_inputs(query, &state->inputs);
    if (!valid.ok())
      return Answer(valid);
    state->width =
        TensorSpan::element_size(query.writes[0].layout.element_type);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.invoke_cpu = selection_invoke;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const SelectionState*>(context);
    };
    return Answer(result);
  };
  return operation;
}
}  // namespace
Status register_numeric_comparisons(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& item :
         {std::make_pair("equal", Comparison::Equal),
          std::make_pair("not_equal", Comparison::NotEqual),
          std::make_pair("less", Comparison::Less),
          std::make_pair("less_equal", Comparison::LessEqual),
          std::make_pair("greater", Comparison::Greater),
          std::make_pair("greater_equal", Comparison::GreaterEqual),
          std::make_pair("is_close", Comparison::IsClose)}) {
      auto status = registry->register_operation(
          comparison(std::string("numeric.") + item.first + variant.first,
                     item.second, variant.second));
      if (!status.ok())
        return status;
    }
    auto status = registry->register_operation(selection(
        std::string("numeric.select") + variant.first, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
