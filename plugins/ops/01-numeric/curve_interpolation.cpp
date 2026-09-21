#include <algorithm>
#include <array>
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
#include "01-numeric/exact_curve.hpp"
#include "data/input_validation.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
Status shape_error(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
Result<ValueDescriptor> metadata(
    bool multi, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ValueDescriptor>;
  for (unsigned i = 0; i < 3; ++i) {
    const auto& d = inputs[i].descriptor;
    if ((d.element_type != ElementType::Float32 &&
         d.element_type != ElementType::Float64) ||
        d.shape.size() != (multi && i == 1 ? 2U : 1U))
      return Answer(shape_error(
          "curve inputs require independent Float32/64 x,y,query ranks"));
    for (auto extent : d.shape)
      if (!extent || extent > (UINT64_C(1) << 40))
        return Answer(shape_error("curve extents require 1..2^40"));
  }
  const auto knots = inputs[0].descriptor.shape[0];
  const auto count = inputs[2].descriptor.shape[0];
  const auto columns = multi ? inputs[1].descriptor.shape[1] : 1;
  if (knots < 2 || knots > 65536 || inputs[1].descriptor.shape[0] != knots ||
      columns > (UINT64_C(1) << 40) / knots ||
      columns > (UINT64_C(1) << 40) / count)
    return Answer(
        shape_error("curve requires matching K=2..65536 and products <=2^40"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (policy != "reject" && policy != "clamp" &&
       policy != "linear_extrapolate"))
    return Answer(numeric_ops::array_parameter_error(
        "invalid curve dtype/domain policy"));
  std::vector<std::uint64_t> shape{count};
  if (multi)
    shape.push_back(columns);
  return Answer(ValueDescriptor{
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      std::move(shape)});
}
struct CurveRow {
  std::uint64_t bits = 0;
  unsigned first = 0, count = 0, segment = 0;
  int selected = -1;
};
struct CurveState final {
  bool pchip, multi;
  SequenceProfile profile;
  const OperationInvocation& call;
  const ResourceBudget* budget;
  unsigned policy;
  numeric_ops::ExactCurve arithmetic;
  ResourceVector<std::uint64_t> knots;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  std::vector<std::uint64_t> scalar{0}, ordinate;
  CurveState(bool cubic, bool columns, SequenceProfile selected,
             const OperationInvocation& invocation)
      : pchip(cubic),
        multi(columns),
        profile(selected),
        call(invocation),
        budget(resource_internal::metadata_budget()),
        policy(std::get<std::string>(call.parameters.at("out_of_domain")) ==
                       "reject"
                   ? 0U
               : std::get<std::string>(call.parameters.at("out_of_domain")) ==
                       "clamp"
                   ? 1U
                   : 2U),
        arithmetic(selected),
        ordinate(multi ? 2 : 1, 0) {}
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
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& at) {
    using Answer = Result<std::uint64_t>;
    auto status = work(at.size() + 1);
    if (!status.ok())
      return Answer(status);
    const auto& input = call.inputs[port];
    auto address = input.byte_address(at);
    if (!address.ok())
      return Answer(address.status());
    const bool narrow = input.descriptor().element_type == ElementType::Float32;
    std::uint64_t bits = 0;
    std::memcpy(&bits, input.bytes().data() + address.value(), narrow ? 4 : 8);
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Answer(failure(port, at[0], "nonfinite curve input"));
    if (narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!value.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(value.significand);
        bits = sign |
               (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
               ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
      }
    }
    return Answer(bits);
  }
  Status classify(std::uint64_t index, CurveRow* row) {
    scalar[0] = index;
    auto query = read(2, scalar);
    if (!query.ok())
      return query.status();
    row->bits = query.value();
    const auto key = BinaryParts::decode(row->bits, false).order_key();
    unsigned lo = 0, hi = knots.size();
    while (lo < hi) {
      auto status = work(1);
      if (!status.ok())
        return status;
      const auto mid = lo + (hi - lo) / 2;
      if (BinaryParts::decode(knots[mid], false).order_key() < key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < knots.size() &&
        BinaryParts::decode(knots[lo], false).order_key() == key) {
      row->selected = lo;
    } else if (!lo || lo == knots.size()) {
      if (!policy)
        return failure(2, index, "curve query outside domain");
      if (policy == 1)
        row->selected = lo ? knots.size() - 1 : 0;
      row->segment = lo ? knots.size() - 2 : 0;
    } else {
      row->segment = lo - 1;
    }
    if (row->selected >= 0) {
      row->first = row->selected;
      row->count = 1;
    } else if (!pchip) {
      row->first = row->segment;
      row->count = 2;
    } else {
      row->first = row->segment ? row->segment - 1 : 0;
      row->count =
          std::min<unsigned>(knots.size(), row->segment + 3) - row->first;
    }
    return Status::success();
  }
  Result<Value> execute() {
    using Answer = Result<Value>;
    auto status = work(1);
    if (!status.ok())
      return Answer(status);
    knots.resize(call.inputs[0].descriptor().shape[0]);
    for (unsigned i = 0; i < knots.size(); ++i) {
      scalar[0] = i;
      auto value = read(0, scalar);
      if (!value.ok())
        return Answer(value.status());
      knots[i] = value.value();
      if (i && BinaryParts::decode(knots[i - 1], false).order_key() >=
                   BinaryParts::decode(knots[i], false).order_key())
        return Answer(failure(0, i, "curve knots require strict increase"));
    }
    const auto& shape = call.prepared->traits().outputs[0].fixed_output_shape;
    // Preserve control rejection before ordinate arithmetic without retaining
    // per-query rows or dependency certificates for the complete output.
    const auto lower = BinaryParts::decode(knots.front(), false).order_key();
    const auto upper = BinaryParts::decode(knots.back(), false).order_key();
    for (std::uint64_t i = 0; i < shape[0]; ++i) {
      scalar[0] = i;
      auto query = read(2, scalar);
      if (!query.ok())
        return Answer(query.status());
      const auto key = BinaryParts::decode(query.value(), false).order_key();
      if (!policy && (key < lower || key > upper))
        return Answer(failure(2, i, "curve query outside domain"));
    }
    const bool narrow =
        std::get<std::string>(call.parameters.at("dtype")) == "float32";
    auto allocated = MutableValue::allocate(
        {narrow ? ElementType::Float32 : ElementType::Float64, shape},
        call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    const auto columns = multi ? shape[1] : 1;
    const auto width = narrow ? 4U : 8U;
    std::optional<input_internal::Float32Environment> environment;
    if (narrow && profile != SequenceProfile::Strict)
      environment.emplace();
    const std::function<Status(std::uint64_t)> consume = [&](auto amount) {
      return work(amount);
    };
    for (std::uint64_t i = 0; i < shape[0]; ++i) {
      CurveRow row;
      status = classify(i, &row);
      if (!status.ok())
        return Answer(status);
      for (std::uint64_t column = 0; column < columns; ++column) {
        if (multi)
          ordinate[1] = column;
        for (unsigned j = 0; j < row.count; ++j) {
          ordinate[0] = row.first + j;
          auto value = read(1, ordinate);
          if (!value.ok())
            return Answer(value.status());
          x[j] = knots[row.first + j];
          y[j] = value.value();
        }
        auto value = arithmetic.evaluate(pchip, knots.size(), row.first,
                                         row.count, row.segment, row.selected,
                                         row.bits, x, y, narrow, consume, {},
                                         environment && environment->active());
        if (!value.ok())
          return Answer(value.status());
        if (BinaryParts::decode(value.value(), narrow).infinite)
          return Answer(failure(1, row.first, "curve output overflow",
                                FailureReason::ArithmeticOverflow));
        numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                  1, profile);
        std::memcpy(output.data() + (i * columns + column) * width,
                    replicas.data(), width);
      }
    }
    status = work(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  }
};
Result<Value> execute_curve(const OperationInvocation& call, bool pchip,
                            bool multi, SequenceProfile profile) {
  using Answer = Result<Value>;
  try {
    auto allocated = call.allocator.allocate(sizeof(CurveState));
    if (!allocated.ok())
      return Answer(allocated.status());
    auto buffer = allocated.take_value();
    std::unique_ptr<CurveState, void (*)(CurveState*)> state(
        new (buffer.data()) CurveState(pchip, multi, profile, call),
        [](CurveState* value) { value->~CurveState(); });
    return state->execute();
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}
OperationDefinition curve_operation(const std::string& key, bool pchip,
                                    bool multi, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Whole;
  output.requires_dense_output = true;
  traits.workspace_bytes = sizeof(CurveState);
  operation.specialize_metadata = [multi, profile](const auto& inputs,
                                                   const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto descriptor = metadata(multi, inputs, parameters);
    if (!descriptor.ok())
      return Answer(descriptor.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = descriptor.take_value();
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.callback = [pchip, multi,
                        profile](const OperationInvocation& call) {
    return execute_curve(call, pchip, multi, profile);
  };
  return operation;
}
}  // namespace
Status register_curve_interpolation(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool pchip : {false, true})
      for (bool multi : {false, true}) {
        auto status = registry->register_operation(curve_operation(
            std::string("curve.interpolate_") + (pchip ? "pchip" : "linear") +
                (multi ? "_multi" : "") + entry.first,
            pchip, multi, entry.second));
        if (!status.ok())
          return status;
      }
  return Status::success();
}
}  // namespace ps::plugin_internal
