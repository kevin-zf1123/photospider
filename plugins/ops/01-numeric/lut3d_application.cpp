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
#include "01-numeric/exact_lut3d.hpp"
#include "01-numeric/uniform_axis.hpp"
#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
Status type_error(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
struct Lut3dProgram {
  ColorModel model;
  SequenceProfile profile;
  bool tetrahedral = false, clamp = false;
};
Result<OperationPreparation> prepare(
    bool tetrahedral, SequenceProfile profile,
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<OperationPreparation>;
  auto source = color_array_from_parameter(
      std::get<std::string>(parameters.at("input_color_description")));
  auto destination = color_array_from_parameter(
      std::get<std::string>(parameters.at("output_color_description")));
  if (!source.ok())
    return Answer(source.status());
  if (!destination.ok())
    return Answer(destination.status());
  const auto& a = source.value();
  const auto& b = destination.value();
  if (a.model != b.model || a.model == ColorModel::Cmyk ||
      a.association != ColorAssociation::None ||
      b.association != ColorAssociation::None ||
      a.source_layout != ColorSourceLayout::Interleaved ||
      b.source_layout != ColorSourceLayout::Interleaved)
    return Answer(numeric_ops::array_parameter_error(
        "LUT3D requires same-model three-component interleaved colors"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& domain = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (domain != "clamp" && domain != "reject"))
    return Answer(
        numeric_ops::array_parameter_error("invalid LUT3D dtype/domain"));
  if (inputs.size() != 3)
    return Answer(type_error("LUT3D requires input, table, axis"));
  for (unsigned port = 0; port < 3; ++port) {
    const auto& descriptor = inputs[port].descriptor;
    if ((descriptor.element_type != ElementType::Float64 &&
         (port == 2 || descriptor.element_type != ElementType::Float32)) ||
        (port == 0 &&
         (descriptor.shape.size() < 2 || descriptor.shape.size() > 8 ||
          descriptor.shape.back() != 3)) ||
        (port == 1 &&
         (descriptor.shape.size() != 4 || descriptor.shape.back() != 3)) ||
        (port == 2 && descriptor.shape != std::vector<std::uint64_t>{3, 3}))
      return Answer(type_error("invalid LUT3D port shape/dtype"));
    std::uint64_t count = 1;
    for (auto extent : descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(numeric_ops::array_parameter_error(
            "LUT3D logical product exceeds 2^40"));
      count *= extent;
    }
  }
  for (unsigned axis = 0; axis < 3; ++axis)
    if (inputs[1].descriptor.shape[axis] < 2 ||
        inputs[1].descriptor.shape[axis] > 256)
      return Answer(numeric_ops::array_parameter_error(
          "LUT3D axis extent must be 2..256"));
  auto input_facet = encode_color_array(a),
       output_facet = encode_color_array(b);
  if (!input_facet.ok())
    return Answer(input_facet.status());
  if (!output_facet.ok())
    return Answer(output_facet.status());
  for (unsigned port = 0; port < 2; ++port) {
    const auto& expected = port ? output_facet.value() : input_facet.value();
    for (const auto& facet : inputs[port].facets)
      if (facet.key == "photospider.color-array" &&
          (facet.version != expected.version ||
           facet.payload != expected.payload))
        return Answer(type_error(
            "attached LUT3D colors disagree with static description"));
  }
  auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Answer(available);
  OperationPreparation prepared;
  prepared.state = std::make_shared<Lut3dProgram>(
      Lut3dProgram{a.model, profile, tetrahedral, domain == "clamp"});
  prepared.outputs.resize(1);
  auto& output = prepared.outputs[0];
  output.metadata.descriptor = {
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      inputs[0].descriptor.shape};
  output.metadata.facets = {output_facet.take_value()};
  output.metadata.atomic_trailing_axes = 1;
  output.regional_atomic = true;
  return Answer(std::move(prepared));
}
struct Lut3dPoint {
  std::array<std::uint64_t, 7> coordinate{};
  std::array<std::uint64_t, 3> query{};
  std::array<unsigned, 3> cell{};
  numeric_ops::ExactLut3d::Support support;
  std::uint64_t fragment = 0, offset = 0;
};
struct Lut3dState {
  const Lut3dProgram* program;
  std::array<numeric_ops::UniformAxis, 3> axes;
  numeric_ops::ExactLut3d arithmetic;
  unsigned stage = 0, rank = 0;
  ResourceVector<Lut3dPoint> points;
  ResourceVector<MutableValue> outputs;
  std::shared_ptr<const dependency_internal::MetadataOwner> request_capacity;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  explicit Lut3dState(const Lut3dProgram* value)
      : program(value),
        axes{numeric_ops::UniformAxis(value->profile),
             numeric_ops::UniformAxis(value->profile),
             numeric_ops::UniformAxis(value->profile)},
        arithmetic(value->profile) {}
  std::vector<std::uint64_t> coordinate(const Lut3dPoint& point) const {
    return {point.coordinate.begin(), point.coordinate.begin() + rank};
  }
  Status failure(const DependencyPhase& phase, const Lut3dPoint& point,
                 const std::string& message,
                 FailureReason reason = FailureReason::InvalidDomain) const {
    Status result{ErrorCode::OperationFailed,
                  message,
                  reason,
                  {FailureOrigin::Domain, FailureScope::Atom}};
    AtomKey atom;
    atom.output_index = phase.query.output_index;
    atom.rank = rank;
    std::copy_n(point.coordinate.begin(), rank, atom.coordinate.begin());
    result.detail.atom = atom;
    return result;
  }
  Result<std::uint64_t> read(const DependencyPhase& phase, unsigned port,
                             const std::vector<std::uint64_t>& at,
                             const Lut3dPoint& point) const {
    auto work =
        phase.consume_work(phase.inputs[port].fragments().size() + at.size());
    if (!work.ok())
      return Result<std::uint64_t>(work);
    const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                        ElementType::Float32;
    std::uint64_t bits = 0;
    auto status = phase.read(port, at, &bits, narrow ? 4 : 8);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    const auto parts = BinaryParts::decode(bits, narrow);
    if (parts.nan || parts.infinite)
      return Result<std::uint64_t>(failure(
          phase, point, "nonfinite LUT3D input; port=" + std::to_string(port)));
    if (narrow) {
      const auto sign = (bits >> 31) << 63;
      if (!parts.magnitude) {
        bits = sign;
      } else {
        const auto top = 63 - __builtin_clzll(parts.significand);
        bits = sign |
               (static_cast<std::uint64_t>(parts.exponent + top + 1023) << 52) |
               ((parts.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
      }
    }
    return Result<std::uint64_t>(bits);
  }
  Status validate_color(const DependencyPhase& phase, const Lut3dPoint& point,
                        const std::array<std::uint64_t, 3>& color,
                        unsigned port) const {
    if (program->model == ColorModel::Cielch ||
        program->model == ColorModel::Oklch) {
      const auto chroma = BinaryParts::decode(color[1], false);
      if (chroma.negative && chroma.magnitude)
        return failure(phase, point,
                       "negative LUT3D chroma; port=" + std::to_string(port));
    }
    return Status::success();
  }
  Status initialize(const DependencyPhase& phase) {
    rank = phase.query.output.descriptor.shape.size() - 1;
    auto count = phase.query.observations.element_count();
    if (!count.ok())
      return count.status();
    auto work = phase.consume_work(count.value() * (rank + 2) + 1);
    if (!work.ok())
      return work;
    points.reserve(count.value());
    outputs.reserve(phase.query.outputs.boxes().size());
    publication = std::make_unique<numeric_ops::ArrayPublication>(
        phase.query.outputs.boxes().size(), rank + 1);
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              box, phase.allocator);
      if (!allocated.ok())
        return allocated.status();
      Lut3dPoint point;
      point.fragment = outputs.size();
      std::uint64_t elements = 1;
      for (unsigned axis = 0; axis < rank; ++axis) {
        point.coordinate[axis] = box.dimensions()[axis].offset;
        elements *= box.dimensions()[axis].extent;
      }
      for (std::uint64_t i = 0; i < elements; ++i) {
        work = phase.consume_work(1);
        if (!work.ok())
          return work;
        point.offset = i * 3;
        points.push_back(point);
        for (unsigned axis = rank; axis; --axis) {
          const auto dimension = box.dimensions()[axis - 1];
          if (++point.coordinate[axis - 1] <
              dimension.offset + dimension.extent)
            break;
          point.coordinate[axis - 1] = dimension.offset;
        }
      }
      outputs.push_back(allocated.take_value());
    }
    return Status::success();
  }
  std::vector<std::uint64_t> vertex(const Lut3dPoint& point,
                                    unsigned index) const {
    const auto bits = point.support.vertices[index];
    return {point.cell[0] + (bits & 1), point.cell[1] + ((bits >> 1) & 1),
            point.cell[2] + ((bits >> 2) & 1)};
  }
  Result<DependencyPoll> need(const DependencyPhase& phase, unsigned port) {
    using Answer = Result<DependencyPoll>;
    request_capacity =
        dependency_internal::metadata_owner(4096 + points.size() * 32768);
    std::vector<AtomCertificate> certificates;
    certificates.reserve(points.size());
    for (const auto& point : points) {
      auto work = phase.consume_work(32);
      if (!work.ok())
        return Answer(work);
      std::vector<Region> regions;
      if (port == 2) {
        regions.push_back(Region::whole({3, 3}));
      } else if (port == 0) {
        std::vector<RegionDimension> dimensions;
        for (auto value : coordinate(point))
          dimensions.push_back({value, 1});
        dimensions.push_back({0, 3});
        regions.emplace_back(std::move(dimensions));
      } else {
        for (unsigned i = 0; i < point.support.count; ++i) {
          const auto at = vertex(point, i);
          regions.emplace_back(std::vector<RegionDimension>{{at[0], 1},
                                                            {at[1], 1},
                                                            {at[2], 1},
                                                            {0, 3}});
        }
      }
      auto support =
          Footprint::from_regions(phase.query.inputs[port].descriptor.shape,
                                  std::move(regions), phase.sets);
      if (!support.ok())
        return Answer(support.status());
      auto validation = input_internal::validation_closure(
          phase.query.inputs[port], support.value(), phase.sets,
          phase.consume_work);
      if (!validation.ok())
        return Answer(validation.status());
      AtomCertificate certificate{coordinate(point), {}};
      certificate.inputs.push_back(
          {port,
           static_cast<std::uint8_t>(port == 1 ? 1 : 2),
           support.take_value(),
           {}});
      certificate.inputs.push_back({port, 4, validation.take_value(), {}});
      certificates.push_back(std::move(certificate));
    }
    return Answer(DependencyNeedBatch{std::move(certificates)});
  }
  numeric_ops::ExactLut3d::Cell cell(const Lut3dPoint& point) const {
    numeric_ops::ExactLut3d::Cell result;
    for (unsigned i = 0; i < 3; ++i)
      result[i] = {axes[i].knots[point.cell[i]],
                   axes[i].knots[point.cell[i] + 1]};
    return result;
  }
  Status classify(const DependencyPhase& phase, Lut3dPoint* point) {
    auto at = coordinate(*point);
    at.push_back(0);
    // Validate every original component before any domain clamp can hide it.
    for (unsigned i = 0; i < 3; ++i) {
      at.back() = i;
      auto read_value = read(phase, 0, at, *point);
      if (!read_value.ok())
        return read_value.status();
      point->query[i] = read_value.value();
    }
    auto valid = validate_color(phase, *point, point->query, 0);
    if (!valid.ok())
      return valid;
    for (unsigned i = 0; i < 3; ++i) {
      const auto& axis = axes[i];
      const auto key = axis.key(point->query[i]);
      unsigned lo = 0, hi = axis.knots.size();
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
      const auto size = static_cast<unsigned>(axis.knots.size());
      if (lo < size && axis.key(axis.knots[lo]) == key) {
        point->cell[i] = std::min(lo, size - 2);
      } else if (!lo || lo == size) {
        if (!program->clamp)
          return failure(phase, *point,
                         "LUT3D query outside axis=" + std::to_string(i));
        point->cell[i] = lo ? size - 2 : 0;
        point->query[i] = axis.knots[lo ? size - 1 : 0];
      } else {
        point->cell[i] = lo - 1;
      }
    }
    auto support = arithmetic.support(cell(*point), point->query,
                                      program->tetrahedral, phase.consume_work);
    if (!support.ok())
      return support.status();
    point->support = support.take_value();
    return Status::success();
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied) const {
    NumericDiagnostics diagnostic;
    diagnostic.profile = static_cast<CpuNumericProfile>(
        static_cast<unsigned>(program->profile) + 1);
    const auto length = std::snprintf(
        diagnostic.implementation.data(), diagnostic.implementation.size(),
        "photospider.lut3d/1;exact-%s;%s",
        program->tetrahedral ? "tetrahedral" : "trilinear",
        numeric_ops::numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostic.implementation.size())
      return {ErrorCode::Internal, "LUT3D diagnostic identity"};
    diagnostic.evaluated_values = evaluated;
    diagnostic.copied_elements = copied;
    return phase.report_numeric(diagnostic);
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
      for (unsigned axis = 0; axis < 3; ++axis) {
        std::array<std::uint64_t, 3> values{};
        for (unsigned j = 0; j < 3; ++j) {
          auto value = read(phase, 2, {axis, j}, points.front());
          if (!value.ok())
            return Answer(value.status());
          values[j] = value.value();
        }
        auto status = axes[axis].validate(
            values, phase.query.inputs[1].descriptor.shape[axis],
            phase.consume_work);
        if (!status.ok())
          return Answer(status.code == ErrorCode::OperationFailed
                            ? failure(phase, points.front(), status.message,
                                      status.reason)
                            : status);
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
    const unsigned width = narrow ? 4 : 8;
    for (const auto& point : points) {
      std::array<std::array<std::uint64_t, 3>, 8> colors{};
      for (unsigned i = 0; i < point.support.count; ++i) {
        auto at = vertex(point, i);
        at.push_back(0);
        for (unsigned channel = 0; channel < 3; ++channel) {
          at.back() = channel;
          auto value = read(phase, 1, at, point);
          if (!value.ok())
            return Answer(value.status());
          colors[i][channel] = value.value();
        }
        auto valid = validate_color(phase, point, colors[i], 1);
        if (!valid.ok())
          return Answer(valid);
      }
      auto recorded = report(phase, 3, 0);
      if (!recorded.ok())
        return Answer(recorded);
      auto values =
          arithmetic.evaluate(cell(point), point.query, program->tetrahedral,
                              colors, narrow, phase.consume_work);
      if (!values.ok())
        return Answer(values.status());
      for (unsigned channel = 0; channel < 3; ++channel) {
        if (BinaryParts::decode(values.value()[channel], narrow).infinite)
          return Answer(failure(phase, point, "LUT3D output overflow",
                                FailureReason::ArithmeticOverflow));
        std::memcpy(static_cast<std::uint8_t*>(outputs[point.fragment].data()) +
                        (point.offset + channel) * width,
                    &values.value()[channel], width);
      }
      recorded = report(phase, 0, 3);
      if (!recorded.ok())
        return Answer(recorded);
    }
    std::vector<Value> values;
    values.reserve(outputs.size());
    for (auto& output : outputs) {
      auto work = phase.consume_work(1);
      if (!work.ok())
        return Answer(work);
      auto published = std::move(output).publish(phase.query.output.facets,
                                                 phase.query.resources);
      if (!published.ok())
        return Answer(published.status());
      auto retained = publication->retain(published.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      values.push_back(retained.take_value());
    }
    auto result =
        publication->finish(phase.query.output.descriptor, phase.query.outputs,
                            values.data(), values.size(), phase.sets,
                            phase.query.output.facets, phase.query.resources);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition operation(const std::string& key, bool tetrahedral,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.input_schema[0].element_type_mask = 12;
  traits.input_schema[1].element_type_mask = 12;
  traits.input_schema[2].element_type_mask = 4;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"input_color_description", OperationParameterType::String},
      {"output_color_description", OperationParameterType::String},
      {"dtype", OperationParameterType::String},
      {"out_of_domain", OperationParameterType::String}};
  auto& output = traits.outputs[0];
  output.key = "values";
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(Lut3dState);
  output.maximum_dependency_stages = 4;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  result.prepare_static = [tetrahedral, profile](const auto& inputs,
                                                 const auto& parameters) {
    return prepare(tetrahedral, profile, inputs, parameters);
  };
  result.start_dependency = [](const auto& query, const auto& allocator) {
    return DependencyContinuation::make<Lut3dState>(
        allocator, static_cast<const Lut3dProgram*>(query.prepared->state()));
  };
  return result;
}
}  // namespace
Status register_lut3d_application(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (bool tetrahedral : {false, true}) {
      auto status = registry->register_operation(operation(
          std::string("curve.apply_lut3d_") +
              (tetrahedral ? "tetrahedral" : "trilinear") + profile.first,
          tetrahedral, profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
