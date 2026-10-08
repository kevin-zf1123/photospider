#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_shaper.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct ShaperState {
  numeric_ops::ExactShaper arithmetic;
  explicit ShaperState(SequenceProfile profile) : arithmetic(profile) {}
};
struct ShaperPrepared final {
  bool inverse;
  SequenceProfile profile;
};
struct ShaperKernel final {
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    const auto& prepared =
        *static_cast<const ShaperPrepared*>(phase.query.prepared->state());
    const auto& tensor = phase.query.output.result_schema->tensors[0];
    const auto shape = tensor.sample_shape();
    const bool narrow = tensor.descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    std::array<MathTensorReader, 3> inputs{
        MathTensorReader(phase.tensors->at({0, 0}), phase.query.cancellation),
        MathTensorReader(phase.tensors->at({1, 0}), phase.query.cancellation),
        MathTensorReader(phase.tensors->at({2, 0}), phase.query.cancellation)};
    std::array<std::uint64_t, 2> bounds{};
    std::array<BinaryParts, 2> parts{};
    const auto invalid = [&](unsigned i) {
      return Status{ErrorCode::InvalidArgument,
                    "InvalidBounds: port=" + std::to_string(i + 1) +
                        " bits=" + std::to_string(bounds[i]),
                    FailureReason::InvalidDomain,
                    {FailureOrigin::Domain, FailureScope::Run}};
    };
    for (unsigned i = 0; i < 2; ++i) {
      math_require(phase.consume_work(2));
      bounds[i] = inputs[i + 1].bits({0});
      parts[i] = BinaryParts::decode(bounds[i], narrow);
    }
    for (unsigned i = 0; i < 2; ++i)
      if (parts[i].nan || parts[i].infinite || parts[i].negative ||
          !parts[i].magnitude)
        return invalid(i);
    if (parts[0].order_key() >= parts[1].order_key())
      return invalid(0);
    auto scratch = math_take(phase.allocator.allocate(sizeof(ShaperState)));
    static_assert(alignof(ShaperState) <= alignof(std::max_align_t));
    std::unique_ptr<ShaperState, void (*)(ShaperState*)> state(
        new (scratch.data()) ShaperState(prepared.profile),
        [](auto* value) { value->~ShaperState(); });
    MathTensorWriter writer(writers[0]);
    std::vector<std::uint64_t> at(shape.size(), 0);
    for (std::uint64_t i = 0, count = math_take(tensor.sample_count());
         i < count; ++i) {
      math_require(phase.consume_work(at.size() + 1));
      const auto input = inputs[0].bits(at);
      auto result = state->arithmetic.evaluate(
          input, bounds[0], bounds[1], prepared.inverse, narrow,
          phase.consume_work, [] { return Status::success(); });
      if (!result.ok())
        return result.status();
      const auto bits = result.value();
      std::memcpy(writer.address(at), &bits, width);
      math_next(at, shape);
    }
    return phase.consume_work(1);
  }
};
using ShaperProgram = numeric_ops::WholeTensorProgram<ShaperKernel>;
OperationDefinition operation(const std::string& key, bool inverse,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 12;
  }
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(ShaperProgram));
  traits.workspace_bytes = sizeof(ShaperState);
  traits.requires_metadata_specialization = true;
  result.prepare_static = [inverse, profile](const auto& inputs, const auto&) {
    using Answer = Result<OperationPreparation>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 3)
      return Answer(mismatch("shaper requires three Result inputs"));
    for (const auto& input : inputs)
      if (!input.result_schema || !input.result_schema->fields.empty() ||
          input.result_schema->tensors.size() != 1)
        return Answer(mismatch("shaper requires one tensor per Result"));
    const auto& tensor = inputs[0].result_schema->tensors[0];
    const ValueDescriptor first{tensor.descriptor.element_type,
                                tensor.sample_shape()};
    if (first.shape.empty() || first.shape.size() > 8)
      return Answer(mismatch("shaper requires rank-1..8 input and two bounds"));
    const auto type = first.element_type;
    if (type != ElementType::Float32 && type != ElementType::Float64)
      return Answer(mismatch("shaper requires Float32/64"));
    for (unsigned i = 1; i < 3; ++i)
      if (inputs[i].result_schema->tensors[0].descriptor.element_type != type ||
          inputs[i].result_schema->tensors[0].sample_shape() !=
              std::vector<std::uint64_t>{1})
        return Answer(
            mismatch("shaper bounds require matching dtype and shape [1]"));
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(mismatch("shaper logical product exceeds 2^40 values"));
      count *= extent;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(type, first.shape));
    OperationPreparation prepared;
    prepared.outputs.push_back(std::move(resolved));
    prepared.state = std::make_shared<const ShaperPrepared>(
        ShaperPrepared{inverse, profile});
    return Answer(std::move(prepared));
  };
  result.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<ShaperProgram>(allocator);
  };
  return result;
}
}  // namespace
Status register_log_shapers(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool inverse : {false, true}) {
      auto status = registry->register_operation(
          operation(std::string(inverse ? "curve.log2_shaper_inverse"
                                        : "curve.log2_shaper") +
                        profile.first,
                    inverse, profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
