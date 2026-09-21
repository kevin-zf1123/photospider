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
struct InversePoint {
  std::uint64_t row = 0, fragment = 0, offset = 0, query = 0;
  unsigned first = 0, count = 0, segment = 0;
  int selected = -1;
};
struct InverseState {
  bool pchip, clamp, increasing = true;
  SequenceProfile profile;
  numeric_ops::ExactCurve arithmetic;
  unsigned stage = 0;
  ResourceVector<std::uint64_t> xs, ys;
  ResourceVector<InversePoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  InverseState(bool cubic, bool clipped, SequenceProfile selected)
      : pchip(cubic), clamp(clipped), profile(selected), arithmetic(selected) {}
  Status failure(const DependencyPhase& phase, const InversePoint& point,
                 unsigned port, std::uint64_t index, const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{ErrorCode::OperationFailed,
                  std::string(message) + "; port=" + std::to_string(port) +
                      " index=" + std::to_string(index),
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = 1;
    atom.coordinate[0] = point.row;
    result.detail.atom = atom;
    return result;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             std::uint64_t index,
                             const InversePoint& point) const {
    auto charged =
        phase.consume_work(phase.inputs[port].fragments().size() + 1);
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                        ElementType::Float32;
    std::uint64_t bits = 0;
    auto status = phase.read(port, {index}, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    const auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(phase, point, port, index, "nonfinite inverse input"));
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
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied, bool fallback = false) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.inverse/2;%s;%s",
        pchip ? "pchip-exact-lattice" : "linear-exact-rational",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return {ErrorCode::Internal, "inverse diagnostic identity"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    if (fallback && profile != SequenceProfile::Strict) {
      result.strict_fallbacks = 1;
      result.fallback_reasons[static_cast<unsigned>(
          NumericFallbackReason::RoundingUnresolved)] = 1;
    }
    return phase.report_numeric(result);
  }
  Status initialize(const DependencyPhase& phase) {
    auto count = phase.query.outputs.element_count();
    if (!count.ok())
      return count.status();
    auto work = phase.consume_work(count.value() * 4 + 1);
    if (!work.ok())
      return work;
    points.reserve(count.value());
    outputs.reserve(phase.query.outputs.boxes().size());
    xs.resize(phase.query.inputs[0].descriptor.shape[0]);
    ys.resize(xs.size());
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), 1);
    for (const auto& region : phase.query.outputs.boxes()) {
      auto made = MutableValue::allocate(phase.query.output.descriptor, region,
                                         phase.allocator);
      if (!made.ok())
        return made.status();
      const auto span = region.dimensions()[0];
      for (std::uint64_t i = 0; i < span.extent; ++i) {
        work = phase.consume_work(1);
        if (!work.ok())
          return work;
        points.push_back({span.offset + i, outputs.size(), i});
      }
      outputs.push_back(made.take_value());
    }
    return Status::success();
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, unsigned kind) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 16384);
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto work = phase.consume_work(16);
      if (!work.ok())
        return Answer(work);
      AtomCertificate certificate{{point.row}, {}};
      for (unsigned port = kind == 1 ? 2 : 0; port < (kind == 1 ? 3U : 2U);
           ++port) {
        const RegionDimension dimension =
            kind == 0   ? RegionDimension{0, xs.size()}
            : kind == 1 ? RegionDimension{point.row, 1}
                        : RegionDimension{point.first, point.count};
        auto support =
            Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                    {Region({dimension})}, phase.sets);
        if (!support.ok())
          return Answer(support.status());
        auto closure = input_internal::validation_closure(
            phase.query.inputs[port], support.value(), phase.sets,
            phase.consume_work);
        if (!closure.ok())
          return Answer(closure.status());
        certificate.inputs.push_back(
            {port,
             static_cast<std::uint8_t>(kind == 2 ? 1 : 2),
             support.take_value(),
             {}});
        certificate.inputs.push_back({port, 4, closure.take_value(), {}});
      }
      certificates.push_back(std::move(certificate));
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  std::uint64_t key(std::uint64_t bits) const {
    const auto ordered = BinaryParts::decode(bits, false).order_key();
    return increasing ? ordered : UINT64_MAX - ordered;
  }
  Status classify(const DependencyPhase& phase, InversePoint* point) {
    auto query = read(phase, 2, point->row, *point);
    if (!query.ok())
      return query.status();
    point->query = query.value();
    const auto wanted = key(point->query);
    unsigned lo = 0, hi = ys.size();
    while (lo < hi) {
      auto work = phase.consume_work(1);
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
        return failure(phase, *point, 2, point->row,
                       "inverse query outside domain");
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
      for (unsigned i = 0; i < xs.size(); ++i) {
        auto x = read(phase, 0, i, points.front()),
             y = read(phase, 1, i, points.front());
        if (!x.ok() || !y.ok())
          return Answer(!x.ok() ? x.status() : y.status());
        xs[i] = x.value();
        ys[i] = y.value();
        if (i && BinaryParts::decode(xs[i - 1], false).order_key() >=
                     BinaryParts::decode(xs[i], false).order_key())
          return Answer(failure(phase, points.front(), 0, i,
                                "inverse x requires strict increase"));
        if (i == 1)
          increasing = BinaryParts::decode(ys[0], false).order_key() <
                       BinaryParts::decode(ys[1], false).order_key();
        if (i && key(ys[i - 1]) >= key(ys[i]))
          return Answer(failure(phase, points.front(), 1, i,
                                "inverse y requires strict monotonicity"));
      }
      stage = 2;
      return need(phase, 1);
    }
    if (stage == 2) {
      for (auto& point : points) {
        auto status = classify(phase, &point);
        if (!status.ok())
          return Answer(status);
      }
      stage = 3;
      return need(phase, 2);
    }
    const bool narrow =
        phase.query.output.descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    for (const auto& point : points) {
      std::array<std::uint64_t, 4> x{}, y{};
      for (unsigned i = 0; i < point.count; ++i) {
        auto a = read(phase, 0, point.first + i, point),
             b = read(phase, 1, point.first + i, point);
        if (!a.ok() || !b.ok())
          return Answer(!a.ok() ? a.status() : b.status());
        x[i] = a.value();
        y[i] = b.value();
      }
      const bool known_fallback =
          !narrow && point.selected < 0 && profile != SequenceProfile::Strict;
      auto status = report(phase, 1, 0, known_fallback);
      if (!status.ok())
        return Answer(status);
      auto computed = arithmetic.inverse(
          pchip, xs.size(), point.first, point.count, point.segment,
          point.selected, point.query, x, y, narrow, phase.consume_work, [&] {
            return known_fallback ? Status::success()
                                  : report(phase, 0, 0, true);
          });
      if (!computed.ok())
        return Answer(computed.status());
      const auto bits = computed.value();
      if (BinaryParts::decode(bits, narrow).infinite)
        return Answer(failure(phase, point, 2, point.row,
                              "inverse output overflow",
                              FailureReason::ArithmeticOverflow));
      std::memcpy(outputs[point.fragment].data() + point.offset * width, &bits,
                  width);
      status = report(phase, 0, 1);
      if (!status.ok())
        return Answer(status);
    }
    ResourceVector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto charged = phase.consume_work(1);
      if (!charged.ok())
        return Answer(charged);
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
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(InverseState);
  output.maximum_dependency_stages = 4;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
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
    resolved.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  definition.start_dependency = [pchip, profile](const auto& query,
                                                 const auto& allocator) {
    return DependencyContinuation::make<InverseState>(
        allocator, pchip,
        std::get<std::string>(query.parameters.at("out_of_domain")) == "clamp",
        profile);
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
