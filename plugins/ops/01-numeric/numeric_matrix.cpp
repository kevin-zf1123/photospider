#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/accelerated_matrix.hpp"
#include "01-numeric/exact_dot.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

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
Result<TensorDescriptor> metadata(
    const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<TensorDescriptor>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  if (inputs.size() != 3)
    return mismatch("matrix transform requires three inputs");
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
  }
  const auto descriptor = [](const OperationMetadata& input) {
    const auto& tensor = input.result_schema->tensors[0];
    return TensorDescriptor{tensor.descriptor.element_type,
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
  TensorDescriptor output = vectors;
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
// Mutable arithmetic and batch buffers belong to preallocated invocation
// scratch.
struct MatrixWorkspace final {
  numeric_ops::MatrixBlock block;
  numeric_ops::ExactDot arithmetic;
  explicit MatrixWorkspace(SequenceProfile profile) : arithmetic(profile) {}
};
std::array<Region, 3> input_regions(const Region& output,
                                    std::uint64_t input_components) {
  auto dimensions = output.dimensions();
  const auto channels = dimensions.back();
  dimensions.back() = {0, input_components};
  return {Region(std::move(dimensions)),
          Region({channels, {0, input_components}}), Region({channels})};
}
struct MatrixState final {
  SequenceProfile profile;
  unsigned input_components = 0;
  fixed_tensor::PointInputs<3> inputs;
};
void vector_coordinate(std::array<std::uint64_t, 8>* at, std::uint64_t index,
                       const ps_kernel_layout_v8& layout) noexcept {
  for (unsigned axis = layout.rank - 1; axis; --axis) {
    (*at)[axis - 1] = layout.origin[axis - 1] + index % layout.shape[axis - 1];
    index /= layout.shape[axis - 1];
  }
}
void matrix_invoke(const ps_kernel_invocation_v8* invocation,
                   ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state =
      *static_cast<const MatrixState*>(invocation->prepared_context);
  static_assert(alignof(MatrixWorkspace) <= alignof(std::max_align_t));
  try {
    auto* scratch =
        new (invocation->scratch[0].allocation) MatrixWorkspace(state.profile);
    const auto destroy = [](MatrixWorkspace* value) {
      value->~MatrixWorkspace();
    };
    std::unique_ptr<MatrixWorkspace, decltype(destroy)> guard(scratch, destroy);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    fixed_tensor::PointReader<3> readers{state.inputs, *invocation};
    const auto& output = invocation->outputs[0];
    const auto& layout = *output.layout;
    const auto dtype = static_cast<ElementType>(layout.element_type);
    const auto width = TensorSpan::element_size(dtype);
    const unsigned input_components = state.input_components,
                   output_components =
                       static_cast<unsigned>(layout.shape[layout.rank - 1]);
    const auto first_channel = layout.origin[layout.rank - 1];
    std::uint64_t count = 1;
    for (unsigned axis = 0; axis + 1 < layout.rank; ++axis)
      count *= layout.shape[axis];
    auto& block = scratch->block;
    const bool fast =
        state.profile == SequenceProfile::AppleSilicon && width == 4;
    std::array<bool, 4> finite_matrix{};
    for (unsigned o = 0; o < output_components; ++o) {
      finite_matrix[o] = fast;
      for (unsigned j = 0; j < input_components; ++j) {
        block.matrix[o][j] = readers.bits(1, {first_channel + o, j}, width);
        if (fast) {
          const auto value =
              numeric_ops::numeric_double(block.matrix[o][j], true);
          finite_matrix[o] &= std::isfinite(value);
          block.coefficients[o * input_components + j] =
              std::isfinite(value) ? value : 0;
        }
      }
      block.bias[o] = readers.bits(2, {first_channel + o}, width);
      if (fast) {
        const auto value = numeric_ops::numeric_double(block.bias[o], true);
        finite_matrix[o] &= std::isfinite(value);
        block.offsets[o] = std::isfinite(value) ? value : 0;
      }
    }
    std::array<std::uint64_t, 8> at{};
    for (std::uint64_t begin = 0; begin < count; begin += block.kRows) {
      auto cancelled = consume(1);
      if (!cancelled.ok()) {
        *status = kernel_failure(cancelled.code);
        return;
      }
      const auto rows = static_cast<unsigned>(
          std::min<std::uint64_t>(block.kRows, count - begin));
      std::array<bool, numeric_ops::MatrixBlock::kRows> finite_vectors{};
      for (unsigned r = 0; r < rows; ++r) {
        finite_vectors[r] = fast;
        vector_coordinate(&at, begin + r, layout);
        for (unsigned j = 0; j < input_components; ++j) {
          at[layout.rank - 1] = j;
          block.raw[r][j] = readers.bits(0, at, width);
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
      for (unsigned r = 0; r < rows; ++r) {
        for (unsigned o = 0; o < output_components; ++o) {
          std::optional<std::uint64_t> word;
          if (finite_vectors[r] && finite_matrix[o])
            word = numeric_ops::certify_matrix_float32(
                block, r, o, input_components, output_components);
          if (!word) {
            auto exact = scratch->arithmetic.evaluate(
                block.raw[r], block.matrix[o], block.bias[o], input_components,
                dtype, consume);
            if (!exact.ok()) {
              *status = kernel_failure(exact.status().code,
                                       exact.status().message.c_str(),
                                       exact.status().reason);
              return;
            }
            word = exact.value();
          }
          std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                          output.byte_offset +
                          ((begin + r) * output_components + o) * width,
                      &*word, width);
        }
      }
    }
    const auto settled = consume(1);
    *status =
        settled.ok() ? ps_kernel_status_v8{} : kernel_failure(settled.code);
  } catch (const Status& failure) {
    *status =
        kernel_failure(failure.code, failure.message.c_str(), failure.reason);
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "matrix arithmetic failed");
  }
}
Result<OperationProgram> lower_matrix(const OperationLowering& query) {
  OperationProgramBuilder builder(query);
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Result<OperationProgram>(output.status());
  std::array<TileGrid, 3> inputs;
  for (unsigned port = 0; port < 3; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Result<OperationProgram>(grid.status());
    inputs[port] = grid.take_value();
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task("matrix", "exact-dot-v1", {},
                                   {sizeof(MatrixWorkspace), 0, 0});
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Result<OperationProgram>(region.status());
      const auto regions = input_regions(
          region.value(), inputs[0].shape()[inputs[0].rank() - 1]);
      for (unsigned port = 0; port < 3; ++port) {
        auto reads = sdk::tiles_for_region(budget, inputs[port], regions[port]);
        if (!reads.ok())
          return Result<OperationProgram>(reads.status());
        for (auto index : reads.value()) {
          auto status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), port, 0, index);
          if (!status.ok())
            return Result<OperationProgram>(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
  return builder.finish();
}
OperationDefinition matrix_operation(const std::string& key,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema) {
    input.element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
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
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = output;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = lower_matrix;
  operation.prepare_kernel =
      [profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != "matrix" || kernel.contract != "exact-dot-v1" ||
        query.inputs.size() != 3 || query.outputs.size() != 1 ||
        query.writes.size() != 1 || !query.task_parameters.empty() ||
        query.scratch !=
            std::array<std::uint64_t, 3>{sizeof(MatrixWorkspace), 0, 0})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid matrix kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "matrix requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto state = std::make_unique<MatrixState>();
    state->profile = profile;
    state->input_components =
        query.inputs[0].result_schema->tensors[0].sample_shape().back();
    const auto regions =
        input_regions(fixed_tensor::view_region(query.writes[0].layout),
                      state->input_components);
    auto valid =
        fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
    if (!valid.ok())
      return Answer(valid);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.invoke_cpu = matrix_invoke;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const MatrixState*>(context);
    };
    return Answer(result);
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
