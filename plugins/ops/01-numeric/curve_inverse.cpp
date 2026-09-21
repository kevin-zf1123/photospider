#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_curve.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct InversePoint {
  std::uint64_t row = 0, query = 0;
  unsigned first = 0, count = 0, segment = 0;
  int selected = -1;
};
struct InverseState {
  bool pchip, clamp, increasing = true;
  SequenceProfile profile;
  numeric_ops::ExactCurve arithmetic;
  const OperationInvocation& call;
  const ResourceBudget* budget;
  std::function<Status(std::uint64_t)> consume;
  ResourceVector<std::uint64_t> xs, ys;
  InverseState(bool cubic, bool clipped, SequenceProfile selected,
               const OperationInvocation& invocation)
      : pchip(cubic),
        clamp(clipped),
        profile(selected),
        arithmetic(selected),
        call(invocation),
        budget(resource_internal::metadata_budget()),
        consume([this](auto amount) { return work(amount); }) {}
  Status work(std::uint64_t amount) const {
    if (call.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    return budget ? budget->consume({amount}) : Status::success();
  }
  Status failure(unsigned port, std::uint64_t index, const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    return {ErrorCode::OperationFailed,
            std::string(message) + "; port=" + std::to_string(port) +
                " index=" + std::to_string(index),
            reason,
            {FailureOrigin::Domain, FailureScope::Run}};
  }
  Result<std::uint64_t> read(unsigned port, std::uint64_t index) const {
    auto charged = work(2);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto& input = call.inputs[port];
    const bool narrow = input.descriptor().element_type == ElementType::Float32;
    std::uint64_t bits = 0;
    auto address = input.byte_address({index});
    if (!address.ok())
      return Result<std::uint64_t>(address.status());
    std::memcpy(&bits, input.bytes().data() + address.value(), narrow ? 4 : 8);
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(port, index, "nonfinite inverse input"));
    if (narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!value.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(value.significand);
        bits =
            sign |
            (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
            ((value.significand << (52 - top)) & UINT64_C(0x000fffffffffffff));
      }
    }
    return Result<std::uint64_t>(bits);
  }
  std::uint64_t key(std::uint64_t bits) const {
    const auto ordered = BinaryParts::decode(bits, false).order_key();
    return increasing ? ordered : UINT64_MAX - ordered;
  }
  Status classify(InversePoint* point) {
    auto query = read(2, point->row);
    if (!query.ok())
      return query.status();
    point->query = query.value();
    const auto wanted = key(point->query);
    unsigned lo = 0, hi = ys.size();
    while (lo < hi) {
      auto work = consume(1);
      if (!work.ok())
        return work;
      const auto middle = lo + (hi - lo) / 2;
      if (key(ys[middle]) < wanted)
        lo = middle + 1;
      else
        hi = middle;
    }
    if (lo < ys.size() && key(ys[lo]) == wanted) {
      point->selected = lo;
    } else if (!lo || lo == ys.size()) {
      if (!clamp)
        return failure(2, point->row, "inverse query outside domain");
      point->selected = lo ? ys.size() - 1 : 0;
    } else {
      point->segment = lo - 1;
    }
    if (point->selected >= 0) {
      point->first = point->selected;
      point->count = 1;
    } else if (!pchip) {
      point->first = point->segment;
      point->count = 2;
    } else {
      point->first = point->segment ? point->segment - 1 : 0;
      point->count =
          std::min<unsigned>(ys.size(), point->segment + 3) - point->first;
    }
    return Status::success();
  }
  Result<Value> execute() {
    using Answer = Result<Value>;
    xs.resize(call.inputs[0].descriptor().shape[0]);
    ys.resize(xs.size());
    for (unsigned i = 0; i < xs.size(); ++i) {
      auto x = read(0, i), y = read(1, i);
      if (!x.ok() || !y.ok())
        return Answer(!x.ok() ? x.status() : y.status());
      xs[i] = x.value();
      ys[i] = y.value();
      if (i && BinaryParts::decode(xs[i - 1], false).order_key() >=
                   BinaryParts::decode(xs[i], false).order_key())
        return Answer(failure(0, i, "inverse x requires strict increase"));
      if (i == 1)
        increasing = BinaryParts::decode(ys[0], false).order_key() <
                     BinaryParts::decode(ys[1], false).order_key();
      if (i && key(ys[i - 1]) >= key(ys[i]))
        return Answer(failure(1, i, "inverse y requires strict monotonicity"));
    }
    const auto count = call.inputs[2].descriptor().shape[0];
    // Validate all query controls before inverse arithmetic without retaining
    // per-query classifications or repeating segment searches.
    for (std::uint64_t row = 0; row < count; ++row) {
      auto query = read(2, row);
      if (!query.ok())
        return Answer(query.status());
      const auto wanted = key(query.value());
      if (!clamp && (wanted < key(ys.front()) || wanted > key(ys.back())))
        return Answer(failure(2, row, "inverse query outside domain"));
    }
    const auto& output_traits = call.prepared->traits().outputs[0];
    const bool narrow =
        output_traits.output_element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    auto allocated = MutableValue::allocate(
        {output_traits.output_element_type, output_traits.fixed_output_shape},
        call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    for (std::uint64_t row = 0; row < count; ++row) {
      InversePoint point;
      point.row = row;
      auto status = classify(&point);
      if (!status.ok())
        return Answer(status);
      std::array<std::uint64_t, 4> x{}, y{};
      for (unsigned i = 0; i < point.count; ++i) {
        x[i] = xs[point.first + i];
        y[i] = ys[point.first + i];
      }
      auto computed =
          arithmetic.inverse(pchip, xs.size(), point.first, point.count,
                             point.segment, point.selected, point.query, x, y,
                             narrow, consume, [] { return Status::success(); });
      if (!computed.ok())
        return Answer(computed.status());
      const auto bits = computed.value();
      if (BinaryParts::decode(bits, narrow).infinite)
        return Answer(failure(2, point.row, "inverse output overflow",
                              FailureReason::ArithmeticOverflow));
      std::memcpy(output.data() + row * width, &bits, width);
    }
    auto status = work(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  }
};
Result<Value> execute_inverse(const OperationInvocation& call, bool pchip,
                              SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    auto scratch = call.allocator.allocate(sizeof(InverseState));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto buffer = scratch.take_value();
    std::unique_ptr<InverseState, void (*)(InverseState*)> state(
        new (buffer.data())
            InverseState(pchip,
                         std::get<std::string>(
                             call.parameters.at("out_of_domain")) == "clamp",
                         profile, call),
        [](auto* value) { value->~InverseState(); });
    return state->execute();
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
OperationDefinition operation(const std::string& name, bool pchip,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& port : traits.input_schema)
    port.element_type_mask = 12;
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(InverseState);
  definition.specialize_metadata = [profile](const auto& inputs,
                                             const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 3)
      return Answer(mismatch("inverse port count"));
    for (const auto& input : inputs)
      if (input.result_schema || input.descriptor.shape.size() != 1 ||
          (input.descriptor.element_type != ElementType::Float32 &&
           input.descriptor.element_type != ElementType::Float64))
        return Answer(mismatch("inverse requires Float32/64 rank-one ports"));
    if (inputs[0].descriptor.shape[0] != inputs[1].descriptor.shape[0])
      return Answer(mismatch("inverse x/y count mismatch"));
    if (inputs[0].descriptor.shape[0] < 2 ||
        inputs[0].descriptor.shape[0] > 65536 ||
        !inputs[2].descriptor.shape[0] ||
        inputs[2].descriptor.shape[0] > (UINT64_C(1) << 40))
      return Answer(numeric_ops::array_parameter_error(
          "inverse requires K=2..65536,N=1..2^40"));
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
    if ((dtype != "float32" && dtype != "float64") ||
        (policy != "reject" && policy != "clamp"))
      return Answer(numeric_ops::array_parameter_error("inverse dtype/policy"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        inputs[2].descriptor.shape};
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  definition.callback = [pchip, profile](const OperationInvocation& call) {
    return execute_inverse(call, pchip, profile);
  };
  return definition;
}
}  // namespace
Status register_curve_inverse(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool pchip : {false, true}) {
      auto status = registry->register_operation(operation(
          std::string(pchip ? "curve.invert_pchip" : "curve.invert_linear") +
              profile.first,
          pchip, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
