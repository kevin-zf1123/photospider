#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

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
             const std::map<std::string, ParameterValue>& parameters) {
    const auto x = BinaryParts::decode(a, narrow),
               y = BinaryParts::decode(b, narrow);
    if (x.nan || y.nan)
      return false;
    if (x.infinite || y.infinite)
      return x.infinite && y.infinite && x.negative == y.negative;
    std::uint64_t absolute_bits = 0, relative_bits = 0;
    const auto absolute = std::get<double>(parameters.at("atol"));
    const auto relative = std::get<double>(parameters.at("rtol"));
    std::memcpy(&absolute_bits, &absolute, 8);
    std::memcpy(&relative_bits, &relative, 8);
    const auto high = x.magnitude >= y.magnitude ? x : y;
    const auto low = x.magnitude >= y.magnitude ? y : x;
    difference.set(high);
    temporary.set(low);
    if (x.negative != y.negative)
      difference.add(temporary);
    else
      difference.subtract(temporary);
    threshold.set(BinaryParts::decode(absolute_bits, false));
    product.set_product(BinaryParts::decode(relative_bits, false), high);
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
  void evaluate(ElementType type, unsigned count,
                const std::map<std::string, ParameterValue>& parameters,
                std::uint8_t* output) {
    if (kind == Comparison::IsClose) {
      for (unsigned i = 0; i < count; ++i)
        output[i] =
            close(left[i], right[i], type == ElementType::Float32, parameters);
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
struct ComparisonKernel final {
  Comparison kind;
  SequenceProfile profile;
  ComparisonKernel(Comparison kind, SequenceProfile profile)
      : kind(kind), profile(profile) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    const auto& spec = phase.query.inputs[0].result_schema->tensors[0];
    const auto shape = spec.sample_shape();
    const auto count = math_take(spec.sample_count());
    auto memory = math_take(phase.allocator.allocate(sizeof(ComparisonMath)));
    auto* arithmetic = new (memory.data()) ComparisonMath(kind, profile);
    const auto destroy = [](ComparisonMath* value) {
      value->~ComparisonMath();
    };
    std::unique_ptr<ComparisonMath, decltype(destroy)> guard(arithmetic,
                                                             destroy);
    MathTensorReader first(phase.tensors->at({0, 0}), phase.query.cancellation),
        second(phase.tensors->at({1, 0}), phase.query.cancellation);
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "comparison requires one packed writer"};
    MathTensorWriter writer(writers[0]);
    std::vector<uint64_t> at(shape.size(), 0);
    for (uint64_t begin = 0; begin < count; begin += 4) {
      const auto lanes =
          static_cast<unsigned>(std::min<uint64_t>(4, count - begin));
      auto charged = phase.consume_work(
          lanes *
          (2 * shape.size() + (kind == Comparison::IsClose ? 1024 : 16)));
      if (!charged.ok())
        return charged;
      std::array<uint8_t*, 4> destinations{};
      std::array<uint8_t, 4> answers{};
      for (unsigned lane = 0; lane < lanes; ++lane) {
        arithmetic->left[lane] = first.bits(at);
        arithmetic->right[lane] = second.bits(at);
        destinations[lane] = writer.address(at);
        math_next(at, shape);
      }
      arithmetic->evaluate(spec.descriptor.element_type, lanes,
                           phase.query.parameters, answers.data());
      for (unsigned lane = 0; lane < lanes; ++lane)
        *destinations[lane] = answers[lane];
    }
    return phase.consume_work(1);
  }
};
using ComparisonProgram = numeric_ops::WholeTensorProgram<ComparisonKernel>;
OperationDefinition comparison(const std::string& key, Comparison kind,
                               SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = kind == Comparison::IsClose ? 12 : 15;
  }
  if (kind == Comparison::IsClose)
    traits.parameter_schema = {{"atol", OperationParameterType::Float64},
                               {"rtol", OperationParameterType::Float64}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::UInt8,
                                       sizeof(ComparisonProgram));
  traits.workspace_bytes = sizeof(ComparisonMath);
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
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
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(ElementType::UInt8, shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [kind, profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<ComparisonProgram>(allocator, kind,
                                                       profile);
  };
  return operation;
}
struct SelectionKernel final {
  explicit SelectionKernel(SequenceProfile) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    const auto& output = phase.query.output.result_schema->tensors[0];
    const auto& shape = output.descriptor.shape;
    const auto count = math_take(output.sample_count());
    const auto width = Value::element_size(output.descriptor.element_type);
    std::array<MathTensorReader, 3> inputs{
        MathTensorReader(phase.tensors->at({0, 0}), phase.query.cancellation),
        MathTensorReader(phase.tensors->at({1, 0}), phase.query.cancellation),
        MathTensorReader(phase.tensors->at({2, 0}), phase.query.cancellation)};
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed,
              "selection requires one packed writer"};
    MathTensorWriter writer(writers[0]);
    std::vector<uint64_t> at(shape.size(), 0);
    for (uint64_t i = 0; i < count; ++i) {
      auto charged = phase.consume_work(32 + width + 3 * shape.size());
      if (!charged.ok())
        return charged;
      // Both complete branches remain required even for a constant condition.
      const std::array<uint64_t, 3> bits{inputs[0].bits(at), inputs[1].bits(at),
                                         inputs[2].bits(at)};
      if (bits[0] > 1)
        return {ErrorCode::InvalidArgument,
                "InvalidCondition: port=0 byte=" + std::to_string(bits[0]) +
                    " linear_index=" + std::to_string(i),
                FailureReason::InvalidDomain,
                {FailureOrigin::Domain, FailureScope::Run}};
      const auto selected = bits[0] ? bits[1] : bits[2];
      std::memcpy(writer.address(at), &selected, width);
      math_next(at, shape);
    }
    return phase.consume_work(1);
  }
};
using SelectionProgram = numeric_ops::WholeTensorProgram<SelectionKernel>;
OperationDefinition selection(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 15;
  }
  traits.input_schema[0].element_type_mask = 1;
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(SelectionProgram));
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
    result.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(left.descriptor.element_type,
                                           shape));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_result = [profile](const auto&, const auto& allocator) {
    return ResultContinuation::make<SelectionProgram>(allocator, profile);
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
