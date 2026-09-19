#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_dot.hpp"
#include "photospider/data/semantic.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Result<ValueDescriptor> metadata(const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<ValueDescriptor>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const auto& vectors = inputs[0].descriptor;
  const auto& matrix = inputs[1].descriptor;
  const auto& bias = inputs[2].descriptor;
  if (vectors.element_type != matrix.element_type ||
      vectors.element_type != bias.element_type ||
      (vectors.element_type != ElementType::Float32 &&
       vectors.element_type != ElementType::Float64))
    return mismatch("matrix transform requires identical Float32/64 inputs");
  if (vectors.shape.empty() || vectors.shape.size() > 8 ||
      matrix.shape.size() != 2 || bias.shape.size() != 1)
    return mismatch("invalid vector/matrix/bias rank");
  const auto input_components = vectors.shape.back(),
             output_components = matrix.shape[0];
  if (input_components < 2 || input_components > 4 || output_components < 2 ||
      output_components > 4 || matrix.shape[1] != input_components ||
      bias.shape[0] != output_components)
    return mismatch("matrix transform requires matching Cin/Cout in 2..4");
  ValueDescriptor output = vectors;
  output.shape.back() = output_components;
  std::uint64_t source_count = 1, target_count = 1;
  for (std::size_t j = 0; j < vectors.shape.size(); ++j) {
    if (!vectors.shape[j] ||
        vectors.shape[j] > (UINT64_C(1) << 40) / source_count ||
        output.shape[j] > (UINT64_C(1) << 40) / target_count)
      return mismatch("matrix vector count exceeds 2^40");
    source_count *= vectors.shape[j];
    target_count *= output.shape[j];
  }
  return Answer(std::move(output));
}
struct MatrixState final {
  SequenceProfile profile;
  numeric_ops::ExactDot arithmetic;
  bool requested = false;
  std::array<std::uint64_t, 4> vectors{}, matrix{}, replicas{};
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  explicit MatrixState(SequenceProfile selected)
      : profile(selected), arithmetic(selected) {}
  Result<Footprint> support(const DependencyPhase& phase, unsigned port,
                            const std::vector<std::uint64_t>& coordinate,
                            bool validation) const {
    const auto& input = phase.query.inputs[port];
    std::vector<RegionDimension> dimensions;
    if (port == 0) {
      for (auto value : coordinate)
        dimensions.push_back({value, 1});
      dimensions.back() = {0, input.descriptor.shape.back()};
    } else if (port == 1) {
      dimensions = {{coordinate.back(), 1}, {0, input.descriptor.shape[1]}};
    } else {
      dimensions = {{coordinate.back(), 1}};
    }
    if (validation) {
      for (const auto& facet : input.facets)
        if (facet.key == "photospider.image" ||
            facet.key == "photospider.semantic") {
          auto semantic = decode_semantic(facet);
          if (!semantic.ok())
            return Result<Footprint>(semantic.status());
          if (semantic.value().kind == SemanticKind::Image)
            dimensions[2] = {0, input.descriptor.shape[2]};
        }
    }
    return Footprint::from_regions(input.descriptor.shape,
                                   {Region(std::move(dimensions))}, phase.sets);
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics result;
    result.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        result.implementation.data(), result.implementation.size(),
        "photospider.matrix/1;exact-dot;replica-store%s",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= result.implementation.size())
      return Status{ErrorCode::Internal, "matrix identity too long"};
    result.evaluated_values = evaluated;
    result.copied_elements = copied;
    return phase.report_numeric(result);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    const auto& output = phase.query.output.descriptor;
    const auto rank = output.shape.size();
    const auto input_components =
        static_cast<unsigned>(phase.query.inputs[0].descriptor.shape.back());
    if (!requested) {
      const auto count = phase.query.observations.element_count().value();
      request_capacity =
          dependency_internal::metadata_owner(4096 + count * 8192);
      std::vector<AtomCertificate> rows;
      rows.reserve(count);
      auto status = phase.query.observations.visit(
          [&](const auto& coordinate) {
            auto charged = phase.consume_work(6 * rank + 1);
            if (!charged.ok())
              return charged;
            AtomCertificate row{coordinate, {}};
            row.inputs.reserve(6);
            for (unsigned port = 0; port < 3; ++port)
              for (bool validation : {false, true}) {
                auto samples = support(phase, port, coordinate, validation);
                if (!samples.ok())
                  return samples.status();
                row.inputs.push_back(
                    {port, validation ? 4U : 1U, samples.take_value(), {}});
              }
            rows.push_back(std::move(row));
            return Status::success();
          },
          phase.sets.maximum_work, phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      requested = true;
      return Answer(DependencyNeedBatch{std::move(rows)});
    }
    request_capacity.reset();
    auto construction = dependency_internal::metadata_owner(32768);
    numeric_ops::ArrayPublication publication(
        phase.query.outputs.boxes().size(), rank);
    ResourceVector<Value> fragments;
    fragments.reserve(phase.query.outputs.boxes().size());
    const auto width = Value::element_size(output.element_type);
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(output, box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      auto selected = Footprint::from_regions(output.shape, {box}, phase.sets);
      if (!selected.ok())
        return Answer(selected.status());
      std::uint64_t offset = 0;
      auto status = selected.value().visit(
          [&](const auto& coordinate) {
            auto charged =
                phase.consume_work((phase.inputs[0].fragments().size() +
                                    phase.inputs[1].fragments().size() +
                                    phase.inputs[2].fragments().size() + 1) *
                                   (2 * input_components + 1) * rank);
            if (!charged.ok())
              return charged;
            auto at = coordinate;
            for (unsigned j = 0; j < input_components; ++j) {
              at.back() = j;
              vectors[j] = matrix[j] = 0;
              auto read = phase.read(0, at, &vectors[j], width);
              if (!read.ok())
                return read;
              read = phase.read(1, {coordinate.back(), j}, &matrix[j], width);
              if (!read.ok())
                return read;
            }
            std::uint64_t bias = 0;
            auto read = phase.read(2, {coordinate.back()}, &bias, width);
            if (!read.ok())
              return read;
            charged = report(phase, 1, 0);
            if (!charged.ok())
              return charged;
            auto calculated =
                arithmetic.evaluate(vectors, matrix, bias, input_components,
                                    output.element_type, phase.consume_work);
            if (!calculated.ok())
              return calculated.status();
            charged = report(phase, 0, 1);
            if (!charged.ok())
              return charged;
            numeric_ops::select_words(replicas.data(), calculated.value(),
                                      calculated.value(), 1, profile);
            std::memcpy(writer.data() + offset * width, replicas.data(), width);
            ++offset;
            return Status::success();
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
    if (phase.query.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto result =
        publication.finish(output, phase.query.outputs, fragments.data(),
                           fragments.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition matrix_operation(const std::string& key,
                                     SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.requires_metadata_specialization = true;
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {2};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  output.continuation_bytes = sizeof(MatrixState);
  output.maximum_dependency_stages = 2;
  operation.specialize_metadata = [profile](const auto& inputs, const auto&)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto resolved = metadata(inputs);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = resolved.take_value();
    result.regional_atomic = true;
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [profile](const auto&, const auto& allocator) {
    return DependencyContinuation::make<MatrixState>(allocator, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_matrix(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(matrix_operation(
        std::string("numeric.matrix_transform") + profile.first,
        profile.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
