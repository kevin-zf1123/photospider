#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_quantile.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "01-numeric/stable_order.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Status shape_error(const char* message) {
  return Status{ErrorCode::TypeMismatch,
                message,
                FailureReason::None,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}

Result<std::uint32_t> ordering_axis(
    const ValueDescriptor& descriptor,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<std::uint32_t>;
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    return Answer(shape_error("ordering requires rank 1..8"));
  std::uint64_t count = 1;
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      return Answer(shape_error("ordering exceeds 2^40 elements"));
    count *= extent;
  }
  const auto axis = std::get<std::int64_t>(parameters.at("axis"));
  if (axis < 0 || static_cast<std::uint64_t>(axis) >= descriptor.shape.size())
    return Answer(
        numeric_ops::array_parameter_error("ordering axis outside rank"));
  return Answer(static_cast<std::uint32_t>(axis));
}
struct OrderingState final {
  numeric_ops::StableOrderWorkspace ordering;
  numeric_ops::ExactQuantile arithmetic;
  explicit OrderingState(SequenceProfile profile) : arithmetic(profile) {}
};
struct OrderingKernel final {
  bool quantile;
  SequenceProfile profile;
  OrderingKernel(bool quantile, SequenceProfile profile)
      : quantile(quantile), profile(profile) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "ordering requires one packed writer"};
    const auto& input = phase.tensors->at({0, 0});
    const auto shape = input.spec().sample_shape();
    const auto type = input.spec().descriptor.element_type;
    const auto& output = phase.query.output.result_schema->tensors[0];
    const auto target = output.descriptor.element_type;
    const auto axis =
        math_take(ordering_axis({type, shape}, phase.query.parameters));
    const auto count = shape[axis];
    QuantilePosition position;
    if (quantile && count > 1) {
      MathTensorReader probability(phase.tensors->at({1, 0}),
                                   phase.query.cancellation);
      const auto bits = probability.bits({0});
      auto found = quantile_position(
          bits, phase.tensors->at({1, 0}).spec().descriptor.element_type,
          count);
      if (!found.ok()) {
        auto failure = found.status();
        std::array<char, 128> message{};
        std::snprintf(message.data(), message.size(),
                      "InvalidQuantileProbability: port=1 bits=0x%016" PRIx64
                      "; require finite q in [0,1]",
                      bits);
        failure.message = message.data();
        failure.detail.scope = FailureScope::Run;
        return failure;
      }
      position = found.value();
    }
    auto buffer = math_take(phase.allocator.allocate(sizeof(OrderingState)));
    std::unique_ptr<OrderingState, void (*)(OrderingState*)> state(
        new (buffer.data()) OrderingState(profile),
        [](OrderingState* item) { item->~OrderingState(); });
    MathTensorReader reader(input, phase.query.cancellation);
    MathTensorWriter writer(writers[0]);
    std::vector<uint64_t> coordinate(shape.size(), 0), source(shape.size(), 0);
    uint64_t lines = 1;
    for (size_t axis_index = 0; axis_index < shape.size(); ++axis_index)
      if (axis_index != axis)
        lines *= shape[axis_index];
    const auto& work = phase.consume_work;
    auto status = work(1);
    if (!status.ok())
      return status;
    const auto read = [&](uint64_t index, uint64_t* bits) {
      auto charged = work(shape.size() + 1);
      if (!charged.ok())
        return charged;
      source = coordinate;
      source[axis] = index;
      *bits = reader.bits(source);
      return Status::success();
    };
    for (std::uint64_t line = 0; line < lines; ++line) {
      auto permutation =
          state->ordering.build(count, type, profile, work, read);
      if (!permutation.ok())
        return (permutation.status());
      const auto& indices = permutation.value();
      const auto store = [&](std::uint64_t bits) {
        std::array<std::uint64_t, 4> replicas{};
        numeric_ops::select_words(replicas.data(), bits, bits, 1, profile);
        std::memcpy(writer.address(coordinate), replicas.data(),
                    Value::element_size(target));
        return work(1);
      };
      if (!quantile) {
        for (std::uint64_t j = 0; j < count; ++j) {
          coordinate[axis] = j;
          std::uint64_t bits = indices[j];
          if (phase.query.output_index == 0) {
            status = read(indices[j], &bits);
            if (!status.ok())
              return (status);
          }
          status = store(bits);
          if (!status.ok())
            return (status);
        }
      } else {
        std::uint64_t nan_begin = count;
        if (type == ElementType::Float32 || type == ElementType::Float64) {
          std::uint64_t low = 0, high = count;
          while (low < high) {
            const auto middle = low + (high - low) / 2;
            std::uint64_t bits = 0;
            status = read(indices[middle], &bits);
            if (!status.ok())
              return (status);
            if (numeric_ops::BinaryParts::decode(bits,
                                                 type == ElementType::Float32)
                    .nan)
              high = middle;
            else
              low = middle + 1;
          }
          nan_begin = low;
        }
        std::uint64_t bits = 0;
        if (nan_begin < count) {
          status = read(indices[nan_begin], &bits);
          if (!status.ok())
            return (status);
          bits = numeric_ops::converted_nan(bits, type, target);
        } else {
          std::uint64_t a = 0, b = 0;
          status = read(indices[position.index], &a);
          if (status.ok() && position.fractional())
            status = read(indices[position.index + 1], &b);
          if (!status.ok())
            return (status);
          auto calculated =
              state->arithmetic.finish(a, b, type, target, position, work);
          if (!calculated.ok())
            return (calculated.status());
          bits = calculated.value();
        }
        status = store(bits);
        if (!status.ok())
          return (status);
      }
      coordinate[axis] = 0;
      for (std::size_t j = shape.size(); j; --j)
        if (j - 1 != axis) {
          if (++coordinate[j - 1] < shape[j - 1])
            break;
          coordinate[j - 1] = 0;
        }
    }
    return work(1);
  }
};
using OrderingProgram = numeric_ops::WholeTensorProgram<OrderingKernel>;
OperationDefinition ordering_operation(const std::string& key, bool quantile,
                                       SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = quantile ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 15;
  }
  if (quantile) {
    traits.input_schema[1].rank = 1;
    traits.input_schema[1].element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.workspace_bytes = sizeof(OrderingState);
  traits.parameter_schema = {{"axis", OperationParameterType::Int64}};
  if (quantile)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  traits.outputs.resize(quantile ? 1 : 2);
  for (std::size_t j = 0; j < traits.outputs.size(); ++j) {
    numeric_ops::set_whole_tensor_output(
        traits, j ? ElementType::Int64 : ElementType::Float64,
        sizeof(OrderingProgram), j);
    traits.outputs[j].key = j ? "indices" : "values";
  }
  operation.specialize_metadata = [quantile, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& source = inputs[0].result_schema->tensors[0];
    const ValueDescriptor descriptor{source.descriptor.element_type,
                                     source.sample_shape()};
    auto axis = ordering_axis(descriptor, parameters);
    if (!axis.ok())
      return Answer(axis.status());
    auto shape = descriptor.shape;
    auto type = descriptor.element_type;
    if (quantile) {
      if (inputs[1].result_schema->tensors[0].sample_shape() !=
          std::vector<std::uint64_t>{1})
        return Answer(shape_error("quantile q requires scalar [1]"));
      const auto& dtype = std::get<std::string>(parameters.at("dtype"));
      if (dtype != "float32" && dtype != "float64")
        return Answer(numeric_ops::array_parameter_error(
            "quantile requires floating dtype"));
      type = dtype == "float32" ? ElementType::Float32 : ElementType::Float64;
      shape[axis.value()] = 1;
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> results(quantile ? 1 : 2);
    results[0].metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(type, shape));
    if (!quantile)
      results[1].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(
              numeric_ops::numeric_tensor_schema(ElementType::Int64, shape));
    if (quantile && descriptor.shape[axis.value()] == 1)
      results[0].input_indices = std::vector<std::uint32_t>{0};
    return Answer(std::move(results));
  };
  operation.start_result = [quantile, profile](const auto&,
                                               const auto& allocator) {
    return ResultContinuation::make<OrderingProgram>(allocator, quantile,
                                                     profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_ordering(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool quantile : {false, true}) {
      auto status = registry->register_operation(ordering_operation(
          std::string(quantile ? "numeric.quantile" : "array.sort") +
              profile.first,
          quantile, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
