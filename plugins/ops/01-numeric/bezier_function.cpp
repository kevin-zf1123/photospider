#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct BezierPoint {
  std::uint64_t index = 0, fragment = 0, offset = 0, query = 0;
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
  unsigned stage = 0;
  numeric_ops::ExactSampling sampling;
  std::conditional_t<Values, numeric_ops::ExactBezier, EmptyBezier> arithmetic;
  std::array<std::uint64_t, 2> endpoints{};
  std::uint64_t step = 0;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  ResourceVector<std::uint64_t> topology;
  ResourceVector<BezierPoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  BezierState(SequenceProfile selected, unsigned order, unsigned size,
              unsigned anchors, bool clip)
      : profile(selected),
        degree(order),
        count(size),
        knots(anchors),
        clamp(clip),
        sampling(selected),
        arithmetic(selected) {}
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied, std::uint64_t root_calls = 0) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.bezier/1;exact-inverse-Q8192;%s%s",
        profile == SequenceProfile::Strict         ? "scalar-u64"
        : profile == SequenceProfile::AppleSilicon ? "NEON-u64x2"
                                                   : "AVX2-u64x4",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return {ErrorCode::Internal, "Bezier identity"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    result.strict_math_calls = root_calls;
    return phase.report_numeric(result);
  }
  Status fail(const DependencyPhase& phase, const BezierPoint* point,
              const std::string& message,
              FailureReason reason = FailureReason::InvalidDomain) const {
    Status status{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    if (point) {
      AtomKey atom;
      atom.output_index = phase.query.output_index;
      atom.rank = 1;
      atom.coordinate[0] = point->index;
      status.detail.atom = atom;
      if (point->has_query) {
        double q = 0;
        std::memcpy(&q, &point->query, 8);
        std::array<char, 64> buffer{};
        auto printed =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), q);
        if (printed.ec == std::errc{})
          status.message += "; x=" + std::string(buffer.data(), printed.ptr);
      }
    } else {
      auto atom = dependency_atom_key(phase.query);
      if (atom.ok())
        status.detail.atom = atom.take_value();
    }
    return status;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const BezierPoint* point) {
    auto work = phase.consume_work(phase.inputs[port].fragments().size() + 1);
    if (!work.ok())
      return Result<std::uint64_t>(work);
    std::uint64_t bits = 0;
    const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                        ElementType::Float32;
    auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite) {
      std::string message =
          "nonfinite Bezier port=" + std::to_string(port) + " coordinate=";
      for (auto v : at)
        message += std::to_string(v) + ",";
      return Result<std::uint64_t>(fail(phase, point, message));
    }
    return Result<std::uint64_t>(numeric_ops::ExactBezier::widen(bits, narrow));
  }
  Result<std::uint64_t> reconstruct(const DependencyPhase& phase,
                                    std::uint64_t anchor, std::uint64_t offset,
                                    const BezierPoint* point, unsigned segment,
                                    unsigned component) {
    auto result = sampling.weighted(anchor, offset, 1, 1, 1, false, false,
                                    phase.consume_work);
    if (!result.ok())
      return result;
    if (BinaryParts::decode(result.value(), false).infinite)
      return Result<std::uint64_t>(fail(
          phase, point,
          "Bezier reconstruction overflow segment=" + std::to_string(segment) +
              " component=" + std::to_string(component),
          FailureReason::ArithmeticOverflow));
    return result;
  }
  Status initialize(const DependencyPhase& phase) {
    const auto total = phase.query.outputs.element_count().value();
    if constexpr (Values) {
      // This dynamic protocol emits at least one association row per sample.
      // Reject its lower bound before reserving per-sample staging storage.
      if (total > phase.sets.maximum_boxes)
        return {ErrorCode::ResourceExhausted, "Bezier output association limit",
                FailureReason::CapacityLimit};
    }
    auto work = phase.consume_work(total + 1);
    if (!work.ok())
      return work;
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), 1);
    outputs.reserve(phase.query.outputs.boxes().size());
    if constexpr (Values) {
      points.reserve(total);
      topology.resize(knots + (degree - 1) * (knots - 1));
    }
    for (const auto& box : phase.query.outputs.boxes()) {
      auto output = MutableValue::allocate(phase.query.output.descriptor, box,
                                           phase.allocator);
      if (!output.ok())
        return output.status();
      if constexpr (Values) {
        auto span = box.dimensions()[0];
        for (std::uint64_t i = 0; i < span.extent; ++i) {
          work = phase.consume_work(1);
          if (!work.ok())
            return work;
          points.push_back(
              {span.offset + i, outputs.size(), i, 0, 0, -1, false});
        }
      }
      outputs.push_back(output.take_value());
    }
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, unsigned next) {
    using Answer = Result<DependencyPoll>;
    auto entries =
        [&](const BezierPoint* point) -> Result<std::vector<DependencyNeed>> {
      using Needs = Result<std::vector<DependencyNeed>>;
      std::vector<DependencyNeed> list;
      const auto append = [&](unsigned port, unsigned roles,
                              std::vector<Region> regions) -> Status {
        auto footprint =
            Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                    std::move(regions), phase.sets);
        if (!footprint.ok())
          return footprint.status();
        list.push_back({port, roles, footprint.take_value(), {}});
        return Status::success();
      };
      Status status;
      if (next == 0) {
        status = append(2, Values ? 7 : 5, {Region({{0, 1}})});
        if (status.ok() && count > 1)
          status = append(3, Values ? 7 : 5, {Region({{0, 1}})});
      } else if (next == 1) {
        status = append(0, 6, {Region({{0, knots}, {0, 1}})});
        if (status.ok())
          status =
              append(1, 6, {Region({{0, knots - 1}, {0, degree - 1}, {0, 1}})});
      } else if (point->selected >= 0) {
        status =
            append(0, 5,
                   {Region({{static_cast<std::uint64_t>(point->selected), 1},
                            {1, 1}})});
      } else {
        // Global x was Control/Validation. The selected numeric curve also
        // retains its local x Data, without promoting remote x to Data.
        status = append(0, 1, {Region({{point->segment, 2}, {0, 1}})});
        if (status.ok())
          status = append(
              1, 1, {Region({{point->segment, 1}, {0, degree - 1}, {0, 1}})});
        if (status.ok())
          status = append(0, 5, {Region({{point->segment, 2}, {1, 1}})});
        if (status.ok())
          status = append(
              1, 5, {Region({{point->segment, 1}, {0, degree - 1}, {1, 1}})});
      }
      return status.ok() ? Needs(std::move(list)) : Needs(status);
    };
    if constexpr (!Values) {
      auto list = entries(nullptr);
      return list.ok() ? multi_output::need(phase, list.take_value())
                       : Answer(list.status());
    } else {
      request_capacity =
          dependency_internal::metadata_owner(4096 + points.size() * 16384);
      std::vector<AtomCertificate> rows;
      rows.reserve(points.size());
      for (const auto& point : points) {
        auto work = phase.consume_work(8);
        if (!work.ok())
          return Answer(work);
        auto list = entries(&point);
        if (!list.ok())
          return Answer(list.status());
        rows.push_back({{point.index}, list.take_value()});
      }
      return Answer(DependencyNeedBatch{std::move(rows)});
    }
  }
  Status endpoints_ready(const DependencyPhase& phase) {
    const BezierPoint* point = Values ? &points.front() : nullptr;
    for (unsigned i = 0; i < (count > 1 ? 2U : 1U); ++i) {
      auto value = read(phase, i + 2, {0}, point);
      if (!value.ok())
        return value.status();
      endpoints[i] = value.value();
    }
    if (count == 1)
      return Status::success();
    const auto a = BinaryParts::decode(endpoints[0], false).order_key(),
               b = BinaryParts::decode(endpoints[1], false).order_key();
    if (a == b)
      return fail(phase, point, "equal sampling endpoints");
    auto value = sampling.weighted(endpoints[1], endpoints[0], 1, 1, count - 1,
                                   false, true, phase.consume_work);
    if (!value.ok())
      return value.status();
    step = value.value();
    const auto parts = BinaryParts::decode(step, false);
    if (!parts.magnitude || parts.infinite)
      return fail(phase, point, "unrepresentable sampling step",
                  FailureReason::ArithmeticOverflow);
    if (parts.negative != (b < a))
      return fail(phase, point, "sampling step direction");
    return Status::success();
  }
  Status topology_ready(const DependencyPhase& phase) {
    for (unsigned j = 0; j < knots; ++j) {
      auto value = read(phase, 0, {j, 0}, &points.front());
      if (!value.ok())
        return value.status();
      topology[j] = value.value();
      if (j && BinaryParts::decode(topology[j - 1], false).order_key() >=
                   BinaryParts::decode(topology[j], false).order_key())
        return fail(
            phase, &points.front(),
            "Bezier anchors require increasing x; anchor=" + std::to_string(j));
    }
    for (unsigned j = 0; j + 1 < knots; ++j) {
      x[0] = topology[j];
      x[degree] = topology[j + 1];
      for (unsigned h = 0; h + 1 < degree; ++h) {
        auto offset = read(phase, 1, {j, h, 0}, &points.front());
        if (!offset.ok())
          return offset.status();
        auto value = reconstruct(phase, h ? x[degree] : x[0], offset.value(),
                                 &points.front(), j, 0);
        if (!value.ok())
          return value.status();
        x[h + 1] = value.value();
        topology[knots + j * (degree - 1) + h] = value.value();
      }
      auto valid = arithmetic.monotone(x, degree, phase.consume_work);
      if (!valid.ok())
        return valid.status();
      if (!valid.value())
        return fail(phase, &points.front(),
                    "backward Bezier segment=" + std::to_string(j));
    }
    return Status::success();
  }
  Status classify(const DependencyPhase& phase, BezierPoint* point) {
    auto q =
        sampling.coordinate(point->index, count, endpoints, phase.consume_work);
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
          phase.consume_work);
      if (!neighbor.ok())
        return neighbor.status();
      if (BinaryParts::decode(neighbor.value(), false).order_key() == key)
        return fail(phase, point, "duplicate adjacent sampling coordinate",
                    FailureReason::ArithmeticOverflow);
    }
    unsigned lo = 0, hi = knots;
    while (lo < hi) {
      auto work = phase.consume_work(1);
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
        return fail(phase, point, "Bezier sample outside anchor domain");
      point->selected = lo ? knots - 1 : 0;
    } else {
      point->segment = lo - 1;
    }
    return Status::success();
  }
  Result<DependencyPoll> finish(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    ResourceVector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto work = phase.consume_work(1);
      if (!work.ok())
        return Answer(work);
      auto value = std::move(output).publish();
      if (!value.ok())
        return Answer(value.status());
      auto owned = publication->retain(value.take_value());
      if (!owned.ok())
        return Answer(owned.status());
      values.push_back(owned.take_value());
    }
    auto result =
        publication->finish(phase.query.output.descriptor, phase.query.outputs,
                            values.data(), values.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(65536);
    request_capacity.reset();
    if (!stage) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
      stage = 1;
      return need(phase, 0);
    }
    if (stage == 1) {
      auto status = endpoints_ready(phase);
      if (!status.ok())
        return Answer(status);
      if constexpr (Values) {
        stage = 2;
        return need(phase, 1);
      } else {
        std::array<std::uint64_t, 3> axis{
            endpoints[0], count == 1 ? endpoints[0] : endpoints[1], step};
        status = report(phase, 0, 3);
        if (!status.ok())
          return Answer(status);
        for (unsigned i = 0; i < 3; ++i) {
          numeric_ops::select_words(replicas.data(), axis[i], axis[i], 1,
                                    profile);
          std::memcpy(outputs[0].data() + i * 8, replicas.data(), 8);
        }
        return finish(phase);
      }
    }
    if constexpr (Values) {
      if (stage == 2) {
        auto status = topology_ready(phase);
        if (!status.ok())
          return Answer(status);
        for (auto& point : points) {
          status = classify(phase, &point);
          if (!status.ok())
            return Answer(status);
        }
        stage = 3;
        return need(phase, 2);
      }
      const bool narrow =
          phase.query.output.descriptor.element_type == ElementType::Float32;
      for (const auto& point : points) {
        Result<std::uint64_t> value(
            Status{ErrorCode::Internal, "uninitialized Bezier value"});
        if (point.selected >= 0) {
          auto selected =
              read(phase, 0, {static_cast<std::uint64_t>(point.selected), 1},
                   &point);
          if (!selected.ok())
            return Answer(selected.status());
          value = sampling.weighted(selected.value(), 0, 1, 0, 1, narrow, false,
                                    phase.consume_work);
        } else {
          const auto j = point.segment;
          auto first = read(phase, 0, {j, 1}, &point);
          if (!first.ok())
            return Answer(first.status());
          auto last = read(phase, 0, {j + 1, 1}, &point);
          if (!last.ok())
            return Answer(last.status());
          x[0] = topology[j];
          x[degree] = topology[j + 1];
          y[0] = first.value();
          y[degree] = last.value();
          for (unsigned h = 0; h + 1 < degree; ++h) {
            x[h + 1] = topology[knots + j * (degree - 1) + h];
            auto offset = read(phase, 1, {j, h, 1}, &point);
            if (!offset.ok())
              return Answer(offset.status());
            auto reconstructed = reconstruct(phase, h ? y[degree] : y[0],
                                             offset.value(), &point, j, 1);
            if (!reconstructed.ok())
              return Answer(reconstructed.status());
            y[h + 1] = reconstructed.value();
          }
          auto status = report(phase, 1, 0);
          if (!status.ok())
            return Answer(status);
          value = arithmetic.inverse(x, y, degree, point.query, narrow,
                                     phase.consume_work,
                                     [&] { return report(phase, 0, 0, 1); });
        }
        if (!value.ok())
          return Answer(value.status());
        if (BinaryParts::decode(value.value(), narrow).infinite)
          return Answer(fail(phase, &point, "Bezier output overflow",
                             FailureReason::ArithmeticOverflow));
        auto status = report(phase, point.selected >= 0 ? 1 : 0, 1);
        if (!status.ok())
          return Answer(status);
        numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                  1, profile);
        std::memcpy(
            outputs[point.fragment].data() + point.offset * (narrow ? 4 : 8),
            replicas.data(), narrow ? 4 : 8);
      }
      return finish(phase);
    }
    return Answer(Status{ErrorCode::Internal, "invalid Bezier phase"});
  }
};
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
  values.region_rule = OperationRegionRule::Dependency;
  values.dependency_version = 1;
  values.continuation_bytes = sizeof(BezierState<true>);
  values.maximum_dependency_stages = 4;
  values.failure_delivery = FailureDelivery::PerAtomOutcome;
  traits.outputs.push_back(values);
  auto& axis = traits.outputs[1];
  axis.key = "axis";
  axis.continuation_bytes = sizeof(BezierState<false>);
  axis.maximum_dependency_stages = 2;
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
    outputs[0].regional_atomic = true;
    outputs[1].metadata.descriptor = {ElementType::Float64, {3}};
    outputs[1].metadata.atomic_trailing_axes = 1;
    return Answer(std::move(outputs));
  };
  operation.start_dependency = [profile](const auto& query,
                                         const auto& allocator) {
    const auto degree = static_cast<unsigned>(
        std::get<std::int64_t>(query.parameters.at("degree")));
    const auto count = static_cast<unsigned>(
        std::get<std::int64_t>(query.parameters.at("count")));
    const auto knots =
        static_cast<unsigned>(query.inputs[0].descriptor.shape[0]);
    const bool clamp =
        std::get<std::string>(query.parameters.at("out_of_domain")) == "clamp";
    return query.output_index == 0
               ? DependencyContinuation::make<BezierState<true>>(
                     allocator, profile, degree, count, knots, clamp)
               : DependencyContinuation::make<BezierState<false>>(
                     allocator, profile, degree, count, knots, clamp);
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
