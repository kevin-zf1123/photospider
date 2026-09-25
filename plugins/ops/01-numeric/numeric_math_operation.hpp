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
struct FloatMathWorkspace final {
  static constexpr std::size_t kCount = 64;
  static_assert(kCount <= 64);  // One rejection-mask bit per lane.
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
  return (kind >= CertifiedKind::Exp && kind <= CertifiedKind::Tan) ||
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
  auto* const output_data = output.data();
  std::size_t extra = 0;
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if (profile != SequenceProfile::Strict &&
        (kind == CertifiedKind::Exp ||
         trig_simd_kind(static_cast<unsigned>(kind))))
      extra = std::max(
          sizeof(FloatMathWorkspace),
          sleef_batch_kind(kind) ? sizeof(MathBatchWorkspace) : std::size_t{0});
    else if (sleef_batch_kind(kind) && profile != SequenceProfile::Strict)
      extra = sizeof(MathBatchWorkspace);
    static_assert(sizeof(Arithmetic) % alignof(FloatMathWorkspace) == 0);
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
  bool all_packed = true;
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
    all_packed = all_packed && dense;
  }
  const auto advance_coordinate = [&] {
    if (!all_packed) {
      for (std::size_t axis = shape.size(); axis; --axis) {
        if (++coordinate[axis - 1] < shape[axis - 1])
          break;
        coordinate[axis - 1] = 0;
      }
    }
  };
  const auto indexing_work = call.inputs.size() * shape.size() + 1;
  std::optional<input_internal::Float32Environment> environment;
  environment.emplace();
  if constexpr (std::is_same_v<Arithmetic, CertifiedMath>) {
    if ((kind == CertifiedKind::Exp ||
         trig_simd_kind(static_cast<unsigned>(kind))) &&
        profile != SequenceProfile::Strict &&
        descriptor.element_type == ElementType::Float32 &&
        accelerated_math_available()) {
      if (environment->active()) {
        const auto eligible = [kind](std::uint32_t bits) {
          return kind == CertifiedKind::Exp
                     ? (bits & UINT32_C(0x7fffffff)) <= UINT32_C(0x42a00000)
                     : trig_simd_domain(static_cast<unsigned>(kind), bits);
        };
        auto* block =
            new (scratch.data() + sizeof(Arithmetic)) FloatMathWorkspace;
        for (std::uint64_t offset = 0; offset < count;
             offset += FloatMathWorkspace::kCount) {
          const auto size = std::min<std::uint64_t>(FloatMathWorkspace::kCount,
                                                    count - offset);
          // Charge indexing here; arithmetic admission is charged exactly once
          // per lane, either below for a SIMD lane or by evaluate on fallback.
          status = consume(size * indexing_work);
          if (!status.ok())
            return Answer(status);
          if (packed[0]) {
            std::memcpy(block->bits.data(), packed[0] + offset * 4, size * 4);
          } else {
            const auto& input = call.inputs[0];
            const auto* const source = input.bytes().data();
            for (std::size_t lane = 0; lane < size; ++lane) {
              auto address = input.byte_address(coordinate);
              if (!address.ok())
                return Answer(address.status());
              std::memcpy(&block->bits[lane], source + address.value(), 4);
              advance_coordinate();
            }
          }
          std::memcpy(block->input.data(), block->bits.data(), size * 4);
          std::uint64_t rejected = 0;
          std::uint64_t fast_count = size;
          for (std::size_t lane = 0; lane < size; ++lane) {
            // Classify by bits before any FP instruction, including sNaN.
            if (!eligible(block->bits[lane])) {
              rejected |= UINT64_C(1) << lane;
              --fast_count;
              block->input[lane] = 0;
            }
          }
          status = consume(fast_count * DirectedInterval::kSlots *
                           DirectedInterval::kWords);
          if (!status.ok())
            return Answer(status);
          if (kind == CertifiedKind::Exp)
            exp_simd_f32(block->input.data(), block->output.data(), size);
          else
            trig_simd_f32(static_cast<unsigned>(kind), block->input.data(),
                          block->output.data(), size);
          // Preserve the admission/cancellation boundary at each block. The
          // output is unpublished until every rejected lane has been repaired.
          std::memcpy(output_data + offset * 4, block->output.data(), size * 4);
          while (rejected) {
            const auto lane = static_cast<unsigned>(__builtin_ctzll(rejected));
            rejected &= rejected - 1;
            auto calculated = arithmetic->evaluate(
                kind, descriptor.element_type, block->bits[lane], 0, consume,
                [] { return Status::success(); }, true);
            if (!calculated.ok())
              return Answer(calculated.status());
            const auto word = static_cast<std::uint32_t>(calculated.value());
            std::memcpy(output_data + (offset + lane) * 4, &word, 4);
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
        status = consume(size * indexing_work);
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
          advance_coordinate();
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
          std::memcpy(output_data + (offset + lane) * output_width, &word,
                      output_width);
        }
      }
      status = consume(1);
      return status.ok() ? std::move(output).publish() : Answer(status);
    }
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    status = consume(indexing_work);
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
    auto* destination = output_data + i * output_width;
    if (output_width == 8)
      std::memcpy(destination, &word, 8);
    else if (output_width == 4)
      std::memcpy(destination, &word, 4);
    else
      *destination = static_cast<std::uint8_t>(word);
    advance_coordinate();
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
    if (profile != SequenceProfile::Strict &&
        (kind == CertifiedKind::Exp ||
         trig_simd_kind(static_cast<unsigned>(kind))))
      traits.workspace_bytes += std::max(
          sizeof(FloatMathWorkspace),
          sleef_batch_kind(kind) ? sizeof(MathBatchWorkspace) : std::size_t{0});
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
