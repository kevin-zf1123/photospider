#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/lowpass_uniform_math.hpp"
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
  result.radius =
      raw(static_cast<double>(std::get<std::int64_t>(input.at("radius"))));
  if (kernel == LowpassKernel::Gaussian)
    result.sigma = raw(std::get<double>(input.at("sigma")));
  else
    result.cutoff = raw(std::get<double>(input.at("cutoff")));
  if (kernel == LowpassKernel::Kaiser)
    result.beta = raw(std::get<double>(input.at("beta")));
  return result;
}
struct UniformState {
  LowpassParameters parameters;
  unsigned axis, radius;
  std::string boundary;
  SequenceProfile profile;
  numeric_ops::UniformLowpassMath arithmetic;
  ResourceVector<std::uint64_t> samples;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  bool requested = false;
  UniformState(LowpassParameters p, unsigned dimension, unsigned count,
               std::string extension, SequenceProfile selected)
      : parameters(p),
        axis(dimension),
        radius(count),
        boundary(std::move(extension)),
        profile(selected) {}
  std::optional<std::uint64_t> mapped(std::uint64_t center, int offset,
                                      std::uint64_t size) const {
    const auto at = static_cast<std::int64_t>(center) + offset;
    if (at >= 0 && static_cast<std::uint64_t>(at) < size)
      return static_cast<std::uint64_t>(at);
    if (boundary == "zero")
      return {};
    if (boundary == "replicate")
      return at < 0 ? 0 : size - 1;
    if (size == 1)
      return 0;
    const auto period =
        static_cast<std::int64_t>(boundary == "wrap" ? size : 2 * (size - 1));
    auto reduced = at % period;
    if (reduced < 0)
      reduced += period;
    return boundary == "reflect" && static_cast<std::uint64_t>(reduced) >= size
               ? period - reduced
               : reduced;
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.lowpass-uniform/1;certified-whole-sum;%s",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return {ErrorCode::Internal, "lowpass diagnostic identity"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    if (evaluated && profile != SequenceProfile::Strict) {
      result.strict_fallbacks = evaluated;
      result.fallback_reasons[static_cast<unsigned>(
          NumericFallbackReason::FunctionUnsupported)] = evaluated;
    }
    return phase.report_numeric(result);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    const auto& descriptor = phase.query.output.descriptor;
    const auto rank = descriptor.shape.size();
    if (!requested) {
      const auto count = phase.query.outputs.element_count().value();
      request_capacity = dependency_internal::metadata_owner(
          4096 + count * (8192 + (2 * radius + 1) * rank * 128));
      std::vector<AtomCertificate> rows;
      rows.reserve(count);
      auto status = phase.query.outputs.visit(
          [&](const auto& coordinate) {
            std::vector<Region> boxes;
            boxes.reserve(2 * radius + 1);
            std::vector<RegionDimension> dimensions;
            for (auto value : coordinate)
              dimensions.push_back({value, 1});
            for (int offset = -static_cast<int>(radius);
                 offset <= static_cast<int>(radius); ++offset) {
              auto charged = phase.consume_work(rank * 4 + 1);
              if (!charged.ok())
                return charged;
              if (!numeric_ops::uniform_tap_sign(parameters, radius,
                                                 offset < 0 ? -offset : offset))
                continue;
              auto index =
                  mapped(coordinate[axis], offset, descriptor.shape[axis]);
              if (!index)
                continue;
              dimensions[axis] = {*index, 1};
              boxes.emplace_back(dimensions);
            }
            auto support = Footprint::from_regions(
                descriptor.shape, std::move(boxes), phase.sets);
            if (!support.ok())
              return support.status();
            auto closure = input_internal::validation_closure(
                phase.query.inputs[0], support.value(), phase.sets,
                phase.consume_work);
            if (!closure.ok())
              return closure.status();
            rows.push_back({coordinate,
                            {{0, 1, support.take_value(), {}},
                             {0, 4, closure.take_value(), {}}}});
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      requested = true;
      return Answer(DependencyNeedBatch{std::move(rows)});
    }
    request_capacity.reset();
    auto construction = dependency_internal::metadata_owner(65536);
    samples.resize(2 * radius + 1);
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), rank);
    ResourceVector<Value> fragments;
    fragments.reserve(phase.query.outputs.boxes().size());
    const bool narrow = descriptor.element_type == ElementType::Float32;
    const auto width = narrow ? 4U : 8U;
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(descriptor, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      auto selected =
          Footprint::from_regions(descriptor.shape, {box}, phase.sets);
      if (!selected.ok())
        return Answer(selected.status());
      std::uint64_t written = 0;
      auto status = selected.value().visit(
          [&](const auto& coordinate) {
            auto at = coordinate;
            for (int offset = -static_cast<int>(radius);
                 offset <= static_cast<int>(radius); ++offset) {
              auto charged = phase.consume_work(
                  rank * (phase.inputs[0].fragments().size() + 1));
              if (!charged.ok())
                return charged;
              auto& bits = samples[offset + radius];
              bits = 0;
              if (!numeric_ops::uniform_tap_sign(parameters, radius,
                                                 offset < 0 ? -offset : offset))
                continue;
              auto source =
                  mapped(coordinate[axis], offset, descriptor.shape[axis]);
              if (!source)
                continue;
              at[axis] = *source;
              auto read = phase.read(0, at, &bits, width);
              if (!read.ok())
                return read;
            }
            auto charged = report(phase, 1, 0);
            if (!charged.ok())
              return charged;
            auto value = arithmetic.evaluate(parameters, radius, samples.data(),
                                             narrow, phase.consume_work);
            if (!value.ok())
              return value.status();
            auto bits = value.value();
            std::memcpy(writer.data() + written * width, &bits, width);
            ++written;
            return report(phase, 0, 1);
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      auto value = std::move(writer).publish();
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
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type_mask = 12;
  traits.parameter_schema = {{"axis", OperationParameterType::Int64},
                             {"radius", OperationParameterType::Int64},
                             {"boundary", OperationParameterType::String}};
  traits.parameter_schema.push_back(
      {kernel == LowpassKernel::Gaussian ? "sigma" : "cutoff",
       OperationParameterType::Float64});
  if (kernel == LowpassKernel::Kaiser)
    traits.parameter_schema.push_back(
        {"beta", OperationParameterType::Float64});
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(UniformState);
  output.maximum_dependency_stages = 2;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  definition.specialize_metadata = [profile](const auto& inputs,
                                             const auto& p) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    if (inputs.size() != 1 || inputs[0].result_schema ||
        (inputs[0].descriptor.element_type != ElementType::Float32 &&
         inputs[0].descriptor.element_type != ElementType::Float64))
      return Answer(Status{ErrorCode::TypeMismatch,
                           "lowpass input requires Float32/64",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& shape = inputs[0].descriptor.shape;
    std::uint64_t count = 1;
    if (shape.empty() || shape.size() > 8)
      return Answer(numeric_ops::array_parameter_error("lowpass rank 1..8"));
    for (auto extent : shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(numeric_ops::array_parameter_error(
            "lowpass positive shape within 2^40"));
      count *= extent;
    }
    const auto axis = std::get<std::int64_t>(p.at("axis")),
               radius = std::get<std::int64_t>(p.at("radius"));
    const auto& boundary = std::get<std::string>(p.at("boundary"));
    if (axis < 0 || static_cast<std::uint64_t>(axis) >= shape.size() ||
        radius < 1 || radius > 4096 ||
        (boundary != "reflect" && boundary != "replicate" &&
         boundary != "zero" && boundary != "wrap"))
      return Answer(
          numeric_ops::array_parameter_error("lowpass axis/radius/boundary"));
    for (const auto& field : p) {
      if (!std::holds_alternative<double>(field.second))
        continue;
      const auto value =
          BinaryParts::decode(raw(std::get<double>(field.second)), false);
      if (value.nan || value.infinite || (value.negative && value.magnitude) ||
          (field.first != "beta" && !value.magnitude) ||
          (field.first == "cutoff" &&
           value.magnitude >= UINT64_C(0x3fe0000000000000)))
        return Answer(numeric_ops::array_parameter_error(
            "lowpass finite positive kernel parameter (beta>=0, cutoff<0.5)"));
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.descriptor = inputs[0].descriptor;
    resolved.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  definition.start_dependency = [kernel, profile](const auto& query,
                                                  const auto& allocator) {
    return DependencyContinuation::make<UniformState>(
        allocator, parameters(kernel, query.parameters),
        static_cast<unsigned>(
            std::get<std::int64_t>(query.parameters.at("axis"))),
        static_cast<unsigned>(
            std::get<std::int64_t>(query.parameters.at("radius"))),
        std::get<std::string>(query.parameters.at("boundary")), profile);
  };
  return definition;
}
}  // namespace
Status register_uniform_lowpass(OperationRegistry* registry) {
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
      auto status = registry->register_operation(operation(
          std::string("curve.lowpass_uniform_") + kernel.first + profile.first,
          kernel.second, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
