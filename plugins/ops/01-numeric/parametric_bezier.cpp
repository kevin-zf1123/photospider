#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_bezier.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct ParametricPoint {
  std::uint64_t row, column, fragment, offset;
  std::size_t lookup;
};
struct ParametricRow {
  std::uint64_t index, t;
  std::size_t first_point;
  std::uint64_t segment;
  int endpoint;
};
struct ParametricState final {
  SequenceProfile profile;
  unsigned degree, stage = 0;
  numeric_ops::ExactPolynomial arithmetic;
  numeric_ops::ExactSampling sampling;
  ResourceVector<ParametricPoint> points;
  ResourceVector<ParametricRow> rows;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  std::array<std::uint64_t, 4> controls{}, replicas{};
  ParametricState(SequenceProfile selected, unsigned order)
      : profile(selected),
        degree(order),
        arithmetic(selected),
        sampling(selected) {}
  Status failure(const DependencyPhase& phase, const ParametricPoint& point,
                 unsigned port, const std::vector<std::uint64_t>& at,
                 const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{
        (port == 2 || port == 3) ? ErrorCode::InvalidArgument
                                 : ErrorCode::OperationFailed,
        std::string(message) +
            (port == 4 ? "; output="
                       : "; port=" + std::to_string(port) + " coordinate="),
        reason,
        {FailureOrigin::Domain, FailureScope::Atom}};
    for (auto index : at)
      result.message += std::to_string(index) + ",";
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = 2;
    atom.coordinate[0] = point.row;
    atom.coordinate[1] = point.column;
    result.detail.atom = atom;
    return result;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const ParametricPoint& point) {
    auto work = phase.consume_work(phase.inputs[port].fragments().size() + 1);
    if (!work.ok())
      return Result<std::uint64_t>(work);
    std::uint64_t bits = 0;
    const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                        ElementType::Float32;
    auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(phase, point, port, at, "nonfinite parametric input"));
    return Result<std::uint64_t>(numeric_ops::ExactBezier::widen(bits, narrow));
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.parametric/1;exact-Bernstein;%s%s",
        profile == SequenceProfile::Strict         ? "scalar-u64"
        : profile == SequenceProfile::AppleSilicon ? "NEON-u64x2"
                                                   : "AVX2-u64x4",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return {ErrorCode::Internal, "parametric diagnostic identity"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    return phase.report_numeric(result);
  }
  Status initialize(const DependencyPhase& phase) {
    const auto count = phase.query.outputs.element_count().value();
    if (count > phase.sets.maximum_boxes)
      return {ErrorCode::ResourceExhausted, "parametric association capacity",
              FailureReason::CapacityLimit};
    auto status = phase.consume_work(count * 4 + 1);
    if (!status.ok())
      return status;
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), 2);
    points.reserve(count);
    outputs.reserve(phase.query.outputs.boxes().size());
    // Project the bounded rectangle list, not the logical whole output.
    // Query lookup then costs O(P), with O(M) cell association work.
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
        rows.push_back({span.offset + i, 0, SIZE_MAX, 0, -1});
      }
    }
    for (const auto& box : phase.query.outputs.boxes()) {
      auto output = MutableValue::allocate(phase.query.output.descriptor, box,
                                           phase.allocator);
      if (!output.ok())
        return output.status();
      const auto row = box.dimensions()[0];
      const auto col = box.dimensions()[1];
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

  Status declare(const DependencyPhase& phase, unsigned port,
                 std::vector<Region> regions,
                 std::vector<DependencyNeed>* needs) {
    const auto& metadata = phase.query.inputs[port];
    auto data =
        Footprint::from_regions(metadata.descriptor.shape, regions, phase.sets);
    if (!data.ok())
      return data.status();
    auto validation = data.value();
    for (const auto& facet : metadata.facets) {
      if (facet.key != "photospider.image" &&
          facet.key != "photospider.semantic")
        continue;
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return semantic.status();
      if (semantic.value().kind != SemanticKind::Image)
        continue;
      std::vector<Region> closed;
      closed.reserve(regions.size());
      for (const auto& item : regions) {
        auto work = phase.consume_work(3);
        if (!work.ok())
          return work;
        auto dimensions = item.dimensions();
        dimensions[2] = {0, metadata.descriptor.shape[2]};
        closed.emplace_back(std::move(dimensions));
      }
      auto footprint = Footprint::from_regions(metadata.descriptor.shape,
                                               std::move(closed), phase.sets);
      if (!footprint.ok())
        return footprint.status();
      validation = footprint.take_value();
    }
    needs->push_back({port, 1, data.take_value(), {}});
    needs->push_back({port, 4, std::move(validation), {}});
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, bool query) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 16384);
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto work = phase.consume_work(8);
      if (!work.ok())
        return Answer(work);
      std::vector<DependencyNeed> needs;
      if (query) {
        for (unsigned port : {2, 3}) {
          auto footprint =
              Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                      {Region({{point.row, 1}})}, phase.sets);
          if (!footprint.ok())
            return Answer(footprint.status());
          needs.push_back({port, 6, footprint.take_value(), {}});
        }
      } else {
        const auto& row = rows[point.lookup];
        const auto first = row.segment + (row.endpoint == 1 ? 1 : 0);
        auto status = declare(
            phase, 0,
            {Region({{first, row.endpoint < 0 ? 2U : 1U}, {point.column, 1}})},
            &needs);
        if (!status.ok())
          return Answer(status);
        if (row.endpoint < 0) {
          status = declare(
              phase, 1,
              {Region({{row.segment, 1}, {0, degree - 1}, {point.column, 1}})},
              &needs);
          if (!status.ok())
            return Answer(status);
        }
      }
      certificates.push_back({{point.row, point.column}, std::move(needs)});
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  Status classify(const DependencyPhase& phase, ParametricRow* row) {
    const auto& point = points[row->first_point];
    auto work = phase.consume_work(phase.inputs[2].fragments().size() + 1);
    if (!work.ok())
      return work;
    std::int64_t segment = 0;
    auto status = phase.read(2, {row->index}, &segment, 8);
    if (!status.ok())
      return status;
    if (segment < 0 || static_cast<std::uint64_t>(segment) >=
                           phase.query.inputs[0].descriptor.shape[0] - 1)
      return failure(phase, point, 2, {row->index},
                     "parametric segment out of range");
    row->segment = static_cast<std::uint64_t>(segment);
    auto parameter = read(phase, 3, {row->index}, point);
    if (!parameter.ok())
      return parameter.status();
    row->t = parameter.value();
    const auto parts = BinaryParts::decode(row->t, false);
    if ((parts.negative && parts.magnitude) ||
        parts.order_key() > UINT64_C(0xbff0000000000000))
      return failure(phase, point, 3, {row->index},
                     "parametric t outside [0,1]");
    row->endpoint = !parts.magnitude                         ? 0
                    : row->t == UINT64_C(0x3ff0000000000000) ? 1
                                                             : -1;
    return Status::success();
  }
  Result<std::uint64_t> evaluate(const DependencyPhase& phase,
                                 const ParametricPoint& point, bool narrow) {
    const auto& row = rows[point.lookup];
    auto first =
        read(phase, 0,
             {row.segment + (row.endpoint == 1 ? 1 : 0), point.column}, point);
    if (!first.ok())
      return first;
    controls[0] = first.value();
    if (row.endpoint >= 0)
      return sampling.weighted(first.value(), 0, 1, 0, 1, narrow, false,
                               phase.consume_work);
    auto last = read(phase, 0, {row.segment + 1, point.column}, point);
    if (!last.ok())
      return last;
    controls[degree] = last.value();
    for (unsigned h = 0; h + 1 < degree; ++h) {
      auto offset = read(phase, 1, {row.segment, h, point.column}, point);
      if (!offset.ok())
        return offset;
      auto absolute =
          sampling.weighted(h ? controls[degree] : controls[0], offset.value(),
                            1, 1, 1, false, false, phase.consume_work);
      if (!absolute.ok())
        return absolute;
      if (BinaryParts::decode(absolute.value(), false).infinite)
        return Result<std::uint64_t>(
            failure(phase, point, 1, {row.segment, h, point.column},
                    "parametric control reconstruction overflow",
                    FailureReason::ArithmeticOverflow));
      controls[h + 1] = absolute.value();
    }
    arithmetic.begin(phase.consume_work);
    struct End {
      numeric_ops::ExactPolynomial& math;
      ~End() { math.end(); }
    } end{arithmetic};
    auto polynomial = arithmetic.bernstein(controls, degree);
    const auto t = arithmetic.binary(row.t, 1074);
    const auto numerator = arithmetic.evaluate(polynomial, t, 1074);
    const auto denominator =
        arithmetic.shift(arithmetic.integer(1),
                         polynomial.degree > 0 ? polynomial.degree * 1074 : 0);
    auto result = arithmetic.round(numerator, denominator, narrow);
    if (!result.ok())
      return result;
    if (!arithmetic.sign(numerator)) {
      bool negative = true;
      for (unsigned i = 0; i <= degree; ++i)
        negative = negative && controls[i] == (UINT64_C(1) << 63);
      return Result<std::uint64_t>(
          negative ? (UINT64_C(1) << (narrow ? 31 : 63)) : 0);
    }
    return result;
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
      return need(phase, true);
    }
    if (stage == 1) {
      for (auto& row : rows) {
        auto status = classify(phase, &row);
        if (!status.ok())
          return Answer(status);
      }
      stage = 2;
      return need(phase, false);
    }
    const bool narrow =
        phase.query.output.descriptor.element_type == ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    for (const auto& point : points) {
      auto status = report(phase, 1, 0);
      if (!status.ok())
        return Answer(status);
      auto value = evaluate(phase, point, narrow);
      if (!value.ok())
        return Answer(value.status());
      if (BinaryParts::decode(value.value(), narrow).infinite) {
        const auto& row = rows[point.lookup];
        return Answer(failure(
            phase, point, row.endpoint < 0 ? 4 : 0,
            {row.endpoint < 0 ? point.row : row.segment + row.endpoint,
             point.column},
            "parametric output overflow", FailureReason::ArithmeticOverflow));
      }
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
OperationDefinition parametric_operation(const std::string& key,
                                         SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 4;
  traits.input_schema.resize(4);
  for (unsigned p : {0, 1, 3})
    traits.input_schema[p].element_type_mask = 12;
  traits.input_schema[2].element_type =
      static_cast<std::uint32_t>(ElementType::Int64);
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"degree", OperationParameterType::Int64, true, true, 2, 3},
      {"dtype", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.continuation_bytes = sizeof(ParametricState);
  output.maximum_dependency_stages = 3;
  operation.specialize_metadata = [profile](const auto& inputs,
                                            const auto& parameters) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto degree =
        static_cast<unsigned>(std::get<std::int64_t>(parameters.at("degree")));
    const auto& a = inputs[0].descriptor.shape;
    const auto& q = inputs[2].descriptor.shape;
    constexpr auto cap = UINT64_C(1) << 40;
    if (a.size() != 2 || a[0] < 2 || a[0] > 65536 || !a[1] ||
        a[1] > cap / a[0] || a[1] > cap / ((a[0] - 1) * (degree - 1)) ||
        q.size() != 1 || !q[0] || q[0] > cap / a[1] ||
        inputs[3].descriptor.shape != q ||
        inputs[1].descriptor.shape !=
            std::vector<std::uint64_t>{a[0] - 1, degree - 1, a[1]})
      return Answer(Status{
          ErrorCode::TypeMismatch,
          "parametric shapes require K=2..65536, D/N>=1 and products <=2^40",
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(numeric_ops::array_parameter_error("parametric dtype"));
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = {
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        {q[0], a[1]}};
    resolved.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.start_dependency = [profile](const auto& query,
                                         const auto& allocator) {
    return DependencyContinuation::make<ParametricState>(
        allocator, profile,
        static_cast<unsigned>(
            std::get<std::int64_t>(query.parameters.at("degree"))));
  };
  return operation;
}
}  // namespace
Status register_parametric_bezier(OperationRegistry* registry) {
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(parametric_operation(
        std::string("curve.evaluate_bezier") + entry.first, entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
