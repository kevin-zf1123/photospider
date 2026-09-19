#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_curve.hpp"
#include "data/input_validation.hpp"
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
struct CurvePoint {
  std::uint64_t row, column, fragment, offset;
  std::size_t lookup = 0;
};
struct CurveRow {
  std::uint64_t index = 0, bits = 0;
  std::size_t first_point = 0;
  unsigned first = 0, count = 0, segment = 0;
  int selected = -1;
};
struct CurveState final {
  bool pchip, multi;
  SequenceProfile profile;
  unsigned policy;
  numeric_ops::ExactCurve arithmetic;
  unsigned stage = 0;
  ResourceVector<std::uint64_t> knots;
  ResourceVector<CurvePoint> points;
  ResourceVector<CurveRow> rows;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  CurveState(bool cubic, bool columns, SequenceProfile selected,
             unsigned domain)
      : pchip(cubic),
        multi(columns),
        profile(selected),
        policy(domain),
        arithmetic(selected) {}
  std::vector<std::uint64_t> coordinate(const CurvePoint& point) const {
    return multi ? std::vector<std::uint64_t>{point.row, point.column}
                 : std::vector<std::uint64_t>{point.row};
  }
  Status failure(const DependencyPhase& phase, const CurvePoint& point,
                 unsigned port, std::uint64_t index, const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{ErrorCode::OperationFailed,
                  std::string(message) + "; port=" + std::to_string(port) +
                      " index=" + std::to_string(index),
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = multi ? 2 : 1;
    atom.coordinate[0] = point.row;
    if (multi)
      atom.coordinate[1] = point.column;
    result.detail.atom = atom;
    return result;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const CurvePoint& point) {
    auto charged =
        phase.consume_work(phase.inputs[port].fragments().size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    std::uint64_t bits = 0;
    const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                        ElementType::Float32;
    auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(phase, point, port, at[0], "nonfinite curve input"));
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
    return Result<std::uint64_t>(bits);
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.curve/1;%s%s;exact-rational;%s%s",
        pchip ? "pchip" : "linear", multi ? "-multi" : "",
        profile == SequenceProfile::Strict         ? "scalar-u64"
        : profile == SequenceProfile::AppleSilicon ? "NEON-u64x2"
                                                   : "AVX2-u64x4",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return {ErrorCode::Internal, "curve diagnostic identity"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    return phase.report_numeric(result);
  }
  Status initialize(const DependencyPhase& phase) {
    const auto count = phase.query.outputs.element_count().value();
    auto status = phase.consume_work(count * 4 + 1);
    if (!status.ok())
      return status;
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), multi ? 2 : 1);
    points.reserve(count);
    outputs.reserve(phase.query.outputs.boxes().size());
    knots.resize(phase.query.inputs[0].descriptor.shape[0]);
    // Project the bounded rectangle list, not the logical whole output.
    // Query lookup then costs O(P log K), with O(M) cell association work.
    auto projection_capacity = dependency_internal::metadata_owner(
        4096 + phase.query.outputs.boxes().size() * 512);
    std::vector<Region> projected;
    projected.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes())
      projected.emplace_back(std::vector<RegionDimension>{box.dimensions()[0]});
    auto projection =
        Footprint::from_regions(phase.query.inputs[2].descriptor.shape,
                                std::move(projected), phase.sets);
    if (!projection.ok())
      return projection.status();
    rows.reserve(projection.value().element_count().value());
    for (const auto& box : projection.value().boxes()) {
      const auto span = box.dimensions()[0];
      for (std::uint64_t i = 0; i < span.extent; ++i) {
        status = phase.consume_work(1);
        if (!status.ok())
          return status;
        rows.push_back({span.offset + i, 0, SIZE_MAX, 0, 0, 0, -1});
      }
    }
    for (const auto& box : phase.query.outputs.boxes()) {
      auto output = MutableValue::allocate(phase.query.output.descriptor, box,
                                           phase.allocator);
      if (!output.ok())
        return output.status();
      const auto row = box.dimensions()[0];
      const auto col = multi ? box.dimensions()[1] : RegionDimension{0, 1};
      status = phase.consume_work(64);
      if (!status.ok())
        return status;
      const auto found =
          std::lower_bound(rows.begin(), rows.end(), row.offset,
                           [](const auto& a, auto b) { return a.index < b; });
      const auto base = static_cast<std::size_t>(found - rows.begin());
      for (std::uint64_t i = 0; i < row.extent; ++i)
        for (std::uint64_t j = 0; j < col.extent; ++j) {
          status = phase.consume_work(1);
          if (!status.ok())
            return status;
          if (rows[base + i].first_point == SIZE_MAX)
            rows[base + i].first_point = points.size();
          points.push_back({row.offset + i, col.offset + j, outputs.size(),
                            i * col.extent + j, base + i});
        }
      outputs.push_back(output.take_value());
    }
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, unsigned port) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 8192);
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto charged = phase.consume_work(8);
      if (!charged.ok())
        return Answer(charged);
      std::vector<RegionDimension> dimensions;
      if (port == 0) {
        dimensions = {{0, knots.size()}};
      } else if (port == 2) {
        dimensions = {{point.row, 1}};
      } else {
        const auto& row = rows[point.lookup];
        dimensions = {{row.first, row.count}};
        if (multi)
          dimensions.push_back({point.column, 1});
      }
      auto support =
          Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                  {Region(dimensions)}, phase.sets);
      if (!support.ok())
        return Answer(support.status());
      auto closure = input_internal::validation_closure(
          phase.query.inputs[port], support.value(), phase.sets,
          phase.consume_work);
      if (!closure.ok())
        return Answer(closure.status());
      certificates.push_back({coordinate(point),
                              {{port,
                                static_cast<std::uint8_t>(port == 1 ? 1 : 2),
                                support.take_value(),
                                {}},
                               {port, 4, closure.take_value(), {}}}});
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  Status classify(const DependencyPhase& phase, CurveRow* row) {
    const auto& point = points[row->first_point];
    auto query = read(phase, 2, {row->index}, point);
    if (!query.ok())
      return query.status();
    row->bits = query.value();
    const auto key = BinaryParts::decode(row->bits, false).order_key();
    unsigned lo = 0, hi = knots.size();
    while (lo < hi) {
      auto work = phase.consume_work(1);
      if (!work.ok())
        return work;
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
      if (policy == 0)
        return failure(phase, point, 2, row->index,
                       "curve query outside domain");
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
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(32768);
    request_capacity.reset();
    if (!stage) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
      stage = 1;
      return need(phase, 0);
    }
    if (stage == 1) {
      for (unsigned i = 0; i < knots.size(); ++i) {
        auto value = read(phase, 0, {i}, points.front());
        if (!value.ok())
          return Answer(value.status());
        knots[i] = value.value();
        if (i && BinaryParts::decode(knots[i - 1], false).order_key() >=
                     BinaryParts::decode(knots[i], false).order_key())
          return Answer(failure(phase, points.front(), 0, i,
                                "curve knots require strict increase"));
      }
      stage = 2;
      return need(phase, 2);
    }
    if (stage == 2) {
      for (auto& row : rows) {
        auto status = classify(phase, &row);
        if (!status.ok())
          return Answer(status);
      }
      stage = 3;
      return need(phase, 1);
    }
    const bool narrow =
        phase.query.output.descriptor.element_type == ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    for (const auto& point : points) {
      const auto& row = rows[point.lookup];
      for (unsigned j = 0; j < row.count; ++j) {
        std::vector<std::uint64_t> at{row.first + j};
        if (multi)
          at.push_back(point.column);
        auto value = read(phase, 1, at, point);
        if (!value.ok())
          return Answer(value.status());
        x[j] = knots[row.first + j];
        y[j] = value.value();
      }
      auto status = report(phase, 1, 0);
      if (!status.ok())
        return Answer(status);
      auto value = arithmetic.evaluate(
          pchip, knots.size(), row.first, row.count, row.segment, row.selected,
          row.bits, x, y, narrow, phase.consume_work);
      if (!value.ok())
        return Answer(value.status());
      if (BinaryParts::decode(value.value(), narrow).infinite)
        return Answer(failure(phase, point, 1, row.first,
                              "curve output overflow",
                              FailureReason::ArithmeticOverflow));
      status = report(phase, 0, 1);
      if (!status.ok())
        return Answer(status);
      numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                1, profile);
      std::memcpy(outputs[point.fragment].data() + point.offset * width,
                  replicas.data(), width);
    }
    ResourceVector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto work = phase.consume_work(1);
      if (!work.ok())
        return Answer(work);
      auto published = std::move(output).publish();
      if (!published.ok())
        return Answer(published.status());
      auto retained = publication->retain(published.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    auto result =
        publication->finish(phase.query.output.descriptor, phase.query.outputs,
                            values.data(), values.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
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
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.continuation_bytes = sizeof(CurveState);
  output.maximum_dependency_stages = 4;
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
    resolved.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.start_dependency = [pchip, multi, profile](const auto& query,
                                                       const auto& allocator) {
    return DependencyContinuation::make<CurveState>(
        allocator, pchip, multi, profile,
        std::get<std::string>(query.parameters.at("out_of_domain")) == "reject"
            ? 0U
        : std::get<std::string>(query.parameters.at("out_of_domain")) == "clamp"
            ? 1U
            : 2U);
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
