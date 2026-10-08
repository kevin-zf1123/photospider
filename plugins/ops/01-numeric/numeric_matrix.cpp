#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/accelerated_matrix.hpp"
#include "01-numeric/exact_dot.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
void matrix_candidates(numeric_ops::MatrixBlock* block, unsigned rows,
                       unsigned input_components, unsigned output_components) {
#if defined(PHOTOSPIDER_MATRIX_BACKEND_SME)
  if (numeric_ops::sme_matrix_candidates(block, rows, input_components,
                                         output_components))
    return;
#elif defined(PHOTOSPIDER_MATRIX_BACKEND_ACCELERATE)
  if (numeric_ops::accelerate_matrix_candidates(block, rows, input_components,
                                                output_components))
    return;
#endif
  numeric_ops::scalar_matrix_candidates(block, rows, input_components,
                                        output_components);
}
Result<ValueDescriptor> metadata(const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<ValueDescriptor>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto descriptor = [](const OperationMetadata& input) {
    const auto& tensor = input.result_schema->tensors[0];
    return ValueDescriptor{tensor.descriptor.element_type,
                           tensor.sample_shape()};
  };
  const auto vectors = descriptor(inputs[0]);
  const auto matrix = descriptor(inputs[1]);
  const auto bias = descriptor(inputs[2]);
  if (vectors.element_type != matrix.element_type ||
      vectors.element_type != bias.element_type ||
      (vectors.element_type != ElementType::Float32 &&
       vectors.element_type != ElementType::Float64))
    return mismatch("matrix transform requires identical Float32/64 inputs");
  if (vectors.shape.empty() || vectors.shape.size() > 8 ||
      matrix.shape.size() != 2 || bias.shape.size() != 1)
    return mismatch("invalid vector/matrix/bias rank");
  const auto input_components = vectors.shape.back(),
             output_components = matrix.shape[0];
  if (input_components < 2 || input_components > 4 || output_components < 2 ||
      output_components > 4 || matrix.shape[1] != input_components ||
      bias.shape[0] != output_components)
    return mismatch("matrix transform requires matching Cin/Cout in 2..4");
  ValueDescriptor output = vectors;
  output.shape.back() = output_components;
  std::uint64_t source_count = 1, target_count = 1;
  for (std::size_t j = 0; j < vectors.shape.size(); ++j) {
    if (!vectors.shape[j] ||
        vectors.shape[j] > (UINT64_C(1) << 40) / source_count ||
        output.shape[j] > (UINT64_C(1) << 40) / target_count)
      return mismatch("matrix vector count exceeds 2^40");
    source_count *= vectors.shape[j];
    target_count *= output.shape[j];
  }
  return Answer(std::move(output));
}
// Whole callbacks own one fixed scratch allocation, separately admitted from
// the full packed output. Input windows retain the authorized backing; the
// Whole relation needs no per-output dependency rows.
struct MatrixWorkspace final {
  numeric_ops::MatrixBlock block;
  numeric_ops::ExactDot arithmetic;
  explicit MatrixWorkspace(SequenceProfile profile) : arithmetic(profile) {}
};
struct MatrixKernel final {
  SequenceProfile profile;
  explicit MatrixKernel(SequenceProfile selected) : profile(selected) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed, "matrix requires one packed writer"};
    const auto& consume = phase.consume_work;
    auto status = consume(1);
    if (!status.ok())
      return status;
    const auto& vectors = phase.tensors->at({0, 0});
    const auto shape = vectors.spec().sample_shape();
    const auto input_components = static_cast<unsigned>(shape.back());
    const auto output_components = static_cast<unsigned>(
        phase.tensors->at({1, 0}).spec().sample_shape()[0]);
    ValueDescriptor descriptor{vectors.spec().descriptor.element_type, shape};
    descriptor.shape.back() = output_components;
    const auto width = Value::element_size(descriptor.element_type);
    const auto count = numeric_ops::math_take(vectors.spec().sample_count()) /
                       input_components;
    std::array<numeric_ops::MathTensorReader, 3> readers{
        numeric_ops::MathTensorReader(vectors, phase.query.cancellation),
        numeric_ops::MathTensorReader(phase.tensors->at({1, 0}),
                                      phase.query.cancellation),
        numeric_ops::MathTensorReader(phase.tensors->at({2, 0}),
                                      phase.query.cancellation)};
    numeric_ops::MathTensorWriter writer(writers[0]);
    auto storage = phase.allocator.allocate(sizeof(MatrixWorkspace));
    if (!storage.ok())
      return storage.status();
    auto scratch = storage.take_value();
    static_assert(alignof(MatrixWorkspace) <= alignof(std::max_align_t));
    // Byte-array allocations support fundamental alignment. Destroy the placed
    // object before releasing its owning buffer on success and on every
    // failure.
    std::unique_ptr<MatrixWorkspace, void (*)(MatrixWorkspace*)> workspace(
        new (scratch.data()) MatrixWorkspace(profile),
        [](MatrixWorkspace* value) { value->~MatrixWorkspace(); });
    auto& block = workspace->block;
    input_internal::Float32Environment environment;
    const bool fast = profile == SequenceProfile::AppleSilicon && width == 4 &&
                      environment.active();
    std::array<bool, 4> finite_matrix{};
    status = consume(output_components * (input_components + 1));
    if (!status.ok())
      return status;
    for (unsigned o = 0; o < output_components; ++o) {
      finite_matrix[o] = fast;
      for (unsigned j = 0; j < input_components; ++j) {
        block.matrix[o][j] = readers[1].bits({o, j});
        if (fast) {
          const auto value =
              numeric_ops::numeric_double(block.matrix[o][j], true);
          finite_matrix[o] &= std::isfinite(value);
          block.coefficients[o * input_components + j] =
              std::isfinite(value) ? value : 0;
        }
      }
      block.bias[o] = readers[2].bits({o});
      if (fast) {
        const auto value = numeric_ops::numeric_double(block.bias[o], true);
        finite_matrix[o] &= std::isfinite(value);
        block.offsets[o] = std::isfinite(value) ? value : 0;
      }
    }
    std::vector<std::uint64_t> coordinate(shape.size(), 0);
    for (std::uint64_t begin = 0; begin < count; begin += block.kRows) {
      const auto rows = static_cast<unsigned>(
          std::min<std::uint64_t>(block.kRows, count - begin));
      status = consume(
          rows * (input_components * shape.size() +
                  output_components * (fast ? 32 * input_components + 32 : 1)));
      if (!status.ok())
        return status;
      std::array<bool, numeric_ops::MatrixBlock::kRows> finite_vectors{};
      for (unsigned r = 0; r < rows; ++r) {
        finite_vectors[r] = fast;
        auto index = begin + r;
        for (std::size_t axis = shape.size() - 1; axis; --axis) {
          coordinate[axis - 1] = index % shape[axis - 1];
          index /= shape[axis - 1];
        }
        for (unsigned j = 0; j < input_components; ++j) {
          coordinate.back() = j;
          block.raw[r][j] = readers[0].bits(coordinate);
          if (fast) {
            const auto value =
                numeric_ops::numeric_double(block.raw[r][j], true);
            finite_vectors[r] &= std::isfinite(value);
            block.x[r * input_components + j] =
                std::isfinite(value) ? value : 0;
          }
        }
      }
      if (fast)
        matrix_candidates(&block, rows, input_components, output_components);
      if (phase.query.cancellation.cancelled())
        return Status{ErrorCode::Cancelled, {}};
      for (unsigned r = 0; r < rows; ++r) {
        auto index = begin + r;
        for (std::size_t axis = shape.size() - 1; axis; --axis) {
          coordinate[axis - 1] = index % shape[axis - 1];
          index /= shape[axis - 1];
        }
        for (unsigned o = 0; o < output_components; ++o) {
          std::optional<std::uint64_t> word;
          if (finite_vectors[r] && finite_matrix[o])
            word = numeric_ops::certify_matrix_float32(
                block, r, o, input_components, output_components);
          if (!word) {
            auto exact = workspace->arithmetic.evaluate(
                block.raw[r], block.matrix[o], block.bias[o], input_components,
                descriptor.element_type, consume);
            if (!exact.ok())
              return exact.status();
            word = exact.value();
          }
          coordinate.back() = o;
          std::memcpy(writer.address(coordinate), &*word, width);
        }
      }
    }
    if (phase.query.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return consume(1);
  }
};
using MatrixProgram = numeric_ops::WholeTensorProgram<MatrixKernel>;
OperationDefinition matrix_operation(const std::string& key,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(MatrixProgram));
  traits.workspace_bytes = sizeof(MatrixWorkspace);
  operation.specialize_metadata = [profile](const auto& inputs, const auto&)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(inputs);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    const auto& output = resolved.value();
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(output.element_type, output.shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<MatrixProgram>(allocator, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_matrix(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(matrix_operation(
        std::string("numeric.matrix_transform") + profile.first,
        profile.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
