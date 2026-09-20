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
  const auto& vectors = inputs[0].descriptor;
  const auto& matrix = inputs[1].descriptor;
  const auto& bias = inputs[2].descriptor;
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
// the full packed output. No per-output dependency rows or retained sources.
struct MatrixWorkspace final {
  numeric_ops::MatrixBlock block;
  numeric_ops::ExactDot arithmetic;
  explicit MatrixWorkspace(SequenceProfile profile) : arithmetic(profile) {}
};
Status read_word(const Value& value, const std::vector<std::uint64_t>& at,
                 std::size_t width, std::uint64_t* word) {
  auto address = value.byte_address(at);
  if (!address.ok())
    return address.status();
  *word = 0;
  std::memcpy(word, value.bytes().data() + address.value(), width);
  return Status::success();
}
// Whole storage has already been validated by Value/registry. A single checked
// origin address suffices for a packed view, including nonzero storage origins.
const std::uint8_t* dense_data(const Value& value, std::size_t width) {
  const auto& shape = value.descriptor().shape;
  std::uint64_t stride = width;
  for (std::size_t axis = shape.size(); axis; --axis) {
    if (shape[axis - 1] > 1 && value.layout().byte_strides[axis - 1] !=
                                   static_cast<std::int64_t>(stride))
      return nullptr;
    stride *= shape[axis - 1];
  }
  auto base = value.byte_address(std::vector<std::uint64_t>(shape.size(), 0));
  return base.ok() ? value.bytes().data() + base.value() : nullptr;
}
Result<Value> execute_matrix(const OperationInvocation& call,
                             SequenceProfile profile) {
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
  const auto& vectors = call.inputs[0];
  const auto& shape = vectors.descriptor().shape;
  const auto input_components = static_cast<unsigned>(shape.back());
  const auto output_components =
      static_cast<unsigned>(call.inputs[1].descriptor().shape[0]);
  auto descriptor = vectors.descriptor();
  descriptor.shape.back() = output_components;
  const auto width = Value::element_size(descriptor.element_type);
  const auto count =
      vectors.region().element_count().value() / input_components;
  auto allocated =
      MutableValue::allocate(descriptor, call.output_region, call.allocator);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  auto storage = call.allocator.allocate(sizeof(MatrixWorkspace));
  if (!storage.ok())
    return Answer(storage.status());
  auto scratch = storage.take_value();
  static_assert(alignof(MatrixWorkspace) <= alignof(std::max_align_t));
  // Byte-array allocations support fundamental alignment. Destroy the placed
  // object before releasing its owning buffer on success and on every failure.
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
    return Answer(status);
  for (unsigned o = 0; o < output_components; ++o) {
    finite_matrix[o] = fast;
    for (unsigned j = 0; j < input_components; ++j) {
      status = read_word(call.inputs[1], {o, j}, width, &block.matrix[o][j]);
      if (!status.ok())
        return Answer(status);
      if (fast) {
        const auto value =
            numeric_ops::numeric_double(block.matrix[o][j], true);
        finite_matrix[o] &= std::isfinite(value);
        block.coefficients[o * input_components + j] =
            std::isfinite(value) ? value : 0;
      }
    }
    status = read_word(call.inputs[2], {o}, width, &block.bias[o]);
    if (!status.ok())
      return Answer(status);
    if (fast) {
      const auto value = numeric_ops::numeric_double(block.bias[o], true);
      finite_matrix[o] &= std::isfinite(value);
      block.offsets[o] = std::isfinite(value) ? value : 0;
    }
  }
  const auto* packed = dense_data(vectors, width);
  std::vector<std::uint64_t> coordinate(shape.size(), 0);
  for (std::uint64_t begin = 0; begin < count; begin += block.kRows) {
    const auto rows = static_cast<unsigned>(
        std::min<std::uint64_t>(block.kRows, count - begin));
    status = consume(
        rows * (input_components * shape.size() +
                output_components * (fast ? 32 * input_components + 32 : 1)));
    if (!status.ok())
      return Answer(status);
    std::array<bool, numeric_ops::MatrixBlock::kRows> finite_vectors{};
    for (unsigned r = 0; r < rows; ++r) {
      finite_vectors[r] = fast;
      if (!packed) {
        auto index = begin + r;
        for (std::size_t axis = shape.size() - 1; axis; --axis) {
          coordinate[axis - 1] = index % shape[axis - 1];
          index /= shape[axis - 1];
        }
      }
      for (unsigned j = 0; j < input_components; ++j) {
        if (packed) {
          block.raw[r][j] = 0;
          std::memcpy(&block.raw[r][j],
                      packed + ((begin + r) * input_components + j) * width,
                      width);
        } else {
          coordinate.back() = j;
          status = read_word(vectors, coordinate, width, &block.raw[r][j]);
          if (!status.ok())
            return Answer(status);
        }
        if (fast) {
          const auto value = numeric_ops::numeric_double(block.raw[r][j], true);
          finite_vectors[r] &= std::isfinite(value);
          block.x[r * input_components + j] = std::isfinite(value) ? value : 0;
        }
      }
    }
    if (fast)
      matrix_candidates(&block, rows, input_components, output_components);
    if (call.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    for (unsigned r = 0; r < rows; ++r)
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
            return Answer(exact.status());
          word = exact.value();
        }
        std::memcpy(
            output.data() + ((begin + r) * output_components + o) * width,
            &*word, width);
      }
  }
  if (call.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  return std::move(output).publish();
}
OperationDefinition matrix_operation(const std::string& key,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {2};
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
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
    result.metadata.descriptor = resolved.take_value();
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.callback = [profile](const OperationInvocation& call) {
    return execute_matrix(call, profile);
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
