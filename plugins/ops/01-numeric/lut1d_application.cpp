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

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_curve.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/uniform_axis.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

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
  const OperationInvocation& call;
  const ResourceBudget* budget;
  std::function<Status(std::uint64_t)> consume;
  std::vector<std::uint64_t> at, table_at;
  numeric_ops::UniformAxis axis;
  numeric_ops::ExactCurve arithmetic;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  LutState(SequenceProfile selected, bool multi,
           const OperationInvocation& invocation)
      : profile(selected),
        channels(multi),
        policy(std::get<std::string>(
                   invocation.parameters.at("out_of_domain")) == "reject"
                   ? 0U
               : std::get<std::string>(
                     invocation.parameters.at("out_of_domain")) == "clamp"
                   ? 1U
                   : 2U),
        call(invocation),
        budget(resource_internal::metadata_budget()),
        consume([this](auto amount) { return work(amount); }),
        at(invocation.inputs[0].descriptor().shape.size(), 0),
        table_at(multi ? 2 : 1, 0),
        axis(selected),
        arithmetic(selected) {}
  Status work(std::uint64_t amount) const {
    if (call.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    return budget ? budget->consume({amount}) : Status::success();
  }
  void advance() {
    const auto& shape = call.inputs[0].descriptor().shape;
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
    const auto& input = call.inputs[port];
    std::uint64_t bits = 0;
    const bool narrow = input.descriptor().element_type == ElementType::Float32;
    auto address = input.byte_address(coordinate);
    if (!address.ok())
      return Result<std::uint64_t>(address.status());
    std::memcpy(&bits, input.bytes().data() + address.value(), narrow ? 4 : 8);
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
  Result<Value> execute() {
    using Answer = Result<Value>;
    std::array<std::uint64_t, 3> axis_values{};
    for (unsigned j = 0; j < 3; ++j) {
      auto value = read(2, {j});
      if (!value.ok())
        return Answer(value.status());
      axis_values[j] = value.value();
    }
    input_internal::Float32Environment environment;
    axis.sampling.environment_established = environment.active();
    auto status = axis.validate(axis_values,
                                call.inputs[1].descriptor().shape[0], consume);
    if (!status.ok())
      return Answer(status.code == ErrorCode::OperationFailed
                        ? failure(status.message, status.reason)
                        : status);
    const auto total = call.inputs[0].region().element_count().value();
    // Preserve complete query rejection before table arithmetic without point
    // records.
    for (std::uint64_t i = 0; i < total; ++i, advance()) {
      auto query = read(0, at);
      if (!query.ok())
        return Answer(query.status());
      auto key = axis.key(query.value());
      if (!policy && (key < axis.key(axis.knots.front()) ||
                      key > axis.key(axis.knots.back())))
        return Answer(failure("LUT1D query outside axis domain"));
    }
    const auto& resolved = call.prepared->traits().outputs[0];
    auto allocated = MutableValue::allocate(
        {resolved.output_element_type, resolved.fixed_output_shape},
        call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const bool narrow = resolved.output_element_type == ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    for (std::uint64_t i = 0; i < total; ++i, advance()) {
      LutPoint point;
      status = classify(&point);
      if (!status.ok())
        return Answer(status);
      if (channels)
        table_at[1] = at.back();
      for (unsigned j = 0; j < point.count; ++j) {
        table_at[0] = point.first + j;
        auto value = read(1, table_at);
        if (!value.ok())
          return Answer(value.status());
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
        return Answer(value.status());
      if (BinaryParts::decode(value.value(), narrow).infinite)
        return Answer(failure("LUT1D output conversion overflow",
                              FailureReason::ArithmeticOverflow));
      numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                1, profile);
      std::memcpy(output.data() + i * width, replicas.data(), width);
    }
    status = work(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  }
};
Result<Value> execute_lut(const OperationInvocation& call, bool channels,
                          SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    auto allocated = call.allocator.allocate(sizeof(LutState));
    if (!allocated.ok())
      return Answer(allocated.status());
    auto buffer = allocated.take_value();
    std::unique_ptr<LutState, void (*)(LutState*)> state(
        new (buffer.data()) LutState(profile, channels, call),
        [](auto* value) { value->~LutState(); });
    return state->execute();
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}

OperationDefinition lut_operation(const std::string& key, bool channels,
                                  SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.input_schema[0].element_type_mask =
      traits.input_schema[1].element_type_mask = 12;
  traits.input_schema[2].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(LutState);
  operation.specialize_metadata = [channels, profile](const auto& inputs,
                                                      const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& in = inputs[0].descriptor.shape;
    const auto& table = inputs[1].descriptor.shape;
    const auto mismatch = [] {
      return Status{ErrorCode::TypeMismatch,
                    "LUT1D input/table/axis shapes or logical size",
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (in.empty() || in.size() > 8 || table.size() != (channels ? 2U : 1U) ||
        table[0] < 1 || table[0] > 1048576 ||
        inputs[2].descriptor.shape != std::vector<std::uint64_t>{3} ||
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
    resolved.metadata.descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64, in};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.callback = [channels, profile](const OperationInvocation& call) {
    return execute_lut(call, channels, profile);
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
