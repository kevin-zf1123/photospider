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
#include "01-numeric/lowpass_nonuniform_math.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::LowpassKernel;
using numeric_ops::LowpassParameters;
using numeric_ops::SequenceProfile;
std::uint64_t raw(double value) {
  std::uint64_t result = 0;
  std::memcpy(&result, &value, 8);
  return result;
}
LowpassParameters parameters(
    LowpassKernel kernel, const std::map<std::string, ParameterValue>& input) {
  LowpassParameters result{kernel};
  result.radius = raw(std::get<double>(input.at("support_radius")));
  if (kernel == LowpassKernel::Gaussian)
    result.sigma = raw(std::get<double>(input.at("sigma")));
  else
    result.cutoff = raw(std::get<double>(input.at("cutoff")));
  if (kernel == LowpassKernel::Kaiser)
    result.beta = raw(std::get<double>(input.at("beta")));
  return result;
}
struct NonuniformPoint {
  std::array<std::uint64_t, 8> coordinate{};
  std::uint64_t fragment = 0, offset = 0, begin = 0, end = 0;
};
struct NonuniformState {
  LowpassParameters parameters;
  unsigned axis, stage = 0;
  std::string boundary;
  SequenceProfile profile;
  numeric_ops::NonuniformLowpassMath arithmetic;
  ResourceVector<std::uint64_t> positions;
  ResourceVector<NonuniformPoint> points;
  ResourceVector<numeric_ops::LowpassPiece> pieces;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  NonuniformState(LowpassParameters p, unsigned dimension,
                  std::string extension, SequenceProfile selected)
      : parameters(p),
        axis(dimension),
        boundary(std::move(extension)),
        profile(selected) {}
  Status failure(const DependencyPhase& phase, const NonuniformPoint& point,
                 const char* message,
                 FailureReason reason = FailureReason::InvalidDomain) {
    Status result{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = phase.query.output.descriptor.shape.size();
    atom.coordinate = point.coordinate;
    result.detail.atom = atom;
    return result;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const NonuniformPoint& point) {
    auto charged = phase.consume_work(
        at.size() * (phase.inputs[port].fragments().size() + 1));
    if (!charged.ok())
      return Result<std::uint64_t>(charged);
    const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                        ElementType::Float32;
    std::uint64_t bits = 0;
    auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    auto value = BinaryParts::decode(bits, narrow);
    if (value.nan || value.infinite)
      return Result<std::uint64_t>(
          failure(phase, point, "nonfinite nonuniform lowpass input"));
    if (!port && narrow) {
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
                std::uint64_t copied) {
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    auto length = std::snprintf(
        diagnostics.implementation.data(), diagnostics.implementation.size(),
        "photospider.lowpass-nonuniform/1;certified-integral;%s",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostics.implementation.size())
      return {ErrorCode::Internal, "nonuniform lowpass diagnostic identity"};
    diagnostics.evaluated_values = evaluated;
    diagnostics.copied_elements = copied;
    if (evaluated && profile != SequenceProfile::Strict) {
      diagnostics.strict_fallbacks = evaluated;
      diagnostics.fallback_reasons[static_cast<unsigned>(
          NumericFallbackReason::FunctionUnsupported)] = evaluated;
    }
    return phase.report_numeric(diagnostics);
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, bool values) {
    using Answer = Result<DependencyPoll>;
    const auto rank = phase.query.output.descriptor.shape.size();
    request_capacity = dependency_internal::metadata_owner(
        4096 + points.size() * 8192 + pieces.size() * rank * 512);
    std::vector<AtomCertificate> rows;
    rows.reserve(points.size());
    const unsigned port = values ? 1 : 0;
    for (const auto& point : points) {
      auto charged = phase.consume_work(16);
      if (!charged.ok())
        return Answer(charged);
      std::vector<std::uint64_t> coordinate(point.coordinate.begin(),
                                            point.coordinate.begin() + rank);
      std::vector<Region> boxes;
      if (values) {
        boxes.reserve(2 * (point.end - point.begin));
        std::vector<RegionDimension> dimensions;
        for (auto value : coordinate)
          dimensions.push_back({value, 1});
        for (auto i = point.begin; i < point.end; ++i)
          for (auto source : {pieces[i].first, pieces[i].last}) {
            charged = phase.consume_work(rank + 1);
            if (!charged.ok())
              return Answer(charged);
            if (source == UINT64_MAX)
              continue;
            dimensions[axis] = {source, 1};
            boxes.emplace_back(dimensions);
          }
      } else {
        boxes.emplace_back(std::vector<RegionDimension>{{0, positions.size()}});
      }
      auto support =
          Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                  std::move(boxes), phase.sets);
      if (!support.ok())
        return Answer(support.status());
      auto closure = input_internal::validation_closure(
          phase.query.inputs[port], support.value(), phase.sets,
          phase.consume_work);
      if (!closure.ok())
        return Answer(closure.status());
      rows.push_back({std::move(coordinate),
                      {{port, values ? 1U : 2U, support.take_value(), {}},
                       {port, 4, closure.take_value(), {}}}});
    }
    return Answer(DependencyNeedBatch{std::move(rows)});
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    auto construction = dependency_internal::metadata_owner(65536);
    request_capacity.reset();
    const auto& descriptor = phase.query.output.descriptor;
    const auto rank = descriptor.shape.size();
    if (!stage) {
      positions.resize(phase.query.inputs[0].descriptor.shape[0]);
      points.reserve(phase.query.outputs.element_count().value());
      std::uint64_t fragment = 0;
      for (const auto& box : phase.query.outputs.boxes()) {
        auto selected =
            Footprint::from_regions(descriptor.shape, {box}, phase.sets);
        if (!selected.ok())
          return Answer(selected.status());
        std::uint64_t offset = 0;
        auto status = selected.value().visit(
            [&](const auto& at) {
              auto charged = phase.consume_work(rank + 1);
              if (!charged.ok())
                return charged;
              NonuniformPoint point;
              std::copy(at.begin(), at.end(), point.coordinate.begin());
              point.fragment = fragment;
              point.offset = offset++;
              points.push_back(point);
              return Status::success();
            },
            phase.sets.maximum_work, phase.query.cancellation);
        if (!status.ok())
          return Answer(status);
        ++fragment;
      }
      stage = 1;
      return need(phase, false);
    }
    if (stage == 1) {
      for (unsigned i = 0; i < positions.size(); ++i) {
        auto value = read(phase, 0, {i}, points.front());
        if (!value.ok())
          return Answer(value.status());
        positions[i] = value.value();
        if (i && BinaryParts::decode(positions[i - 1], false).order_key() >=
                     BinaryParts::decode(positions[i], false).order_key())
          return Answer(
              failure(phase, points.front(),
                      "nonuniform positions require strict increase"));
      }
      auto& m = arithmetic.kernel.functions.math;
      m.used = 0;
      m.precision = 1074;
      m.consume = &phase.consume_work;
      struct End {
        numeric_ops::DirectedInterval& m;
        ~End() {
          m.consume = nullptr;
          m.used = 0;
        }
      } end{m};
      try {
        numeric_ops::LowpassGeometry geometry(m);
        for (auto& point : points) {
          point.begin = pieces.size();
          geometry.partition(&pieces, positions.data(), positions.size(),
                             point.coordinate[axis], parameters.radius,
                             boundary);
          point.end = pieces.size();
        }
      } catch (const Status& status) {
        return Answer(status);
      }
      stage = 2;
      return need(phase, true);
    }
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), rank);
    ResourceVector<MutableValue> outputs;
    outputs.reserve(phase.query.outputs.boxes().size());
    for (const auto& box : phase.query.outputs.boxes()) {
      auto made = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!made.ok())
        return Answer(made.status());
      outputs.push_back(made.take_value());
    }
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const unsigned width = narrow ? 4 : 8;
    // A bounded per-observation copy lets the numerical evaluator own exactly
    // its contiguous interval range. Growth overlap is charged by
    // ResourceVector.
    ResourceVector<numeric_ops::LowpassPiece> selected;
    for (const auto& point : points) {
      std::vector<std::uint64_t> at(point.coordinate.begin(),
                                    point.coordinate.begin() + rank);
      selected.clear();
      selected.reserve(point.end - point.begin);
      for (auto i = point.begin; i < point.end; ++i) {
        auto charged =
            phase.consume_work(4 * numeric_ops::DirectedInterval::kWords);
        if (!charged.ok())
          return Answer(charged);
        selected.push_back(pieces[i]);
        auto& piece = selected.back();
        if (piece.first != UINT64_MAX) {
          at[axis] = piece.first;
          auto a = read(phase, 1, at, point);
          if (!a.ok())
            return Answer(a.status());
          piece.first_value = a.value();
          at[axis] = piece.last;
          auto b = read(phase, 1, at, point);
          if (!b.ok())
            return Answer(b.status());
          piece.last_value = b.value();
        }
      }
      auto status = report(phase, 1, 0);
      if (!status.ok())
        return Answer(status);
      auto value =
          arithmetic.evaluate(selected, positions[point.coordinate[axis]],
                              parameters, narrow, phase.consume_work);
      if (!value.ok())
        return Answer(value.status());
      const auto bits = value.value();
      if (BinaryParts::decode(bits, narrow).infinite)
        return Answer(failure(phase, point,
                              "nonuniform lowpass output overflow",
                              FailureReason::ArithmeticOverflow));
      std::memcpy(outputs[point.fragment].data() + point.offset * width, &bits,
                  width);
      status = report(phase, 0, 1);
      if (!status.ok())
        return Answer(status);
    }
    ResourceVector<Value> fragments;
    fragments.reserve(outputs.size());
    for (auto& output : outputs) {
      auto status = phase.consume_work(1);
      if (!status.ok())
        return Answer(status);
      auto value = std::move(output).publish();
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication.retain(value.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      fragments.push_back(retained.take_value());
    }
    auto published =
        publication.finish(descriptor, phase.query.outputs, fragments.data(),
                           fragments.size(), phase.sets);
    return published.ok() ? Answer(published.take_value())
                          : Answer(published.status());
  }
};
OperationDefinition operation(const std::string& name, LowpassKernel kernel,
                              SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = name;
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  traits.parameter_schema = {
      {"axis", OperationParameterType::Int64},
      {"support_radius", OperationParameterType::Float64},
      {"boundary", OperationParameterType::String},
      {kernel == LowpassKernel::Gaussian ? "sigma" : "cutoff",
       OperationParameterType::Float64}};
  if (kernel == LowpassKernel::Kaiser)
    traits.parameter_schema.push_back(
        {"beta", OperationParameterType::Float64});
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "samples";
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(NonuniformState);
  output.maximum_dependency_stages = 3;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  definition.specialize_metadata = [profile](const auto& inputs,
                                             const auto& p) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto mismatch = [](const char* message) {
      return Status{ErrorCode::TypeMismatch,
                    message,
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    };
    if (inputs.size() != 2)
      return Answer(mismatch("nonuniform lowpass ports"));
    for (const auto& input : inputs)
      if (input.result_schema ||
          (input.descriptor.element_type != ElementType::Float32 &&
           input.descriptor.element_type != ElementType::Float64))
        return Answer(mismatch("nonuniform lowpass Float32/64 inputs"));
    if (inputs[0].descriptor.shape.size() != 1)
      return Answer(mismatch("nonuniform positions rank one"));
    const auto k = inputs[0].descriptor.shape[0];
    const auto& shape = inputs[1].descriptor.shape;
    const auto axis = std::get<std::int64_t>(p.at("axis"));
    const auto& boundary = std::get<std::string>(p.at("boundary"));
    if (k < 2 || k > 1048576 || shape.empty() || shape.size() > 8 || axis < 0 ||
        static_cast<std::uint64_t>(axis) >= shape.size() ||
        (boundary != "reflect" && boundary != "replicate" &&
         boundary != "zero" && boundary != "wrap"))
      return Answer(numeric_ops::array_parameter_error(
          "nonuniform counts/axis/boundary"));
    if (shape[axis] != k)
      return Answer(mismatch("nonuniform positions/value axis mismatch"));
    std::uint64_t count = 1;
    for (auto size : shape) {
      if (!size || size > (UINT64_C(1) << 40) / count)
        return Answer(
            numeric_ops::array_parameter_error("nonuniform shape count"));
      count *= size;
    }
    for (const auto& entry : p)
      if (const auto* real = std::get_if<double>(&entry.second)) {
        const auto value = BinaryParts::decode(raw(*real), false);
        if (value.nan || value.infinite ||
            (value.negative && value.magnitude) ||
            (!value.magnitude && entry.first != "beta"))
          return Answer(numeric_ops::array_parameter_error(
              "nonuniform finite positive parameters (beta>=0)"));
      }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = inputs[1].descriptor;
    resolved.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  definition.start_dependency = [kernel, profile](const auto& query,
                                                  const auto& allocator) {
    return DependencyContinuation::make<NonuniformState>(
        allocator, parameters(kernel, query.parameters),
        static_cast<unsigned>(
            std::get<std::int64_t>(query.parameters.at("axis"))),
        std::get<std::string>(query.parameters.at("boundary")), profile);
  };
  return definition;
}
}  // namespace
Status register_nonuniform_lowpass(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (const auto& kernel :
         {std::make_pair("hann_sinc", LowpassKernel::Hann),
          std::make_pair("hamming_sinc", LowpassKernel::Hamming),
          std::make_pair("blackman_sinc", LowpassKernel::Blackman),
          std::make_pair("kaiser_sinc", LowpassKernel::Kaiser),
          std::make_pair("gaussian", LowpassKernel::Gaussian)}) {
      auto status = registry->register_operation(
          operation(std::string("curve.lowpass_nonuniform_") + kernel.first +
                        profile.first,
                    kernel.second, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
