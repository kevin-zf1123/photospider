#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_parameters.hpp"
#include "01-numeric/comparison_profiles.hpp"
#include "01-numeric/exact_predicate.hpp"
#include "photospider/data/semantic.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::BinaryParts;
using numeric_ops::PredicateInteger;
using numeric_ops::SequenceProfile;
enum class Comparison {
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  IsClose
};
struct ComparisonState final {
  Comparison kind;
  SequenceProfile profile;
  bool ready = false;
  PredicateInteger difference, temporary, threshold, product;
  std::array<std::uint64_t, 4> left{}, right{};
  std::array<std::int64_t, 4> greater{}, less{};
  std::array<bool, 4> unordered{};
  ComparisonState(Comparison operation, SequenceProfile selected)
      : kind(operation), profile(selected) {}
  bool close(std::uint64_t a, std::uint64_t b, bool narrow,
             const std::map<std::string, ParameterValue>& parameters) {
    const auto x = BinaryParts::decode(a, narrow),
               y = BinaryParts::decode(b, narrow);
    if (x.nan || y.nan)
      return false;
    if (x.infinite || y.infinite)
      return x.infinite && y.infinite && x.negative == y.negative;
    std::uint64_t absolute_bits = 0, relative_bits = 0;
    const auto absolute = std::get<double>(parameters.at("atol"));
    const auto relative = std::get<double>(parameters.at("rtol"));
    std::memcpy(&absolute_bits, &absolute, 8);
    std::memcpy(&relative_bits, &relative, 8);
    const auto high = x.magnitude >= y.magnitude ? x : y;
    const auto low = x.magnitude >= y.magnitude ? y : x;
    difference.set(high);
    temporary.set(low);
    if (x.negative != y.negative)
      difference.add(temporary);
    else
      difference.subtract(temporary);
    threshold.set(BinaryParts::decode(absolute_bits, false));
    product.set_product(BinaryParts::decode(relative_bits, false), high);
    threshold.add(product);
    for (std::size_t end = difference.words.size(); end; end -= 4) {
      numeric_ops::compare_keys(difference.words.data() + end - 4,
                                threshold.words.data() + end - 4,
                                greater.data(), less.data(), profile);
      for (unsigned i = 4; i; --i) {
        if (greater[i - 1])
          return false;
        if (less[i - 1])
          return true;
      }
    }
    return true;
  }
  Status report(const DependencyPhase& phase, std::uint64_t count) {
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto* identity = numeric_ops::comparison_implementation(profile);
    std::memcpy(diagnostics.implementation.data(), identity,
                std::strlen(identity) + 1);
    diagnostics.evaluated_values = count;
    return phase.report_numeric(diagnostics);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    if (!ready) {
      ready = true;
      return Answer(DependencyNeedBatch{{}, {}, true});
    }
    const auto type = phase.query.inputs[0].descriptor.element_type;
    const auto width = Value::element_size(type);
    const bool floating =
        type == ElementType::Float32 || type == ElementType::Float64;
    ResourceVector<Value> fragments;
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto output = allocated.take_value();
      auto subset = Footprint::from_regions(phase.query.output.descriptor.shape,
                                            {box}, phase.sets);
      if (!subset.ok())
        return Answer(subset.status());
      std::size_t buffered = 0;
      std::uint64_t offset = 0;
      const auto flush = [&]() -> Status {
        auto status = phase.consume_work(
            buffered * (kind == Comparison::IsClose ? 1024 : 16));
        if (!status.ok())
          return status;
        if (kind == Comparison::IsClose) {
          for (std::size_t i = 0; i < buffered; ++i)
            output.data()[offset + i] =
                close(left[i], right[i], type == ElementType::Float32,
                      phase.query.parameters);
        } else {
          numeric_ops::compare_keys(left.data(), right.data(), greater.data(),
                                    less.data(), profile);
          for (std::size_t i = 0; i < buffered; ++i) {
            bool result = false;
            switch (kind) {
              case Comparison::Equal:
                result = !greater[i] && !less[i];
                break;
              case Comparison::NotEqual:
                result = greater[i] || less[i];
                break;
              case Comparison::Less:
                result = less[i];
                break;
              case Comparison::LessEqual:
                result = !greater[i];
                break;
              case Comparison::Greater:
                result = greater[i];
                break;
              case Comparison::GreaterEqual:
                result = !less[i];
                break;
              case Comparison::IsClose:
                break;
            }
            output.data()[offset + i] =
                unordered[i] ? kind == Comparison::NotEqual : result;
          }
        }
        status = report(phase, buffered);
        offset += buffered;
        buffered = 0;
        return status;
      };
      auto status = subset.value().visit(
          [&](const auto& coordinate) {
            std::uint64_t a = 0, b = 0;
            auto read = phase.read(0, coordinate, &a, width);
            if (!read.ok())
              return read;
            read = phase.read(1, coordinate, &b, width);
            if (!read.ok())
              return read;
            unordered[buffered] = false;
            if (kind != Comparison::IsClose) {
              if (floating) {
                const auto x =
                    BinaryParts::decode(a, type == ElementType::Float32);
                const auto y =
                    BinaryParts::decode(b, type == ElementType::Float32);
                unordered[buffered] = x.nan || y.nan;
                a = x.order_key();
                b = y.order_key();
              } else if (type == ElementType::Int64) {
                a ^= UINT64_C(1) << 63;
                b ^= UINT64_C(1) << 63;
              }
            }
            left[buffered] = a;
            right[buffered] = b;
            ++buffered;
            return buffered == 4 ? flush() : Status::success();
          },
          subset.value().element_count().value(), phase.query.cancellation);
      if (!status.ok())
        return Answer(status);
      if (buffered) {
        status = flush();
        if (!status.ok())
          return Answer(status);
      }
      auto value = std::move(output).publish();
      if (!value.ok())
        return Answer(value.status());
      fragments.push_back(value.take_value());
    }
    auto result = ValueFragments::create_view(
        phase.query.output.descriptor, {}, phase.query.outputs,
        fragments.data(), fragments.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
};
OperationDefinition comparison(const std::string& key, Comparison kind,
                               SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  if (kind == Comparison::IsClose) {
    for (auto& input : traits.input_schema)
      input.element_type_mask = 12;
    traits.parameter_schema = {{"atol", OperationParameterType::Float64},
                               {"rtol", OperationParameterType::Float64}};
  }
  auto& output = traits.outputs[0];
  output.key = "values";
  output.output_element_type = ElementType::UInt8;
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ComparisonState);
  output.maximum_dependency_stages = 2;
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    const auto& first = inputs[0].descriptor;
    if (first.shape != inputs[1].descriptor.shape ||
        first.element_type != inputs[1].descriptor.element_type)
      return Answer(Status{ErrorCode::TypeMismatch,
                           "comparison inputs must match shape and dtype",
                           FailureReason::None,
                           {FailureOrigin::Schema, FailureScope::Unspecified}});
    std::uint64_t count = 1;
    for (auto extent : first.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "comparison input exceeds 2^40 elements",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
      count *= extent;
    }
    if (kind == Comparison::IsClose) {
      for (const auto* name : {"atol", "rtol"}) {
        const auto value = std::get<double>(parameters.at(name));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        const auto parts = BinaryParts::decode(bits, false);
        if (parts.nan || parts.infinite || (parts.negative && parts.magnitude))
          return Answer(numeric_ops::array_parameter_error(
              "is_close tolerances must be finite and nonnegative"));
      }
    }
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    result.metadata.descriptor = {ElementType::UInt8, first.shape};
    std::vector<DependencyMappedNeed> maps;
    for (std::uint32_t port = 0; port < 2; ++port) {
      DependencyMappedNeed data;
      data.port = port;
      data.roles = 1;
      for (std::size_t axis = 0; axis < first.shape.size(); ++axis)
        data.axes.push_back({static_cast<std::int32_t>(axis), {}});
      auto validation = data;
      validation.roles = 4;
      for (const auto& facet : inputs[port].facets)
        if (facet.key == "photospider.image" ||
            facet.key == "photospider.semantic") {
          auto semantic = decode_semantic(facet);
          if (!semantic.ok())
            return Answer(semantic.status());
          if (semantic.value().kind == SemanticKind::Image)
            validation.axes[2] = {-1, {0, first.shape[2]}};
        }
      maps.push_back(std::move(data));
      maps.push_back(std::move(validation));
    }
    result.static_dependency_maps = std::move(maps);
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.start_dependency = [kind, profile](const auto&,
                                               const auto& allocator) {
    return DependencyContinuation::make<ComparisonState>(allocator, kind,
                                                         profile);
  };
  return operation;
}
struct SelectState final {
  SequenceProfile profile;
  std::uint8_t stage = 0, condition = 0;
  std::array<std::uint64_t, 4> selected{};
  explicit SelectState(SequenceProfile selected_profile)
      : profile(selected_profile) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    const auto coordinate = multi_output::coordinate(phase);
    if (stage == 0) {
      ++stage;
      return multi_output::need(phase, {{0, 6, phase.query.outputs, {}}});
    }
    if (stage == 1) {
      auto read = phase.read(0, coordinate, &condition, 1);
      if (!read.ok())
        return Answer(read);
      if (condition > 1) {
        const auto key = dependency_atom_key(phase.query);
        Status status{
            ErrorCode::InvalidArgument,
            "InvalidCondition: port=0 byte=" + std::to_string(condition),
            FailureReason::InvalidDomain,
            {FailureOrigin::Domain, FailureScope::Atom}};
        if (key.ok())
          status.detail.atom = key.value();
        return Answer(status);
      }
      ++stage;
      const std::uint32_t port = condition ? 1 : 2;
      auto validation = phase.query.outputs;
      for (const auto& facet : phase.query.inputs[port].facets)
        if (facet.key == "photospider.image" ||
            facet.key == "photospider.semantic") {
          auto semantic = decode_semantic(facet);
          if (!semantic.ok())
            return Answer(semantic.status());
          if (semantic.value().kind == SemanticKind::Image) {
            auto dimensions = phase.query.outputs.boxes()[0].dimensions();
            dimensions[2] = {0, phase.query.inputs[port].descriptor.shape[2]};
            auto closure = Footprint::from_regions(
                phase.query.inputs[port].descriptor.shape, {Region(dimensions)},
                phase.sets);
            if (!closure.ok())
              return Answer(closure.status());
            validation = closure.take_value();
          }
        }
      return multi_output::need(phase, {{port, 1, phase.query.outputs, {}},
                                        {port, 4, std::move(validation), {}}});
    }
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    auto status = phase.consume_work(32 + width);
    if (!status.ok())
      return Answer(status);
    std::uint64_t bits = 0;
    status = phase.read(condition ? 1 : 2, coordinate, &bits, width);
    if (!status.ok())
      return Answer(status);
    numeric_ops::select_words(selected.data(), condition ? bits : 0,
                              condition ? 0 : bits, condition, profile);
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto* identity = numeric_ops::selection_implementation(profile);
    std::memcpy(diagnostics.implementation.data(), identity,
                std::strlen(identity) + 1);
    diagnostics.evaluated_values = 1;
    status = phase.report_numeric(diagnostics);
    if (!status.ok())
      return Answer(status);
    return multi_output::finish(phase, selected.data(), width);
  }
};
OperationDefinition selection(const std::string& key, SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::UInt8);
  auto& output = traits.outputs[0];
  output.key = "values";
  output.shape_rule = OperationShapeRule::MatchAllInputs;
  output.output_dtype_rule = OperationDtypeRule::Input;
  output.output_dtype_input = 1;
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(SelectState);
  output.maximum_dependency_stages = 3;
  output.failure_delivery = FailureDelivery::PerAtomOutcome;
  operation.validate_dependency = [profile](const auto& inputs, const auto&) {
    if (inputs[1].descriptor.element_type != inputs[2].descriptor.element_type)
      return Status{ErrorCode::TypeMismatch,
                    "select branch dtypes must match",
                    FailureReason::None,
                    {FailureOrigin::Schema, FailureScope::Unspecified}};
    std::uint64_t count = 1;
    for (auto extent : inputs[0].descriptor.shape) {
      if (!extent || extent > (UINT64_C(1) << 40) / count)
        return Status{ErrorCode::TypeMismatch,
                      "select input exceeds 2^40 elements",
                      FailureReason::None,
                      {FailureOrigin::Schema, FailureScope::Unspecified}};
      count *= extent;
    }
    return numeric_ops::sequence_profile_available(profile);
  };
  operation.start_dependency = [profile](const auto&, const auto& allocator) {
    return DependencyContinuation::make<SelectState>(allocator, profile);
  };
  return operation;
}
}  // namespace
Status register_numeric_comparisons(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& item :
         {std::make_pair("equal", Comparison::Equal),
          std::make_pair("not_equal", Comparison::NotEqual),
          std::make_pair("less", Comparison::Less),
          std::make_pair("less_equal", Comparison::LessEqual),
          std::make_pair("greater", Comparison::Greater),
          std::make_pair("greater_equal", Comparison::GreaterEqual),
          std::make_pair("is_close", Comparison::IsClose)}) {
      auto status = registry->register_operation(
          comparison(std::string("numeric.") + item.first + variant.first,
                     item.second, variant.second));
      if (!status.ok())
        return status;
    }
    auto status = registry->register_operation(selection(
        std::string("numeric.select") + variant.first, variant.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
