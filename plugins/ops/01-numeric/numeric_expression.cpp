#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/multi_output.hpp"
#include "01-numeric/array_publication.hpp"
#include "01-numeric/exact_sampling.hpp"
#include "01-numeric/expression_evaluator.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "photospider/numeric/expression.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal::numeric_ops {
namespace {
Status schema(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::InvalidDomain,
                {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::string canonical_names(const ExpressionProgram& program) {
  std::string result;
  for (const auto& name : program.names) {
    if (!result.empty())
      result += ' ';
    result += name;
  }
  return result;
}
struct SampleProgram final {
  ExpressionProgram expression;
  std::uint32_t count;
  ElementType dtype;
  bool regional;
};
struct EmptyEvaluator {
  explicit EmptyEvaluator(SequenceProfile) {}
};
template <bool Values>
struct SampleState final {
  const SampleProgram* program;
  SequenceProfile profile;
  std::conditional_t<Values, ExpressionEvaluator, EmptyEvaluator> evaluator;
  ExactSampling sampling;
  std::array<std::uint64_t, 4> replicas{};
  std::array<std::uint64_t, Values ? 256 : 0> coefficients{};
  std::array<std::uint64_t, 2> endpoints{};
  std::uint32_t pending_begin = 0, pending_end = 0;
  explicit SampleState(const SampleProgram* prepared, SequenceProfile selected)
      : program(prepared),
        profile(selected),
        evaluator(selected),
        sampling(selected) {}
  bool required(std::uint32_t port) const {
    return port != 1 || program->count > 1;
  }
  Status report(const DependencyPhase& phase, std::uint64_t evaluated,
                std::uint64_t copied,
                NumericMathFunction function = NumericMathFunction::Other,
                bool fallback = false) const {
    NumericDiagnostics diagnostics;
    diagnostics.profile =
        static_cast<CpuNumericProfile>(static_cast<unsigned>(profile) + 1);
    const auto length = std::snprintf(
        diagnostics.implementation.data(), diagnostics.implementation.size(),
        "photospider.expression/1;RN64-postorder;Q128..4096;integer-ISA%s",
        numeric_build_identity());
    if (length < 0 ||
        static_cast<std::size_t>(length) >= diagnostics.implementation.size())
      return Status{ErrorCode::Internal, "expression identity too long"};
    diagnostics.evaluated_values = evaluated;
    diagnostics.copied_elements = copied;
    if (function != NumericMathFunction::Other) {
      if (fallback) {
        diagnostics.strict_fallbacks = 1;
        const auto reason =
            static_cast<unsigned>(NumericFallbackReason::FunctionUnsupported);
        diagnostics.fallback_reasons[reason] = 1;
        diagnostics
            .function_fallbacks[static_cast<unsigned>(function)][reason] = 1;
      } else {
        diagnostics.strict_math_calls = 1;
      }
    }
    return phase.report_numeric(diagnostics);
  }
  Status fail(const DependencyPhase& phase, std::uint64_t index,
              FailureReason reason, const std::string& message) const {
    auto status = expression_failure(index, false, 0, reason, message);
    auto atom = dependency_atom_key(phase.query);
    if (atom.ok())
      status.detail.atom = atom.take_value();
    return status;
  }
  static std::uint64_t widened(std::uint64_t bits, bool narrow) {
    if (!narrow)
      return bits;
    const auto value = BinaryParts::decode(bits, true);
    const auto sign = (bits >> 31) << 63;
    if (!value.magnitude)
      return sign;
    const auto top = 63 - __builtin_clzll(value.significand);
    return sign |
           (static_cast<std::uint64_t>(value.exponent + top + 1023) << 52) |
           ((value.significand << (52 - top)) & UINT64_C(0xfffffffffffff));
  }
  Result<std::uint64_t> rounded(std::uint64_t a, std::uint64_t b,
                                std::uint32_t wa, std::uint32_t wb,
                                std::uint32_t divisor, bool narrow,
                                bool subtract, const DependencyPhase& phase) {
    return sampling.weighted(a, b, wa, wb, divisor, narrow, subtract,
                             phase.consume_work);
  }
  Result<std::uint64_t> coordinate(std::uint64_t index,
                                   const DependencyPhase& phase) {
    return sampling.coordinate(static_cast<std::uint32_t>(index),
                               program->count, endpoints, phase.consume_work);
  }
  static bool equal(std::uint64_t a, std::uint64_t b) {
    return a == b || ((a | b) & UINT64_C(0x7fffffffffffffff)) == 0;
  }
  Result<DependencyPoll> publish(const DependencyPhase& phase,
                                 const std::uint64_t* bits, std::size_t count) {
    using Answer = Result<DependencyPoll>;
    auto work = phase.consume_work(count);
    if (!work.ok())
      return Answer(work);
    ArrayPublication publication(1, 1);
    auto allocated =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto writer = allocated.take_value();
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    for (std::size_t i = 0; i < count; ++i) {
      select_words(replicas.data(), bits[i], bits[i], 1, profile);
      std::memcpy(writer.data() + i * width, replicas.data(), width);
    }
    auto value = std::move(writer).publish();
    if (!value.ok())
      return Answer(value.status());
    auto retained = publication.retain(value.take_value());
    if (!retained.ok())
      return Answer(retained.status());
    auto owned = retained.take_value();
    auto result =
        publication.finish(phase.query.output.descriptor, phase.query.outputs,
                           &owned, 1, phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
  Result<std::uint64_t> evaluate_sample(std::uint64_t index,
                                        const DependencyPhase& phase) {
    auto current = coordinate(index, phase);
    if (!current.ok())
      return Result<std::uint64_t>(current.status());
    for (int direction : {-1, 1}) {
      if ((direction < 0 && !index) ||
          (direction > 0 && index + 1 == program->count))
        continue;
      auto adjacent = coordinate(direction < 0 ? index - 1 : index + 1, phase);
      if (!adjacent.ok())
        return Result<std::uint64_t>(adjacent.status());
      if (equal(current.value(), adjacent.value()))
        return Result<std::uint64_t>(expression_failure(
            index, true, current.value(), FailureReason::ArithmeticOverflow,
            "duplicate adjacent sampling coordinate"));
    }
    auto reported = report(phase, 1, 0);
    if (!reported.ok())
      return Result<std::uint64_t>(reported);
    auto value = evaluator.evaluate(
        program->expression, current.value(), coefficients, index,
        phase.consume_work, [&](NumericMathFunction function, bool fallback) {
          return report(phase, 0, 0, function, fallback);
        });
    if (!value.ok())
      return Result<std::uint64_t>(value.status());
    if (program->dtype == ElementType::Float32) {
      value = rounded(value.value(), 0, 1, 0, 1, true, false, phase);
      if (!value.ok())
        return Result<std::uint64_t>(value.status());
      if (BinaryParts::decode(value.value(), true).infinite) {
        const auto& root =
            program->expression.nodes[program->expression.size - 1];
        return Result<std::uint64_t>(expression_failure(
            index, true, current.value(), FailureReason::ArithmeticOverflow,
            "span=[" + std::to_string(root.begin) + "," +
                std::to_string(root.end) + ") final Float32 overflow"));
      }
    }
    return value;
  }
  Result<DependencyPoll> publish_values(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    ArrayPublication publication(phase.query.outputs.boxes().size(), 1);
    ResourceVector<Value> outputs;
    outputs.reserve(phase.query.outputs.boxes().size());
    const auto width = Value::element_size(program->dtype);
    for (const auto& box : phase.query.outputs.boxes()) {
      auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                              box, phase.allocator);
      if (!allocated.ok())
        return Answer(allocated.status());
      auto writer = allocated.take_value();
      const auto range = box.dimensions()[0];
      for (std::uint64_t local = 0; local < range.extent; ++local) {
        auto value = evaluate_sample(range.offset + local, phase);
        if (!value.ok())
          return Answer(value.status());
        auto reported = report(phase, 0, 1);
        if (!reported.ok())
          return Answer(reported);
        select_words(replicas.data(), value.value(), value.value(), 1, profile);
        std::memcpy(writer.data() + local * width, replicas.data(), width);
      }
      auto value = std::move(writer).publish();
      if (!value.ok())
        return Answer(value.status());
      auto retained = publication.retain(value.take_value());
      if (!retained.ok())
        return Answer(retained.status());
      outputs.push_back(retained.take_value());
    }
    auto result =
        publication.finish(phase.query.output.descriptor, phase.query.outputs,
                           outputs.data(), outputs.size(), phase.sets);
    return result.ok() ? Answer(result.take_value()) : Answer(result.status());
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    using Answer = Result<DependencyPoll>;
    const auto index = multi_output::coordinate(phase)[0];
    if constexpr (Values) {
      if (program->regional && pending_end == 0) {
        pending_end =
            static_cast<std::uint32_t>(program->expression.names.size() + 2);
        return Answer(DependencyNeedBatch{{}, {}, true});
      }
    }
    // Consume only the previous bounded transport stage before requesting more.
    for (auto port = pending_begin; port < pending_end; ++port) {
      if (!required(port))
        continue;
      auto charged = phase.consume_work(128);
      if (!charged.ok())
        return Answer(charged);
      const bool narrow = phase.query.inputs[port].descriptor.element_type ==
                          ElementType::Float32;
      std::uint64_t bits = 0;
      auto read = phase.read(port, {0}, &bits, narrow ? 4 : 8);
      if (!read.ok())
        return Answer(read);
      const auto value = BinaryParts::decode(bits, narrow);
      if (value.nan || value.infinite) {
        const auto name = port == 0   ? "start"
                          : port == 1 ? "end"
                                      : program->expression.names[port - 2];
        return Answer(fail(phase, index, FailureReason::InvalidDomain,
                           "nonfinite input " + name +
                               " port=" + std::to_string(port) +
                               " bits=" + std::to_string(bits)));
      }
      bits = widened(bits, narrow);
      if (port < 2)
        endpoints[port] = bits;
      else if constexpr (Values)
        coefficients[port - 2] = bits;
    }
    const auto ports =
        Values
            ? static_cast<std::uint32_t>(program->expression.names.size() + 2)
            : 2U;
    pending_begin = pending_end;
    if (pending_end < ports) {
      auto construction = dependency_internal::metadata_owner(8192);
      std::vector<DependencyNeed> needs;
      const auto limit = std::min<std::uint64_t>(16, phase.sets.maximum_boxes);
      if (!limit)
        return Answer(Status{ErrorCode::ResourceExhausted,
                             "expression scalar transport capacity",
                             FailureReason::CapacityLimit});
      while (pending_end < ports && needs.size() < limit) {
        const auto port = pending_end++;
        if (!required(port))
          continue;
        const bool data = !Values || port >= 2 ||
                          (program->expression.uses_x &&
                           (program->count == 1           ? port == 0
                            : index == 0                  ? port == 0
                            : index + 1 == program->count ? port == 1
                                                          : true));
        auto samples = Footprint::all({1}, phase.sets);
        if (!samples.ok())
          return Answer(samples.status());
        needs.push_back({port,
                         static_cast<std::uint32_t>(data ? 5 : 4),
                         samples.take_value(),
                         {}});
      }
      if (!needs.empty())
        return multi_output::need(phase, std::move(needs));
    }
    std::uint64_t step = 0;
    if (program->count > 1) {
      if (equal(endpoints[0], endpoints[1]))
        return Answer(fail(phase, index, FailureReason::InvalidDomain,
                           "equal sampling endpoints"));
      auto calculated = rounded(endpoints[1], endpoints[0], 1, 1,
                                program->count - 1, false, true, phase);
      if (!calculated.ok())
        return Answer(calculated.status());
      step = calculated.value();
      const auto parts = BinaryParts::decode(step, false);
      if (parts.infinite || !parts.magnitude)
        return Answer(fail(phase, index, FailureReason::ArithmeticOverflow,
                           "unrepresentable sampling step"));
      const bool descending =
          BinaryParts::decode(endpoints[1], false).order_key() <
          BinaryParts::decode(endpoints[0], false).order_key();
      if (parts.negative != descending)
        return Answer(fail(phase, index, FailureReason::InvalidDomain,
                           "sampling step direction"));
    }
    if constexpr (!Values) {
      const std::array<std::uint64_t, 3> axis{
          endpoints[0], program->count == 1 ? endpoints[0] : endpoints[1],
          step};
      auto reported = report(phase, 0, 3);
      if (!reported.ok())
        return Answer(reported);
      return publish(phase, axis.data(), 3);
    } else {
      return publish_values(phase);
    }
  }
};
OperationDefinition expression_operation(const std::string& key,
                                         SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.repeated_minimum = 1;
  traits.repeated_maximum = 257;
  traits.repeated_match = false;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.rank = 1;
    port.element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"expression", OperationParameterType::String},
      {"coefficient_names", OperationParameterType::String},
      {"count", OperationParameterType::Int64, true, true, 1, 1048576},
      {"dtype", OperationParameterType::String}};
  auto& values = traits.outputs[0];
  values.key = "values";
  values.region_rule = OperationRegionRule::Dependency;
  values.dependency_version = 1;
  values.continuation_bytes = sizeof(SampleState<true>);
  values.maximum_dependency_stages = 259;
  values.failure_delivery = FailureDelivery::PerAtomOutcome;
  traits.outputs.push_back(values);
  traits.outputs[1].key = "axis";
  traits.outputs[1].continuation_bytes = sizeof(SampleState<false>);
  operation.prepare_static =
      [profile](const auto& inputs,
                const auto& parameters) -> Result<OperationPreparation> {
    using Answer = Result<OperationPreparation>;
    for (const auto& input : inputs)
      if (input.descriptor.shape != std::vector<std::uint64_t>{1})
        return Answer(
            Status{ErrorCode::TypeMismatch,
                   "expression inputs require [1]",
                   FailureReason::None,
                   {FailureOrigin::Schema, FailureScope::Unspecified}});
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype != "float32" && dtype != "float64")
      return Answer(schema("expression dtype must be float32/float64"));
    auto parsed =
        ExpressionParser(std::get<std::string>(parameters.at("expression")))
            .run();
    if (!parsed.ok())
      return Answer(parsed.status());
    if (canonical_names(parsed.value()) !=
            std::get<std::string>(parameters.at("coefficient_names")) ||
        inputs.size() != parsed.value().names.size() + 2)
      return Answer(
          schema("coefficient_names must exactly match sorted free identifiers "
                 "and inputs"));
    auto available = sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto program = std::make_shared<const SampleProgram>(SampleProgram{
        parsed.take_value(),
        static_cast<std::uint32_t>(
            std::get<std::int64_t>(parameters.at("count"))),
        dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
        inputs.size() <= 16});
    OperationPreparation prepared;
    prepared.outputs.resize(2);
    prepared.state = program;
    prepared.outputs[0].metadata.descriptor = {program->dtype,
                                               {program->count}};
    prepared.outputs[1].metadata.descriptor = {ElementType::Float64, {3}};
    prepared.outputs[1].metadata.atomic_trailing_axes = 1;
    if (program->regional) {
      std::vector<DependencyMapPiece> pieces;
      const auto append = [&](std::uint64_t offset,
                              std::uint64_t extent) -> Status {
        if (!extent)
          return Status::success();
        auto coverage = Footprint::from_regions({program->count},
                                                {Region({{offset, extent}})});
        if (!coverage.ok())
          return coverage.status();
        std::vector<DependencyMappedNeed> maps;
        for (std::uint32_t port = 0; port < inputs.size(); ++port) {
          if (port == 1 && program->count == 1)
            continue;
          const bool data =
              port >= 2 || (program->expression.uses_x &&
                            (program->count == 1            ? port == 0
                             : offset == 0                  ? port == 0
                             : offset + 1 == program->count ? port == 1
                                                            : true));
          DependencyMappedNeed map;
          map.port = port;
          map.roles = data ? 5 : 4;
          map.axes = {{-1, {0, 1}}};
          maps.push_back(std::move(map));
        }
        pieces.push_back({coverage.take_value(), std::move(maps)});
        return Status::success();
      };
      auto status = append(0, 1);
      if (!status.ok())
        return Answer(status);
      if (program->count > 2) {
        status = append(1, program->count - 2);
        if (!status.ok())
          return Answer(status);
      }
      if (program->count > 1) {
        status = append(program->count - 1, 1);
        if (!status.ok())
          return Answer(status);
      }
      prepared.outputs[0].static_dependency_pieces = std::move(pieces);
    }

    return Answer(std::move(prepared));
  };
  operation.start_dependency = [profile](const auto& query,
                                         const auto& allocator) {
    const auto* program =
        static_cast<const SampleProgram*>(query.prepared->state());
    return query.output_index == 0
               ? DependencyContinuation::make<SampleState<true>>(
                     allocator, program, profile)
               : DependencyContinuation::make<SampleState<false>>(
                     allocator, program, profile);
  };
  return operation;
}
}  // namespace
}  // namespace ps::plugin_internal::numeric_ops

namespace ps::plugin_internal {
Status register_numeric_expression(OperationRegistry* registry) {
  using namespace numeric_ops;  // NOLINT(build/namespaces)
  for (const auto& entry :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(expression_operation(
        std::string("numeric.sample_expression") + entry.first, entry.second));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
namespace ps::numeric {
Result<WorkflowNode> sample_expression_node(
    std::uint64_t id, std::string expression, WorkflowInput start,
    WorkflowInput end, std::int64_t count,
    const std::map<std::string, WorkflowInput>& coefficients, ElementType dtype,
    CpuNumericProfile profile) {
  using namespace plugin_internal::numeric_ops;  // NOLINT(build/namespaces)
  if (!id || count < 1 || count > 1048576 ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      profile < CpuNumericProfile::Strict ||
      profile > CpuNumericProfile::X86Avx2)
    return Result<WorkflowNode>(
        schema("invalid expression id/count/dtype/profile"));
  auto program = ExpressionParser(expression).run();
  if (!program.ok())
    return Result<WorkflowNode>(program.status());
  if (coefficients.size() != program.value().names.size())
    return Result<WorkflowNode>(
        schema("expression coefficient names mismatch"));
  std::vector<WorkflowInput> inputs{std::move(start), std::move(end)};
  for (const auto& name : program.value().names) {
    auto found = coefficients.find(name);
    if (found == coefficients.end())
      return Result<WorkflowNode>(schema("expression coefficient missing"));
    inputs.push_back(found->second);
  }
  const auto* suffix = profile == CpuNumericProfile::Strict ? "_strict"
                       : profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                           : "_accelerated_x86_64";
  return Result<WorkflowNode>(WorkflowNode{
      id,
      std::string("numeric.sample_expression") + suffix,
      std::move(inputs),
      {{"expression", std::move(expression)},
       {"count", count},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"coefficient_names", canonical_names(program.value())}}});
}
}  // namespace ps::numeric
