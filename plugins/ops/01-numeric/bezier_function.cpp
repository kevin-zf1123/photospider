#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct BezierPoint {
  std::uint64_t index = 0, query = 0;
  unsigned segment = 0;
  int selected = -1;
  bool has_query = false;
};
struct EmptyBezier {
  explicit EmptyBezier(SequenceProfile) {}
};
template <bool Values>
struct BezierState final {
  SequenceProfile profile;
  unsigned degree, count, knots;
  bool clamp;
  const OperationInvocation& call;
  const ResourceBudget* budget;
  std::function<Status(std::uint64_t)> consume;
  numeric_ops::ExactSampling sampling;
  std::conditional_t<Values, numeric_ops::ExactBezier, EmptyBezier> arithmetic;
  std::array<std::uint64_t, 2> endpoints{};
  std::uint64_t step = 0;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  ResourceVector<std::uint64_t> topology;
  explicit BezierState(SequenceProfile selected,
                       const OperationInvocation& invocation)
      : profile(selected),
        degree(std::get<std::int64_t>(invocation.parameters.at("degree"))),
        count(std::get<std::int64_t>(invocation.parameters.at("count"))),
        knots(Values ? invocation.inputs[0].descriptor().shape[0] : 0),
        clamp(std::get<std::string>(
                  invocation.parameters.at("out_of_domain")) == "clamp"),
        call(invocation),
        budget(resource_internal::metadata_budget()),
        consume([this](auto amount) { return work(amount); }),
        sampling(selected),
        arithmetic(selected) {}
  Status work(std::uint64_t amount) const {
    if (call.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    return budget ? budget->consume({amount}) : Status::success();
  }
  Status fail(const BezierPoint* point, const std::string& message,
              FailureReason reason = FailureReason::InvalidDomain) const {
    Status status{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Run}};
    if (point) {
      status.message += "; sample=" + std::to_string(point->index);
      if (point->has_query) {
        double q = 0;
        std::memcpy(&q, &point->query, 8);
        std::array<char, 64> buffer{};
        auto printed =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), q);
        if (printed.ec == std::errc{})
          status.message += "; x=" + std::string(buffer.data(), printed.ptr);
      }
    }
    return status;
  }
  Result<std::uint64_t> read(unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const BezierPoint* point) {
    auto charged = work(at.size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const auto& input = call.inputs[Values ? port : port - 2];
    std::uint64_t bits = 0;
    const bool narrow = input.descriptor().element_type == ElementType::Float32;
    auto address = input.byte_address(at);
    if (!address.ok())
      return Result<std::uint64_t>(address.status());
    std::memcpy(&bits, input.bytes().data() + address.value(), narrow ? 4 : 8);
    auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite) {
      std::string message =
          "nonfinite Bezier port=" + std::to_string(port) + " coordinate=";
      for (auto v : at)
        message += std::to_string(v) + ",";
      return Result<std::uint64_t>(fail(point, message));
    }
    return Result<std::uint64_t>(numeric_ops::ExactBezier::widen(bits, narrow));
  }
  Result<std::uint64_t> reconstruct(std::uint64_t anchor, std::uint64_t offset,
                                    const BezierPoint* point, unsigned segment,
                                    unsigned component) {
    auto result =
        sampling.weighted(anchor, offset, 1, 1, 1, false, false, consume);
    if (!result.ok())
      return result;
    if (BinaryParts::decode(result.value(), false).infinite)
      return Result<std::uint64_t>(fail(
          point,
          "Bezier reconstruction overflow segment=" + std::to_string(segment) +
              " component=" + std::to_string(component),
          FailureReason::ArithmeticOverflow));
    return result;
  }
  Status endpoints_ready() {
    const BezierPoint* point = nullptr;
    for (unsigned i = 0; i < (count > 1 ? 2U : 1U); ++i) {
      auto value = read(i + 2, {0}, point);
      if (!value.ok())
        return value.status();
      endpoints[i] = value.value();
    }
    if (count == 1)
      return Status::success();
    const auto a = BinaryParts::decode(endpoints[0], false).order_key(),
               b = BinaryParts::decode(endpoints[1], false).order_key();
    if (a == b)
      return fail(point, "equal sampling endpoints");
    auto value = sampling.weighted(endpoints[1], endpoints[0], 1, 1, count - 1,
                                   false, true, consume);
    if (!value.ok())
      return value.status();
    step = value.value();
    const auto parts = BinaryParts::decode(step, false);
    if (!parts.magnitude || parts.infinite)
      return fail(point, "unrepresentable sampling step",
                  FailureReason::ArithmeticOverflow);
    if (parts.negative != (b < a))
      return fail(point, "sampling step direction");
    return Status::success();
  }
  Status topology_ready() {
    for (unsigned j = 0; j < knots; ++j) {
      auto value = read(0, {j, 0}, nullptr);
      if (!value.ok())
        return value.status();
      topology[j] = value.value();
      if (j && BinaryParts::decode(topology[j - 1], false).order_key() >=
                   BinaryParts::decode(topology[j], false).order_key())
        return fail(nullptr, "Bezier anchors require increasing x; anchor=" +
                                 std::to_string(j));
    }
    for (unsigned j = 0; j + 1 < knots; ++j) {
      x[0] = topology[j];
      x[degree] = topology[j + 1];
      for (unsigned h = 0; h + 1 < degree; ++h) {
        auto offset = read(1, {j, h, 0}, nullptr);
        if (!offset.ok())
          return offset.status();
        auto value =
            reconstruct(h ? x[degree] : x[0], offset.value(), nullptr, j, 0);
        if (!value.ok())
          return value.status();
        x[h + 1] = value.value();
        topology[knots + j * (degree - 1) + h] = value.value();
      }
      auto valid = arithmetic.monotone(x, degree, consume);
      if (!valid.ok())
        return valid.status();
      if (!valid.value())
        return fail(nullptr, "backward Bezier segment=" + std::to_string(j));
    }
    return Status::success();
  }
  Status classify(BezierPoint* point) {
    auto q = sampling.coordinate(point->index, count, endpoints, consume);
    if (!q.ok())
      return q.status();
    point->query = q.value();
    point->has_query = true;
    const auto key = BinaryParts::decode(q.value(), false).order_key();
    for (int direction : {-1, 1}) {
      if ((direction < 0 && !point->index) ||
          (direction > 0 && point->index + 1 == count))
        continue;
      auto neighbor = sampling.coordinate(
          direction < 0 ? point->index - 1 : point->index + 1, count, endpoints,
          consume);
      if (!neighbor.ok())
        return neighbor.status();
      if (BinaryParts::decode(neighbor.value(), false).order_key() == key)
        return fail(point, "duplicate adjacent sampling coordinate",
                    FailureReason::ArithmeticOverflow);
    }
    unsigned lo = 0, hi = knots;
    while (lo < hi) {
      auto work = consume(1);
      if (!work.ok())
        return work;
      const auto mid = lo + (hi - lo) / 2;
      if (BinaryParts::decode(topology[mid], false).order_key() < key)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < knots &&
        BinaryParts::decode(topology[lo], false).order_key() == key) {
      point->selected = lo;
    } else if (!lo || lo == knots) {
      if (!clamp)
        return fail(point, "Bezier sample outside anchor domain");
      point->selected = lo ? knots - 1 : 0;
    } else {
      point->segment = lo - 1;
    }
    return Status::success();
  }
  Result<Value> execute() {
    using Answer = Result<Value>;
    auto status = endpoints_ready();
    if (!status.ok())
      return Answer(status);
    if constexpr (Values) {
      topology.resize(knots + (degree - 1) * (knots - 1));
      status = topology_ready();
      if (!status.ok())
        return Answer(status);
      // Validate the complete sampling controls before any y arithmetic.
      // Keep only one row of classification state, independent of count.
      for (std::uint64_t i = 0; i < count; ++i) {
        BezierPoint point;
        point.index = i;
        status = classify(&point);
        if (!status.ok())
          return Answer(status);
      }
    }
    const auto& resolved = call.prepared->traits().outputs[call.output_index];
    auto allocated = MutableValue::allocate(
        {resolved.output_element_type, resolved.fixed_output_shape},
        call.output_region, call.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto output = allocated.take_value();
    if constexpr (!Values) {
      const std::array<std::uint64_t, 3> axis{
          endpoints[0], count == 1 ? endpoints[0] : endpoints[1], step};
      for (unsigned i = 0; i < 3; ++i) {
        numeric_ops::select_words(replicas.data(), axis[i], axis[i], 1,
                                  profile);
        std::memcpy(output.data() + i * 8, replicas.data(), 8);
      }
    } else {
      const bool narrow =
          std::get<std::string>(call.parameters.at("dtype")) == "float32";
      for (std::uint64_t i = 0; i < count; ++i) {
        BezierPoint point;
        point.index = i;
        status = classify(&point);
        if (!status.ok())
          return Answer(status);
        Result<std::uint64_t> value(
            Status{ErrorCode::Internal, "uninitialized Bezier value"});
        if (point.selected >= 0) {
          auto selected =
              read(0, {static_cast<std::uint64_t>(point.selected), 1}, &point);
          if (!selected.ok())
            return Answer(selected.status());
          value = sampling.weighted(selected.value(), 0, 1, 0, 1, narrow, false,
                                    consume);
        } else {
          const auto j = point.segment;
          auto first = read(0, {j, 1}, &point);
          if (!first.ok())
            return Answer(first.status());
          auto last = read(0, {j + 1, 1}, &point);
          if (!last.ok())
            return Answer(last.status());
          x[0] = topology[j];
          x[degree] = topology[j + 1];
          y[0] = first.value();
          y[degree] = last.value();
          for (unsigned h = 0; h + 1 < degree; ++h) {
            x[h + 1] = topology[knots + j * (degree - 1) + h];
            auto offset = read(1, {j, h, 1}, &point);
            if (!offset.ok())
              return Answer(offset.status());
            auto reconstructed =
                reconstruct(h ? y[degree] : y[0], offset.value(), &point, j, 1);
            if (!reconstructed.ok())
              return Answer(reconstructed.status());
            y[h + 1] = reconstructed.value();
          }
          value =
              arithmetic.inverse(x, y, degree, point.query, narrow, consume);
        }
        if (!value.ok())
          return Answer(value.status());
        if (BinaryParts::decode(value.value(), narrow).infinite)
          return Answer(fail(&point, "Bezier output overflow",
                             FailureReason::ArithmeticOverflow));
        numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                  1, profile);
        std::memcpy(output.data() + i * (narrow ? 4 : 8), replicas.data(),
                    narrow ? 4 : 8);
      }
    }
    status = work(1);
    return status.ok() ? std::move(output).publish() : Answer(status);
  }
};
template <bool Values>
Result<Value> execute_bezier(const OperationInvocation& call,
                             SequenceProfile profile) {
  using Answer = Result<Value>;
  using State = BezierState<Values>;
  try {
    auto allocated = call.allocator.allocate(sizeof(State));
    if (!allocated.ok())
      return Answer(allocated.status());
    auto buffer = allocated.take_value();
    std::unique_ptr<State, void (*)(State*)> state(
        new (buffer.data()) State(profile, call),
        [](State* value) { value->~State(); });
    return state->execute();
  } catch (const std::bad_alloc&) {
    return Answer(Status{ErrorCode::ResourceExhausted,
                         {},
                         FailureReason::CapacityLimit,
                         {FailureOrigin::Resource, FailureScope::Run}});
  }
}

OperationDefinition bezier_operation(const std::string& key,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 4;
  traits.input_schema.resize(4);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"degree", OperationParameterType::Int64, true, true, 2, 3},
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  auto& values = traits.outputs[0];
  values.key = "values";
  values.region_rule = OperationRegionRule::Whole;
  values.requires_dense_output = true;
  traits.workspace_bytes = sizeof(BezierState<true>);
  traits.outputs.push_back(values);
  auto& axis = traits.outputs[1];
  axis.key = "axis";
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto degree =
        static_cast<unsigned>(std::get<std::int64_t>(parameters.at("degree")));
    const auto count =
        static_cast<unsigned>(std::get<std::int64_t>(parameters.at("count")));
    const auto& a = inputs[0].descriptor.shape;
    if (a.size() != 2 || a[0] < 2 || a[0] > 65536 || a[1] != 2 ||
        inputs[1].descriptor.shape !=
            std::vector<std::uint64_t>{a[0] - 1, degree - 1, 2} ||
        inputs[2].descriptor.shape != std::vector<std::uint64_t>{1} ||
        inputs[3].descriptor.shape != std::vector<std::uint64_t>{1})
      return Answer(Status{ErrorCode::TypeMismatch,
                           "Bezier anchors/handles/scalar shapes",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
    if ((dtype != "float32" && dtype != "float64") ||
        (domain != "reject" && domain != "clamp"))
      return Answer(
          numeric_ops::array_parameter_error("Bezier dtype/domain policy"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    std::vector<OperationOutputSpecialization> outputs(2);
    outputs[0].metadata.descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        {count}};
    outputs[0].input_indices = count == 1
                                   ? std::vector<std::uint32_t>{0, 1, 2}
                                   : std::vector<std::uint32_t>{0, 1, 2, 3};
    outputs[1].metadata.descriptor = {ElementType::Float64, {3}};
    outputs[1].metadata.atomic_trailing_axes = 1;
    outputs[1].input_indices = count == 1 ? std::vector<std::uint32_t>{2}
                                          : std::vector<std::uint32_t>{2, 3};
    return Answer(std::move(outputs));
  };
  operation.callback = [profile](const OperationInvocation& call) {
    return call.output_index == 0 ? execute_bezier<true>(call, profile)
                                  : execute_bezier<false>(call, profile);
  };
  return operation;
}
}  // namespace
Status register_bezier_function(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(bezier_operation(
        std::string("curve.sample_bezier_function") + entry.first,
        entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
