#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_curve.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "01-numeric/uniform_axis.hpp"
#include "photospider/core/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct LutPoint {
  std::uint64_t query = 0;
  unsigned first = 0, count = 0;
};
struct LutState final {
  SequenceProfile profile;
  bool channels;
  unsigned policy;
  const ResultProgramPhase& phase;
  std::array<std::optional<numeric_ops::MathTensorReader>, 3> readers;
  std::function<Status(std::uint64_t)> consume;
  std::vector<std::uint64_t> shape, at, table_at;
  numeric_ops::UniformAxis axis;
  numeric_ops::ExactCurve arithmetic;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  LutState(SequenceProfile selected, bool multi,
           const ResultProgramPhase& invocation)
      : profile(selected),
        channels(multi),
        policy(std::get<std::string>(
                   invocation.query.parameters.at("out_of_domain")) == "reject"
                   ? 0U
               : std::get<std::string>(
                     invocation.query.parameters.at("out_of_domain")) == "clamp"
                   ? 1U
                   : 2U),
        phase(invocation),
        consume([this](auto amount) { return work(amount); }),
        shape(invocation.query.inputs[0]
                  .result_schema->tensors[0]
                  .sample_shape()),
        at(shape.size(), 0),
        table_at(multi ? 2 : 1, 0),
        axis(selected),
        arithmetic(selected) {
    axis.knots = ResourceVector<std::uint64_t>(
        ResourceAllocator<std::uint64_t>(phase.resources));
    for (unsigned port = 0; port < readers.size(); ++port)
      readers[port].emplace(phase.tensors->at({port, 0}),
                            phase.query.cancellation);
  }
  Status work(std::uint64_t amount) const { return phase.consume_work(amount); }
  void advance() {
    for (auto i = at.size(); i; --i) {
      if (++at[i - 1] < shape[i - 1])
        break;
      at[i - 1] = 0;
    }
  }
  Status failure(const std::string& message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Run}};
    return result;
  }
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& coordinate) {
    auto charged = work(coordinate.size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto& input = phase.tensors->at({port, 0});
    const bool narrow =
        input.spec().descriptor.element_type == ElementType::Float32;
    const auto bits = readers[port]->bits(coordinate);
    auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite) {
      std::string message =
          "nonfinite LUT1D port=" + std::to_string(port) + " coordinate=";
      for (auto index : coordinate)
        message += std::to_string(index) + ",";
      return Result<std::uint64_t>(failure(message));
    }
    return Result<std::uint64_t>(numeric_ops::ExactBezier::widen(bits, narrow));
  }
  Status classify(LutPoint* point) {
    auto value = read(0, at);
    if (!value.ok())
      return value.status();
    point->query = value.value();
    const auto key = axis.key(point->query);
    const auto size = static_cast<unsigned>(axis.knots.size());
    unsigned lo = 0, hi = size;
    while (lo < hi) {
      auto work = consume(1);
      if (!work.ok())
        return work;
      const auto mid = lo + (hi - lo) / 2;
      if (axis.key(axis.knots[mid]) < key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < size && axis.key(axis.knots[lo]) == key) {
      point->first = lo;
      point->count = 1;
    } else if (!lo || lo == size) {
      if (!policy)
        return failure("LUT1D query outside axis domain");
      if (policy == 1 || size == 1) {
        point->first = lo ? size - 1 : 0;
        point->count = 1;
      } else {
        point->first = lo ? size - 2 : 0;
        point->count = 2;
      }
    } else {
      point->first = lo - 1;
      point->count = 2;
    }
    return Status::success();
  }
  Status execute(const ResourceVector<ResultTensorWriteWindow>& writers) {
    std::array<std::uint64_t, 3> axis_values{};
    for (unsigned j = 0; j < 3; ++j) {
      auto value = read(2, {j});
      if (!value.ok())
        return value.status();
      axis_values[j] = value.value();
    }
    input_internal::Float32Environment environment;
    axis.sampling.environment_established = environment.active();
    auto status = axis.validate(
        axis_values,
        phase.query.inputs[1].result_schema->tensors[0].sample_shape()[0],
        consume);
    if (!status.ok())
      return status.code == ErrorCode::OperationFailed
                 ? failure(status.message, status.reason)
                 : status;
    const auto total =
        phase.query.output.result_schema->tensors[0].sample_count().value();
    // Preserve complete query rejection before table arithmetic without point
    // records.
    for (std::uint64_t i = 0; i < total; ++i, advance()) {
      auto query = read(0, at);
      if (!query.ok())
        return query.status();
      auto key = axis.key(query.value());
      if (!policy && (key < axis.key(axis.knots.front()) ||
                      key > axis.key(axis.knots.back())))
        return failure("LUT1D query outside axis domain");
    }
    numeric_ops::MathTensorWriter output(writers[0]);
    const bool narrow =
        phase.query.output.result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    for (std::uint64_t i = 0; i < total; ++i, advance()) {
      LutPoint point;
      status = classify(&point);
      if (!status.ok())
        return status;
      if (channels)
        table_at[1] = at.back();
      for (unsigned j = 0; j < point.count; ++j) {
        table_at[0] = point.first + j;
        auto value = read(1, table_at);
        if (!value.ok())
          return value.status();
        x[j] = axis.knots[point.first + j];
        y[j] = value.value();
      }
      // The exact linear evaluator requires a positive denominator.
      if (point.count == 2 && axis.descending) {
        std::swap(x[0], x[1]);
        std::swap(y[0], y[1]);
      }
      auto value = arithmetic.evaluate(
          false, 2, 0, point.count, 0, point.count == 1 ? 0 : -1, point.query,
          x, y, narrow, consume, {}, environment.active());
      if (!value.ok())
        return value.status();
      if (BinaryParts::decode(value.value(), narrow).infinite)
        return failure("LUT1D output conversion overflow",
                       FailureReason::ArithmeticOverflow);
      numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                1, profile);
      std::memcpy(output.address(at), replicas.data(), width);
    }
    return work(1);
  }
};
struct LutKernel final {
  SequenceProfile profile;
  bool channels;
  LutKernel(SequenceProfile selected, bool multi)
      : profile(selected), channels(multi) {}
  Status write(const ResultProgramPhase& phase,
               const ResourceVector<ResultTensorWriteWindow>& writers) {
    if (writers.size() != 1)
      return {ErrorCode::OperationFailed, "LUT1D requires one packed writer"};
    auto allocated = phase.allocator.allocate(sizeof(LutState));
    if (!allocated.ok())
      return allocated.status();
    auto buffer = allocated.take_value();
    std::unique_ptr<LutState, void (*)(LutState*)> state(
        new (buffer.data()) LutState(profile, channels, phase),
        [](auto* value) { value->~LutState(); });
    return state->execute(writers);
  }
};
using LutProgram = numeric_ops::WholeTensorProgram<LutKernel>;

OperationDefinition lut_operation(const std::string& key, bool channels,
                                  SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema)
    input.kind = OperationPortKind::Result;
  traits.input_schema[0].element_type_mask =
      traits.input_schema[1].element_type_mask = 12;
  traits.input_schema[2].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                       sizeof(LutProgram));
  traits.workspace_bytes = sizeof(LutState);
  operation.specialize_metadata = [channels, profile](const auto& inputs,
                                                      const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto in = inputs[0].result_schema->tensors[0].sample_shape();
    const auto table = inputs[1].result_schema->tensors[0].sample_shape();
    const auto mismatch = [] {
      return Status{ErrorCode::TypeMismatch,
                    "LUT1D input/table/axis shapes or logical size",
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (in.empty() || in.size() > 8 || table.size() != (channels ? 2U : 1U) ||
        table[0] < 1 || table[0] > 1048576 ||
        inputs[2].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{3} ||
        (channels && (!table[1] || table[1] != in.back())))
      return Answer(mismatch());
    for (const auto* shape : {&in, &table}) {
      std::uint64_t count = 1;
      for (auto extent : *shape) {
        if (!extent || extent > (UINT64_C(1) << 40) / count)
          return Answer(mismatch());
        count *= extent;
      }
    }
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
    if ((dtype != "float32" && dtype != "float64") ||
        (policy != "reject" && policy != "clamp" &&
         policy != "linear_extrapolate"))
      return Answer(numeric_ops::array_parameter_error("LUT1D dtype/domain"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_ops::numeric_tensor_schema(
            dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
            in));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.start_result = [channels, profile](const auto&,
                                               const auto& allocator) {
    return ResultContinuation::make<LutProgram>(allocator, profile, channels);
  };
  return operation;
}
}  // namespace
Status register_lut1d_application(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool channels : {false, true}) {
      auto status = registry->register_operation(
          lut_operation(std::string("curve.apply_lut1d") +
                            (channels ? "_channels" : "") + entry.first,
                        channels, entry.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
