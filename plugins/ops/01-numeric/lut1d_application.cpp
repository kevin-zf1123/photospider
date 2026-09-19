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
#include "01-numeric/exact_curve.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/uniform_axis.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct LutPoint {
  std::array<std::uint64_t, 8> at{};
  std::uint64_t fragment = 0, offset = 0, query = 0;
  unsigned first = 0, count = 0;
};
struct LutState final {
  SequenceProfile profile;
  bool channels;
  unsigned policy, stage = 0;
  numeric_ops::UniformAxis axis;
  numeric_ops::ExactCurve arithmetic;
  ResourceVector<LutPoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  std::array<std::uint64_t, 4> x{}, y{}, replicas{};
  LutState(SequenceProfile selected, bool multi, unsigned domain)
      : profile(selected),
        channels(multi),
        policy(domain),
        axis(selected),
        arithmetic(selected) {}
  std::vector<std::uint64_t> coordinate(const DependencyPhase& phase,
                                        const LutPoint& point) const {
    return {point.at.begin(),
            point.at.begin() + phase.query.output.descriptor.shape.size()};
  }
  Status failure(const DependencyPhase& phase, const LutPoint& point,
                 const std::string& message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = phase.query.output.descriptor.shape.size();
    atom.coordinate = point.at;
    result.detail.atom = atom;
    return result;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const LutPoint& point) {
    auto work =
        phase.consume_work(phase.inputs[port].fragments().size() + at.size());
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
          "nonfinite LUT1D port=" + std::to_string(port) + " coordinate=";
      for (auto index : at)
        message += std::to_string(index) + ",";
      return Result<std::uint64_t>(failure(phase, point, message));
    }
    return Result<std::uint64_t>(numeric_ops::ExactBezier::widen(bits, narrow));
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.lut1d/1;exact-linear;%s%s",
        profile == SequenceProfile::Strict         ? "scalar-u64"
        : profile == SequenceProfile::AppleSilicon ? "NEON-u64x2"
                                                   : "AVX2-u64x4",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return {ErrorCode::Internal, "LUT1D diagnostic identity"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    return phase.report_numeric(result);
  }
  Status initialize(const DependencyPhase& phase) {
    const auto count = phase.query.outputs.element_count().value();
    if (count > phase.sets.maximum_boxes)
      return {ErrorCode::ResourceExhausted, "LUT1D association capacity",
              FailureReason::CapacityLimit};
    auto work = phase.consume_work(count + 1);
    if (!work.ok())
      return work;
    const auto rank = phase.query.output.descriptor.shape.size();
    points.reserve(count);
    outputs.reserve(phase.query.outputs.boxes().size());
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), rank);
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              box, phase.allocator);
      if (!allocated.ok())
        return allocated.status();
      auto selected = Footprint::from_regions(
          phase.query.output.descriptor.shape, {box}, phase.sets);
      if (!selected.ok())
        return selected.status();
      std::uint64_t offset = 0;
      auto status = selected.value().visit(
          [&](const auto& at) {
            auto charged = phase.consume_work(rank);
            if (!charged.ok())
              return charged;
            LutPoint point;
            std::copy(at.begin(), at.end(), point.at.begin());
            point.fragment = outputs.size();
            point.offset = offset++;
            points.push_back(point);
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return status;
      outputs.push_back(allocated.take_value());
    }
    return Status::success();
  }
  Status declare(const DependencyPhase& phase, unsigned port, unsigned role,
                 std::vector<RegionDimension> dimensions,
                 std::vector<DependencyNeed>* needs) {
    const auto& input = phase.query.inputs[port];
    auto data = Footprint::from_regions(input.descriptor.shape,
                                        {Region(dimensions)}, phase.sets);
    if (!data.ok())
      return data.status();
    auto validation = data.value();
    for (const auto& facet : input.facets) {
      if (facet.key != "photospider.image" &&
          facet.key != "photospider.semantic")
        continue;
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return semantic.status();
      if (semantic.value().kind == SemanticKind::Image) {
        dimensions[2] = {0, input.descriptor.shape[2]};
        auto closure = Footprint::from_regions(
            input.descriptor.shape, {Region(dimensions)}, phase.sets);
        if (!closure.ok())
          return closure.status();
        validation = closure.take_value();
      }
    }
    needs->push_back(
        {port, static_cast<std::uint8_t>(role), data.take_value(), {}});
    needs->push_back({port, 4, std::move(validation), {}});
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, unsigned port) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 16384);
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto charged = phase.consume_work(16);
      if (!charged.ok())
        return Answer(charged);
      std::vector<RegionDimension> dimensions;
      if (port == 2) {
        dimensions = {{0, 3}};
      } else if (port == 0) {
        for (auto at : coordinate(phase, point))
          dimensions.push_back({at, 1});
      } else {
        dimensions = {{point.first, point.count}};
        if (channels)
          dimensions.push_back(
              {point.at[phase.query.output.descriptor.shape.size() - 1], 1});
      }
      std::vector<DependencyNeed> needs;
      auto status = declare(phase, port, port == 1 ? 1 : 2,
                            std::move(dimensions), &needs);
      if (!status.ok())
        return Answer(status);
      certificates.push_back({coordinate(phase, point), std::move(needs)});
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  Status classify(const DependencyPhase& phase, LutPoint* point) {
    auto value = read(phase, 0, coordinate(phase, *point), *point);
    if (!value.ok())
      return value.status();
    point->query = value.value();
    const auto key = axis.key(point->query);
    const auto size = static_cast<unsigned>(axis.knots.size());
    unsigned lo = 0, hi = size;
    while (lo < hi) {
      auto work = phase.consume_work(1);
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
        return failure(phase, *point, "LUT1D query outside axis domain");
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
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(65536);
    request_capacity.reset();
    if (!stage) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
      stage = 1;
      return need(phase, 2);
    }
    if (stage == 1) {
      std::array<std::uint64_t, 3> values{};
      for (unsigned j = 0; j < 3; ++j) {
        auto value = read(phase, 2, {j}, points.front());
        if (!value.ok())
          return Answer(value.status());
        values[j] = value.value();
      }
      auto status = axis.validate(
          values,
          static_cast<unsigned>(phase.query.inputs[1].descriptor.shape[0]),
          phase.consume_work);
      if (!status.ok()) {
        if (status.code == ErrorCode::OperationFailed)
          return Answer(
              failure(phase, points.front(), status.message, status.reason));
        return Answer(status);
      }
      stage = 2;
      return need(phase, 0);
    }
    if (stage == 2) {
      for (auto& point : points) {
        auto status = classify(phase, &point);
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
      for (unsigned j = 0; j < point.count; ++j) {
        std::vector<std::uint64_t> at{point.first + j};
        if (channels)
          at.push_back(
              point.at[phase.query.output.descriptor.shape.size() - 1]);
        auto value = read(phase, 1, at, point);
        if (!value.ok())
          return Answer(value.status());
        x[j] = axis.knots[point.first + j];
        y[j] = value.value();
      }
      // ExactCurve's rational denominator is positive; reorder both members
      // of a descending pair before evaluating the unchanged mathematical line.
      if (point.count == 2 && axis.descending) {
        std::swap(x[0], x[1]);
        std::swap(y[0], y[1]);
      }
      auto status = report(phase, 1, 0);
      if (!status.ok())
        return Answer(status);
      auto value = arithmetic.evaluate(false, 2, 0, point.count, 0,
                                       point.count == 1 ? 0 : -1, point.query,
                                       x, y, narrow, phase.consume_work);
      if (!value.ok())
        return Answer(value.status());
      if (BinaryParts::decode(value.value(), narrow).infinite)
        return Answer(failure(phase, point, "LUT1D output conversion overflow",
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
      auto value = std::move(output).publish();
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication->retain(value.take_value());
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
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.continuation_bytes = sizeof(LutState);
  output.maximum_dependency_stages = 4;
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
    resolved.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  operation.start_dependency = [channels, profile](const auto& query,
                                                   const auto& allocator) {
    const auto& policy =
        std::get<std::string>(query.parameters.at("out_of_domain"));
    return DependencyContinuation::make<LutState>(allocator, profile, channels,
                                                  policy == "reject"  ? 0U
                                                  : policy == "clamp" ? 1U
                                                                      : 2U);
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
