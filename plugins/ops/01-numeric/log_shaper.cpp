#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_shaper.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
struct ShaperPoint {
  std::array<std::uint64_t, 8> at{};
  std::uint64_t fragment = 0, offset = 0;
};
struct ShaperState {
  SequenceProfile profile;
  bool inverse;
  unsigned stage = 0, rank = 0;
  std::array<std::uint64_t, 2> bounds{};
  numeric_ops::ExactShaper arithmetic;
  ResourceVector<ShaperPoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  ShaperState(SequenceProfile selected, bool reversed)
      : profile(selected), inverse(reversed), arithmetic(selected) {}
  std::vector<std::uint64_t> coordinate(const ShaperPoint& point) const {
    return {point.at.begin(), point.at.begin() + rank};
  }
  Status invalid(const DependencyPhase& phase, unsigned port) const {
    Status result{ErrorCode::InvalidArgument,
                  "InvalidBounds: port=" + std::to_string(port) +
                      " bits=" + std::to_string(bounds[port - 1]),
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = rank;
    atom.coordinate = points.front().at;
    result.detail.atom = atom;
    return result;
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied, bool interval = false) const {
    NumericDiagnostics diagnostic;
    diagnostic.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        diagnostic.implementation.data(), diagnostic.implementation.size(),
        "photospider.shaper/1;%s;exact-certified;%s",
        inverse ? "log2-inverse" : "log2-forward",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostic.implementation.size())
      return {ErrorCode::Internal, "shaper diagnostic identity"};
    diagnostic.evaluated_values = evaluated;
    diagnostic.copied_elements = copied;
    if (interval && profile != SequenceProfile::Strict) {
      diagnostic.strict_fallbacks = 1;
      diagnostic.fallback_reasons[static_cast<unsigned>(
          NumericFallbackReason::FunctionUnsupported)] = 1;
    }
    return phase.report_numeric(diagnostic);
  }
  Status initialize(const DependencyPhase& phase) {
    rank = phase.query.output.descriptor.shape.size();
    auto count = phase.query.outputs.element_count();
    if (!count.ok())
      return count.status();
    auto work = phase.consume_work(count.value() * (rank + 2) + 1);
    if (!work.ok())
      return work;
    points.reserve(count.value());
    outputs.reserve(phase.query.outputs.boxes().size());
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), rank);
    for (const auto& region : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              region, phase.allocator);
      if (!allocated.ok())
        return allocated.status();
      ShaperPoint point;
      point.fragment = outputs.size();
      std::uint64_t elements = 1;
      for (unsigned axis = 0; axis < rank; ++axis) {
        point.at[axis] = region.dimensions()[axis].offset;
        elements *= region.dimensions()[axis].extent;
      }
      for (std::uint64_t i = 0; i < elements; ++i) {
        work = phase.consume_work(1);
        if (!work.ok())
          return work;
        point.offset = i;
        points.push_back(point);
        for (unsigned axis = rank; axis; --axis) {
          const auto dimension = region.dimensions()[axis - 1];
          if (++point.at[axis - 1] < dimension.offset + dimension.extent)
            break;
          point.at[axis - 1] = dimension.offset;
        }
      }
      outputs.push_back(allocated.take_value());
    }
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase,
                              bool scalar_bounds) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 16384);
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto work = phase.consume_work(16);
      if (!work.ok())
        return Answer(work);
      AtomCertificate certificate{coordinate(point), {}};
      for (unsigned port = scalar_bounds ? 1 : 0;
           port < (scalar_bounds ? 3U : 1U); ++port) {
        std::vector<RegionDimension> dimensions;
        if (scalar_bounds)
          dimensions = {{0, 1}};
        else
          for (auto value : coordinate(point))
            dimensions.push_back({value, 1});
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
        certificate.inputs.push_back(
            {port,
             static_cast<std::uint8_t>(scalar_bounds ? 2 : 1),
             support.take_value(),
             {}});
        certificate.inputs.push_back({port, 4, closure.take_value(), {}});
      }
      certificates.push_back(std::move(certificate));
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(32768);
    request_capacity.reset();
    const bool narrow =
        phase.query.output.descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    if (!stage) {
      auto status = initialize(phase);
      if (!status.ok())
        return Answer(status);
      stage = 1;
      return need(phase, true);
    }
    if (stage == 1) {
      std::array<BinaryParts, 2> parts{};
      for (unsigned i = 0; i < 2; ++i) {
        auto work =
            phase.consume_work(phase.inputs[i + 1].fragments().size() + 1);
        if (!work.ok())
          return Answer(work);
        auto status = phase.read(i + 1, {0}, &bounds[i], width);
        if (!status.ok())
          return Answer(status);
        parts[i] = BinaryParts::decode(bounds[i], narrow);
      }
      for (unsigned i = 0; i < 2; ++i)
        if (parts[i].nan || parts[i].infinite || parts[i].negative ||
            !parts[i].magnitude)
          return Answer(invalid(phase, i + 1));
      if (parts[0].order_key() >= parts[1].order_key())
        return Answer(invalid(phase, 1));
      stage = 2;
      return need(phase, false);
    }
    for (const auto& point : points) {
      auto work = phase.consume_work(phase.inputs[0].fragments().size() + 1);
      if (!work.ok())
        return Answer(work);
      std::uint64_t input = 0;
      auto status = phase.read(0, coordinate(point), &input, width);
      if (!status.ok())
        return Answer(status);
      status = report(phase, 1, 0);
      if (!status.ok())
        return Answer(status);
      auto result = arithmetic.evaluate(
          input, bounds[0], bounds[1], inverse, narrow, phase.consume_work,
          [&] { return report(phase, 0, 0, true); });
      if (!result.ok())
        return Answer(result.status());
      const auto bits = result.value();
      std::memcpy(static_cast<std::uint8_t*>(outputs[point.fragment].data()) +
                      point.offset * width,
                  &bits, width);
      status = report(phase, 0, 1);
      if (!status.ok())
        return Answer(status);
    }
    std::vector<Value> values;
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
OperationDefinition operation(const std::string& key, bool inverse,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::PreserveFirstInput;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ShaperState);
  output.maximum_dependency_stages = 3;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  result.validate_dependency = [profile](const auto& inputs, const auto&) {
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 3 || inputs[0].descriptor.shape.empty() ||
        inputs[0].descriptor.shape.size() > 8)
      return mismatch("shaper requires rank-1..8 input and two bounds");
    const auto type = inputs[0].descriptor.element_type;
    if (type != ElementType::Float32 && type != ElementType::Float64)
      return mismatch("shaper requires Float32/64");
    for (unsigned i = 1; i < 3; ++i)
      if (inputs[i].descriptor.element_type != type ||
          inputs[i].descriptor.shape != std::vector<std::uint64_t>{1})
        return mismatch("shaper bounds require matching dtype and shape [1]");
    std::uint64_t count = 1;
    for (auto extent : inputs[0].descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return mismatch("shaper logical product exceeds 2^40 values");
      count *= extent;
    }
    return numeric_ops::sequence_profile_available(profile);
  };
  result.start_dependency = [inverse, profile](const auto&,
                                               const auto& allocator) {
    return DependencyContinuation::make<ShaperState>(allocator, profile,
                                                     inverse);
  };
  return result;
}
}  // namespace
Status register_log_shapers(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool inverse : {false, true}) {
      auto status = registry->register_operation(
          operation(std::string(inverse ? "curve.log2_shaper_inverse"
                                        : "curve.log2_shaper") +
                        profile.first,
                    inverse, profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
