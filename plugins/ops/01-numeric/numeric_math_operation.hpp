#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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
#include "photospider/execution/resource_allocator.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::numeric_ops {
struct ExpWorkspace final {
  static constexpr std::size_t kCount = 64;
  std::array<float, kCount> input{}, output{};
  std::array<std::uint32_t, kCount> bits{};
};
struct MathBatchWorkspace final {
  static constexpr std::size_t kCount = 64;
  std::array<double, kCount> a{}, b{}, output{};
  std::array<std::array<std::uint64_t, 2>, kCount> bits{};
  std::array<bool, kCount> eligible{};
};
inline bool sleef_batch_kind(CertifiedKind kind) {
  return (kind >= CertifiedKind::Ln && kind <= CertifiedKind::Tan) ||
         kind == CertifiedKind::Pow || kind == CertifiedKind::Atan2;
}
// One admitted arithmetic workspace and one complete packed result per
// callback. The registry validates full typed inputs before entering this
// function.
template <class Arithmetic, class Kind>
Result<Value> execute_point_math(const OperationInvocation& call, Kind kind,
                                 SequenceProfile profile, bool rational) {
  using Answer = Result<Value>;
  const auto* budget = resource_internal::metadata_budget();
  const std::function<Status(std::uint64_t)> consume = [&](std::uint64_t work) {
    if (call.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return budget ? budget->consume({work}) : Status::success();
  };
  auto status = consume(1);
  if (!status.ok())
    return Answer(status);
  auto descriptor = call.inputs[0].descriptor();
  if (rational)
    descriptor.element_type =
        std::get<std::string>(call.parameters.at("dtype")) == "float32"
            ? ElementType::Float32
            : ElementType::Float64;
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  std::size_t extra = 0;
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if (kind == CertifiedKind::Exp && profile != SequenceProfile::Strict)
      extra = sizeof(ExpWorkspace);
    else if (sleef_batch_kind(kind) && profile != SequenceProfile::Strict)
      extra = sizeof(MathBatchWorkspace);
    static_assert(sizeof(Arithmetic) % alignof(ExpWorkspace) == 0);
    static_assert(sizeof(Arithmetic) % alignof(MathBatchWorkspace) == 0);
  }
  auto storage = call.allocator.allocate(sizeof(Arithmetic) + extra);
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  static_assert(alignof(Arithmetic) <= alignof(std::max_align_t));
  std::unique_ptr<Arithmetic, void (*)(Arithmetic*)> arithmetic(
      new (scratch.data()) Arithmetic(profile),
      [](Arithmetic* value) { value->~Arithmetic(); });
  const auto source_width =
      Value::element_size(call.inputs[0].descriptor().element_type);
  const auto output_width = Value::element_size(descriptor.element_type);
  const auto& shape = descriptor.shape;
  const auto count = call.output_region.element_count().value();
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  std::array<const std::uint8_t*, 2> packed{};
  for (std::size_t port = 0; port < call.inputs.size(); ++port) {
    const auto& input = call.inputs[port];
    std::uint64_t stride = source_width;
    bool dense = true;
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (shape[axis - 1] > 1 && input.layout().byte_strides[axis - 1] !=
                                     static_cast<std::int64_t>(stride))
        dense = false;
      stride *= shape[axis - 1];
    }
    if (dense) {
      auto address = input.byte_address(coordinate);
      if (!address.ok())
        return Answer(address.status());
      packed[port] = input.bytes().data() + address.value();
    }
  }
  std::optional<input_internal::Float32Environment> environment;
  environment.emplace();
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if (kind == CertifiedKind::Exp && profile != SequenceProfile::Strict &&
        descriptor.element_type == ElementType::Float32 &&
        accelerated_math_available()) {
      if (environment->active()) {
        auto* block = new (scratch.data() + sizeof(Arithmetic)) ExpWorkspace;
        for (std::uint64_t offset = 0; offset < count;
             offset += ExpWorkspace::kCount) {
          const auto size =
              std::min<std::uint64_t>(ExpWorkspace::kCount, count - offset);
          // Charge indexing here; arithmetic admission is charged exactly once
          // per lane, either below for a SIMD lane or by evaluate on fallback.
          status = consume(size * (call.inputs.size() * shape.size() + 1));
          if (!status.ok())
            return Answer(status);
          std::uint64_t fast_count = 0;
          for (std::size_t lane = 0; lane < size; ++lane) {
            const auto& input = call.inputs[0];
            const auto* data = packed[0];
            if (data) {
              data += (offset + lane) * source_width;
            } else {
              auto address = input.byte_address(coordinate);
              if (!address.ok())
                return Answer(address.status());
              data = input.bytes().data() + address.value();
            }
            std::memcpy(&block->bits[lane], data, 4);
            // Classify by bits before any FP instruction, including sNaN.
            const auto magnitude = block->bits[lane] & UINT32_C(0x7fffffff);
            const auto safe = magnitude <= UINT32_C(0x42a00000)
                                  ? block->bits[lane]
                                  : UINT32_C(0);
            fast_count += magnitude <= UINT32_C(0x42a00000);
            std::memcpy(&block->input[lane], &safe, 4);
            for (std::size_t axis = shape.size(); axis; --axis) {
              if (++coordinate[axis - 1] < shape[axis - 1])
                break;
              coordinate[axis - 1] = 0;
            }
          }
          status = consume(fast_count * DirectedInterval::kSlots *
                           DirectedInterval::kWords);
          if (!status.ok())
            return Answer(status);
          exp_simd_f32(block->input.data(), block->output.data(), size);
          for (std::size_t lane = 0; lane < size; ++lane) {
            const auto magnitude = block->bits[lane] & UINT32_C(0x7fffffff);
            std::uint32_t word;
            if (magnitude <= UINT32_C(0x42a00000)) {
              std::memcpy(&word, &block->output[lane], 4);
            } else {
              auto calculated = arithmetic->evaluate(
                  kind, descriptor.element_type, block->bits[lane], 0, consume,
                  [] { return Status::success(); });
              if (!calculated.ok())
                return Answer(calculated.status());
              word = static_cast<std::uint32_t>(calculated.value());
            }
            std::memcpy(output.data() + (offset + lane) * 4, &word, 4);
          }
        }
        status = consume(1);
        return status.ok() ? std::move(output).publish() : Answer(status);
      }
    }
  }
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if (sleef_batch_kind(kind) && profile != SequenceProfile::Strict &&
        environment->active() && accelerated_math_available()) {
      auto* block =
          new (scratch.data() + sizeof(Arithmetic)) MathBatchWorkspace;
      const bool narrow = descriptor.element_type == ElementType::Float32;
      for (std::uint64_t offset = 0; offset < count;
           offset += MathBatchWorkspace::kCount) {
        const auto size =
            std::min<std::uint64_t>(MathBatchWorkspace::kCount, count - offset);
        status = consume(size * (call.inputs.size() * shape.size() + 1));
        if (!status.ok())
          return Answer(status);
        for (std::size_t lane = 0; lane < size; ++lane) {
          block->bits[lane] = {};
          bool finite = true;
          for (std::size_t port = 0; port < call.inputs.size(); ++port) {
            const auto& input = call.inputs[port];
            const auto* data = packed[port];
            if (data) {
              data += (offset + lane) * source_width;
            } else {
              auto address = input.byte_address(coordinate);
              if (!address.ok())
                return Answer(address.status());
              data = input.bytes().data() + address.value();
            }
            std::memcpy(&block->bits[lane][port], data, source_width);
            const auto parts =
                BinaryParts::decode(block->bits[lane][port], narrow);
            finite = finite && !parts.nan && !parts.infinite;
          }
          // Never convert a signaling NaN or feed a rejected domain to SIMD.
          const auto a =
              finite ? numeric_double(block->bits[lane][0], narrow) : 1;
          const auto b =
              finite ? numeric_double(block->bits[lane][1], narrow) : 1;
          block->eligible[lane] =
              finite &&
              accelerated_math_domain(static_cast<unsigned>(kind), a, b);
          block->a[lane] = block->eligible[lane] ? a : 1;
          block->b[lane] = block->eligible[lane] ? b : 1;
          for (std::size_t axis = shape.size(); axis; --axis) {
            if (++coordinate[axis - 1] < shape[axis - 1])
              break;
            coordinate[axis - 1] = 0;
          }
        }
        status =
            consume(size * DirectedInterval::kSlots * DirectedInterval::kWords);
        if (!status.ok())
          return Answer(status);
        photospider_sleef_evaluate(static_cast<unsigned>(kind), block->a.data(),
                                   block->b.data(), block->output.data(), size);
        for (std::size_t lane = 0; lane < size; ++lane) {
          std::optional<std::uint64_t> candidate;
          if (block->eligible[lane])
            candidate = accelerated_math_enclosure(block->output[lane])
                            .accepted(block->output[lane], narrow);
          // evaluate retains all exact landmarks, special values, admission
          // and strict refinement; fixed arithmetic admission is precharged
          // before SIMD, so evaluate only checks cancellation at that point.
          auto calculated = arithmetic->evaluate(
              kind, descriptor.element_type, block->bits[lane][0],
              block->bits[lane][1], consume, [] { return Status::success(); },
              true, &candidate, true);
          if (!calculated.ok())
            return Answer(calculated.status());
          const auto word = calculated.value();
          std::memcpy(output.data() + (offset + lane) * output_width, &word,
                      output_width);
        }
      }
      status = consume(1);
      return status.ok() ? std::move(output).publish() : Answer(status);
    }
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    status = consume(call.inputs.size() * shape.size() + 1);
    if (!status.ok())
      return Answer(status);
    std::array<std::uint64_t, 2> bits{};
    for (std::size_t port = 0; port < call.inputs.size(); ++port) {
      const auto& input = call.inputs[port];
      const std::uint8_t* data = packed[port];
      if (data) {
        data += i * source_width;
      } else {
        auto address = input.byte_address(coordinate);
        if (!address.ok())
          return Answer(address.status());
        data = input.bytes().data() + address.value();
      }
      // Constant-width copies also support unaligned and shifted storage.
      if (source_width == 8)
        std::memcpy(&bits[port], data, 8);
      else if (source_width == 4)
        std::memcpy(&bits[port], data, 4);
      else
        bits[port] = *data;
    }
    Result<std::uint64_t> calculated = [&] {
      if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
        return arithmetic->evaluate(
            kind, descriptor.element_type, bits[0], bits[1], consume,
            [] { return Status::success(); }, environment->active());
      } else {
        return arithmetic->evaluate(kind, descriptor.element_type, bits[0],
                                    bits[1], consume, &*environment);
      }
    }();
    if (!calculated.ok()) {
      auto failure = calculated.status();
      if (failure.detail.scope == FailureScope::Atom) {
        failure.detail.scope = FailureScope::Run;
        failure.detail.atom.reset();
      }
      return Answer(failure);
    }
    const auto word = calculated.value();
    auto* destination = output.data() + i * output_width;
    if (output_width == 8)
      std::memcpy(destination, &word, 8);
    else if (output_width == 4)
      std::memcpy(destination, &word, 4);
    else
      *destination = static_cast<std::uint8_t>(word);
    for (std::size_t axis = shape.size(); axis; --axis) {
      if (++coordinate[axis - 1] < shape[axis - 1])
        break;
      coordinate[axis - 1] = 0;
    }
  }
  status = consume(1);
  return status.ok() ? std::move(output).publish() : Answer(status);
}
template <class Arithmetic, class Kind>
OperationDefinition point_math_operation(const std::string& key, Kind kind,
                                         SequenceProfile profile,
                                         unsigned ports, unsigned dtype_mask,
                                         bool rational = false) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = ports;
  traits.input_schema.resize(ports);
  for (auto& input : traits.input_schema)
    input.element_type_mask = dtype_mask;
  traits.requires_metadata_specialization = true;
  if (rational)
    traits.parameter_schema = {{"dtype", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(Arithmetic);
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if (kind == CertifiedKind::Exp && profile != SequenceProfile::Strict)
      traits.workspace_bytes += sizeof(ExpWorkspace);
    else if (sleef_batch_kind(kind) && profile != SequenceProfile::Strict)
      traits.workspace_bytes += sizeof(MathBatchWorkspace);
  }
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
    const auto& first = inputs[0].descriptor;
    if (first.shape.empty() || first.shape.size() > 8)
      return mismatch("numeric math requires rank 1..8");
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("numeric math exceeds 2^40 elements");
      count *= extent;
    }
    for (const auto& input : inputs)
      if (input.descriptor.shape != first.shape ||
          input.descriptor.element_type != first.element_type)
        return mismatch("numeric math inputs must match shape and dtype");
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = first;
    if (rational) {
      const auto& dtype = std::get<std::string>(parameters.at("dtype"));
      if (dtype != "float32" && dtype != "float64")
        return Answer(
            array_parameter_error("rational pi dtype must be float32/float64"));
      result.metadata.descriptor.element_type =
          dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
    }
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [kind, profile,
                        rational](const OperationInvocation& call) {
    return execute_point_math<Arithmetic>(call, kind, profile, rational);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
