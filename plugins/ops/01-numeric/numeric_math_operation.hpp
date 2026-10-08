#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
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
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "photospider/plugin/result_program.hpp"

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
                          const input_internal::Float32Environment& env) {
  const auto width = Value::element_size(dtype);
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

template <class Arithmetic, class Kind>
struct PointMathKernel final {
  Kind kind;
  SequenceProfile profile;
  PointMathKernel(Kind kind, SequenceProfile profile)
      : kind(kind), profile(profile) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    const auto& descriptor =
        phase.query.output.result_schema->tensors[0].descriptor;
    const auto count =
        math_take(phase.query.output.result_schema->tensors[0].sample_count());
    auto memory = math_take(phase.allocator.allocate(
        sizeof(Arithmetic) + sizeof(MathBatchWorkspace) +
        sizeof(FloatMathWorkspace)));
    static_assert(sizeof(Arithmetic) % alignof(MathBatchWorkspace) == 0);
    static_assert(alignof(Arithmetic) <= alignof(std::max_align_t));
    auto* arithmetic = new (memory.data()) Arithmetic(profile);
    const auto destroy = [](Arithmetic* value) { value->~Arithmetic(); };
    std::unique_ptr<Arithmetic, decltype(destroy)> guard(arithmetic, destroy);
    auto* block = new (memory.data() + sizeof(Arithmetic)) MathBatchWorkspace;
    auto* fast =
        new (memory.data() + sizeof(Arithmetic) + sizeof(MathBatchWorkspace))
            FloatMathWorkspace;
    MathTensorReader first(phase.tensors->at({0, 0}), phase.query.cancellation);
    std::optional<MathTensorReader> second;
    if (phase.query.inputs.size() == 2)
      second.emplace(phase.tensors->at({1, 0}), phase.query.cancellation);
    if (writers.size() != 1)
      return Status{ErrorCode::OperationFailed,
                    "numeric writer requires one packed window"};
    const auto& writer = writers[0];
    std::vector<std::uint64_t> at(descriptor.shape.size(), 0);
    ResultTensorMutableRun row;
    std::uint64_t row_start = 0, row_left = 0;
    input_internal::Float32Environment environment;
    for (std::uint64_t offset = 0; offset < count;
         offset += MathBatchWorkspace::kCount) {
      const auto size =
          std::min<std::uint64_t>(MathBatchWorkspace::kCount, count - offset);
      auto charged = phase.consume_work(
          size * (phase.query.inputs.size() * at.size() + 1));
      if (!charged.ok())
        return charged;
      for (std::size_t lane = 0; lane < size; ++lane) {
        block->bits[lane] = {first.bits(at), second ? second->bits(at) : 0};
        if (!row_left) {
          row = math_take(writer.row_run(at));
          row_start = at.back();
          row_left = row.samples;
        }
        block->destination[lane] =
            row.data + static_cast<std::ptrdiff_t>(
                           static_cast<__int128>(at.back() - row_start) *
                           row.sample_stride_bytes);
        --row_left;
        for (std::size_t axis = at.size(); axis-- > 0;) {
          if (++at[axis] < descriptor.shape[axis])
            break;
          at[axis] = 0;
        }
      }
      auto calculated = compute_math_batch(
          *arithmetic, kind, profile, descriptor.element_type, *block, *fast,
          size, phase.consume_work, environment);
      if (!calculated.ok())
        return calculated;
    }
    return phase.consume_work(1);
  }
};

template <class Arithmetic, class Kind>
OperationDefinition point_math_operation(const std::string& key, Kind kind,
                                         SequenceProfile profile,
                                         unsigned ports, unsigned dtype_mask,
                                         bool rational = false) {
  using Program = WholeTensorProgram<PointMathKernel<Arithmetic, Kind>>;
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = ports;
  traits.input_schema.resize(ports);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = dtype_mask;
  }
  traits.requires_metadata_specialization = true;
  if (rational)
    traits.parameter_schema = {{"dtype", OperationParameterType::String}};
  set_whole_tensor_output(traits, ElementType::Float64, sizeof(Program));
  traits.workspace_bytes = sizeof(Arithmetic) + sizeof(MathBatchWorkspace) +
                           sizeof(FloatMathWorkspace);
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
  operation.start_result = [kind, profile](const ResultProgramQuery&,
                                           const BufferAllocator& allocator) {
    return ResultContinuation::make<Program>(allocator, kind, profile);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
