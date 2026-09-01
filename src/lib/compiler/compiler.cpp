#include "photospider/compiler/compiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Deterministic non-cryptographic FNV-1a stage digest builder.
 *
 * @note The digest is explicitly non-security identity and is never used for
 * signatures, authentication, or native-code admission.
 */
class DigestBuilder final {
 public:
  /** @brief Creates the canonical FNV-1a offset basis. */
  DigestBuilder() noexcept = default;

  /**
   * @brief Appends raw canonical bytes.
   * @param data Byte pointer, possibly null only when size is zero.
   * @param size Byte count.
   * @throws Nothing.
   * @note Input order is part of identity.
   */
  void bytes(const void* data, std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
      value_ ^= cursor[index];
      value_ *= 1099511628211ULL;
    }
  }

  /**
   * @brief Appends a uint64 in fixed little-endian order.
   * @param value Canonical integer.
   * @throws Nothing.
   * @note Host endianness does not affect the result.
   */
  void integer(std::uint64_t value) noexcept {
    std::uint8_t encoded[8]{};
    for (std::size_t index = 0; index < 8U; ++index) {
      encoded[index] =
          static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
    bytes(encoded, sizeof(encoded));
  }

  /**
   * @brief Appends a length-framed string.
   * @param value Exact bytes.
   * @throws Nothing.
   * @note UTF-8 validation belongs to the stage validator.
   */
  void text(const std::string& value) noexcept {
    integer(value.size());
    bytes(value.data(), value.size());
  }

  /**
   * @brief Returns fixed lowercase hexadecimal text.
   * @return Sixteen-character digest.
   * @throws std::bad_alloc If stream/string allocation fails.
   * @note This representation is stable for compiler version one.
   */
  [[nodiscard]] std::string finish() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value_;
    return stream.str();
  }

 private:
  /** @brief Current FNV-1a state. */
  std::uint64_t value_ = 14695981039346656037ULL;
};

/**
 * @brief Validates bounded diagnostic/source text.
 * @param value Candidate source string.
 * @param maximum Inclusive byte limit.
 * @return True for nonempty text without ASCII control bytes.
 * @throws Nothing.
 * @note Full Unicode normalization is outside source schema version one.
 */
bool valid_text(const std::string& value, std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(), [](unsigned char byte) {
           return byte < 0x20U || byte == 0x7fU;
         });
}

/**
 * @brief Appends one parameter value to a canonical digest.
 * @param digest Destination builder.
 * @param value Closed source parameter variant.
 * @throws Nothing.
 * @note Floating values use their exact IEEE binary representation.
 */
void append_parameter(DigestBuilder* digest,
                      const ParameterValue& value) noexcept {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    digest->integer(1U);
    digest->integer(static_cast<std::uint64_t>(*integer));
  } else if (const auto* floating = std::get_if<double>(&value)) {
    digest->integer(2U);
    std::uint64_t bits = 0U;
    std::memcpy(&bits, floating, sizeof(bits));
    if ((bits << 1U) == 0U) {
      bits = 0U;
    }
    digest->integer(bits);
  } else if (const auto* boolean = std::get_if<bool>(&value)) {
    digest->integer(3U);
    digest->integer(*boolean ? 1U : 0U);
  } else {
    digest->integer(4U);
    digest->text(std::get<std::string>(value));
  }
}

/**
 * @brief Appends compiler-visible operation traits.
 * @param digest Destination builder.
 * @param traits Copied semantic traits.
 * @throws Nothing.
 * @note Callback/library identity is intentionally excluded.
 */
void append_traits(DigestBuilder* digest,
                   const OperationTraits& traits) noexcept {
  digest->integer(traits.input_count);
  digest->integer(traits.deterministic ? 1U : 0U);
  digest->integer(traits.side_effect_free ? 1U : 0U);
  digest->integer(traits.supports_cpu ? 1U : 0U);
  digest->integer(traits.supports_gpu ? 1U : 0U);
  digest->integer(traits.allows_cpu_fallback ? 1U : 0U);
  digest->integer(traits.estimated_bytes);
  digest->integer(traits.version);
  digest->integer(traits.cacheable ? 1U : 0U);
  digest->integer(static_cast<std::uint32_t>(traits.output_element_type));
  digest->integer(static_cast<std::uint32_t>(traits.shape_rule));
  digest->integer(static_cast<std::uint32_t>(traits.region_rule));
  digest->integer(traits.halo_radius);
}

/**
 * @brief Appends one statically inferred Value descriptor to stage identity.
 * @param digest Destination builder.
 * @param descriptor Valid output descriptor.
 * @throws Nothing.
 */
void append_descriptor(DigestBuilder* digest,
                       const ValueDescriptor& descriptor) noexcept {
  digest->integer(static_cast<std::uint32_t>(descriptor.element_type));
  digest->integer(descriptor.shape.size());
  for (std::uint64_t extent : descriptor.shape) {
    digest->integer(extent);
  }
}

/**
 * @brief Infers and validates one operation's static output descriptor.
 * @param traits Complete version-one semantic traits.
 * @param inputs Dependency output descriptors in invocation order.
 * @return Statically known output descriptor or a typed trait/type failure.
 * @throws std::bad_alloc If diagnostic or descriptor allocation fails.
 */
Result<ValueDescriptor> infer_output_descriptor(
    const OperationTraits& traits, const std::vector<ValueDescriptor>& inputs) {
  if (traits.version != 1U || inputs.size() != traits.input_count ||
      (traits.cacheable &&
       (!traits.deterministic || !traits.side_effect_free)) ||
      (traits.region_rule == OperationRegionRule::Halo &&
       traits.halo_radius == 0U) ||
      (traits.region_rule != OperationRegionRule::Halo &&
       traits.halo_radius != 0U)) {
    return Result<ValueDescriptor>(
        Status::failure(ErrorCode::InvalidArgument,
                        "operation semantic trait record is inconsistent"));
  }
  try {
    static_cast<void>(Value::element_size(traits.output_element_type));
  } catch (const std::invalid_argument&) {
    return Result<ValueDescriptor>(
        Status::failure(ErrorCode::TypeMismatch,
                        "operation semantic output element type is unknown"));
  }
  switch (traits.shape_rule) {
    case OperationShapeRule::Scalar:
      return Result<ValueDescriptor>(
          ValueDescriptor{traits.output_element_type, {1U}});
    case OperationShapeRule::PreserveFirstInput:
      if (inputs.empty() ||
          inputs.front().element_type != traits.output_element_type) {
        return Result<ValueDescriptor>(Status::failure(
            ErrorCode::TypeMismatch,
            "preserving operation has no compatible first input"));
      }
      return Result<ValueDescriptor>(inputs.front());
    case OperationShapeRule::MatchAllInputs:
      if (inputs.empty() ||
          inputs.front().element_type != traits.output_element_type) {
        return Result<ValueDescriptor>(Status::failure(
            ErrorCode::TypeMismatch,
            "matching operation has no compatible first input"));
      }
      for (const ValueDescriptor& input : inputs) {
        if (input.element_type != inputs.front().element_type ||
            input.shape != inputs.front().shape) {
          return Result<ValueDescriptor>(
              Status::failure(ErrorCode::TypeMismatch,
                              "matching operation input descriptors differ"));
        }
      }
      return Result<ValueDescriptor>(inputs.front());
  }
  return Result<ValueDescriptor>(Status::failure(
      ErrorCode::InvalidArgument, "operation shape rule is unknown"));
}

/**
 * @brief Builds the canonical semantic digest.
 * @param nodes Deterministic topological semantic nodes.
 * @param outputs Exact requested outputs.
 * @return Non-security digest text.
 * @throws std::bad_alloc If digest text allocation fails.
 * @note Graph revision is excluded so equal semantics across replacements
 * match.
 */
std::string semantic_digest(const std::vector<SemanticNode>& nodes,
                            const std::vector<WorkflowOutput>& outputs) {
  DigestBuilder digest;
  digest.text("semantic-graph-ir-v1");
  digest.integer(nodes.size());
  for (const SemanticNode& node : nodes) {
    digest.integer(node.id);
    digest.text(node.operation);
    digest.integer(node.inputs.size());
    for (std::uint64_t input : node.inputs) {
      digest.integer(input);
    }
    digest.integer(node.parameters.size());
    for (const auto& parameter : node.parameters) {
      digest.text(parameter.first);
      append_parameter(&digest, parameter.second);
    }
    append_traits(&digest, node.traits);
    append_descriptor(&digest, node.output_descriptor);
  }
  digest.integer(outputs.size());
  for (const WorkflowOutput& output : outputs) {
    digest.text(output.name);
    digest.integer(output.node_id);
    digest.text(output.port);
  }
  return digest.finish();
}

/**
 * @brief Builds the canonical optimized-stage digest.
 * @param semantic Parent semantic digest.
 * @param nodes Optimized deterministic nodes.
 * @param outputs Optimized outputs.
 * @return Non-security digest text.
 * @throws std::bad_alloc If digest text allocation fails.
 * @note Optimizer identity is explicit even for a no-op result.
 */
std::string optimized_digest(const std::string& semantic,
                             const std::vector<SemanticNode>& nodes,
                             const std::vector<WorkflowOutput>& outputs) {
  DigestBuilder digest;
  digest.text("optimizer-v1-canonical-noop");
  digest.text(semantic);
  digest.text(semantic_digest(nodes, outputs));
  return digest.finish();
}

/**
 * @brief Builds the canonical physical-plan digest.
 * @param optimized Parent optimized digest.
 * @param steps Validated physical steps.
 * @param outputs Named step indexes.
 * @return Non-security digest text.
 * @throws std::bad_alloc If digest text allocation fails.
 * @note Runtime availability/cancellation/timing is excluded.
 */
std::string physical_digest(const std::string& optimized,
                            const std::vector<PlanStep>& steps,
                            const std::map<std::string, std::size_t>& outputs) {
  DigestBuilder digest;
  digest.text("physical-plan-v1");
  digest.text(optimized);
  digest.integer(steps.size());
  for (const PlanStep& step : steps) {
    digest.integer(step.node_id);
    digest.text(step.operation);
    digest.integer(step.input_steps.size());
    for (std::size_t input : step.input_steps) {
      digest.integer(input);
    }
    digest.integer(static_cast<std::uint32_t>(step.backend));
    digest.integer(step.planned_bytes);
    append_traits(&digest, step.traits);
    append_descriptor(&digest, step.output_descriptor);
    digest.integer(step.parameters.size());
    for (const auto& parameter : step.parameters) {
      digest.text(parameter.first);
      append_parameter(&digest, parameter.second);
    }
  }
  digest.integer(outputs.size());
  for (const auto& output : outputs) {
    digest.text(output.first);
    digest.integer(output.second);
  }
  return digest.finish();
}

/**
 * @brief Builds a domain-separated disposable plan-cache lookup key.
 * @param plan Canonical physical-plan digest text.
 * @return Non-security cache-key text.
 * @throws std::bad_alloc If formatting allocation fails.
 * @note Cache presence never changes source/result authority.
 */
std::string plan_cache_key(const std::string& plan) {
  DigestBuilder digest;
  digest.text("plan-cache-key-v1");
  digest.text(plan);
  return digest.finish();
}

/**
 * @brief Converts a steady-clock duration to bounded microseconds.
 * @param duration Nonnegative steady duration.
 * @return Saturated uint64 microseconds.
 * @throws Nothing.
 * @note Used only for raw diagnostics.
 */
std::uint64_t microseconds(
    std::chrono::steady_clock::duration duration) noexcept {
  const auto count =
      std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
  if (count <= 0) {
    return 0U;
  }
  return static_cast<std::uint64_t>(count);
}

}  // namespace

/**
 * @brief Implements compiler construction over one frozen operation set.
 * @copydetails Compiler::Compiler
 */
Compiler::Compiler(std::shared_ptr<OperationRegistry> operations)
    : operations_(std::move(operations)) {
  if (!operations_ || !operations_->frozen()) {
    throw std::invalid_argument(
        "Compiler requires a frozen operation registry");
  }
}

/**
 * @brief Implements validated source-to-semantic lowering.
 * @copydetails Compiler::analyze
 */
Result<SemanticGraphIR> Compiler::analyze(const GraphSnapshot& snapshot) const {
  if (!snapshot.current()) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::Stale, "graph snapshot is stale"));
  }
  const WorkflowDocument& document = snapshot.document();
  if (document.schema_version != 1U || document.nodes.empty() ||
      document.nodes.size() > 65536U || document.outputs.empty() ||
      document.outputs.size() > 4096U) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::InvalidArgument,
                        "WorkflowDocument version/count bounds are invalid"));
  }

  std::unordered_map<std::uint64_t, const WorkflowNode*> nodes_by_id;
  nodes_by_id.reserve(document.nodes.size());
  std::unordered_map<std::uint64_t, std::size_t> indegree;
  std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> dependents;
  for (const WorkflowNode& node : document.nodes) {
    if (node.id == 0U || !valid_text(node.operation, 1024U) ||
        !nodes_by_id.emplace(node.id, &node).second ||
        node.parameters.size() > 1024U || node.inputs.size() > 1024U) {
      return Result<SemanticGraphIR>(Status::failure(
          ErrorCode::InvalidArgument,
          "workflow node id/key/count is invalid or duplicated"));
    }
    for (const auto& parameter : node.parameters) {
      if (!valid_text(parameter.first, 1024U) ||
          (std::holds_alternative<std::string>(parameter.second) &&
           std::get<std::string>(parameter.second).size() > 8192U)) {
        return Result<SemanticGraphIR>(Status::failure(
            ErrorCode::InvalidArgument, "workflow parameter is malformed"));
      }
    }
    auto traits = operations_->find_traits(node.operation);
    if (!traits.ok()) {
      return Result<SemanticGraphIR>(traits.status());
    }
    if (traits.value().input_count != node.inputs.size()) {
      return Result<SemanticGraphIR>(Status::failure(
          ErrorCode::TypeMismatch, "workflow operation input count mismatch"));
    }
    indegree.emplace(node.id, node.inputs.size());
  }

  for (const WorkflowNode& node : document.nodes) {
    for (const WorkflowInput& input : node.inputs) {
      if (input.source_node == 0U || input.source_port != "value" ||
          nodes_by_id.count(input.source_node) == 0U) {
        return Result<SemanticGraphIR>(
            Status::failure(ErrorCode::NotFound,
                            "workflow input references a missing producer"));
      }
      dependents[input.source_node].push_back(node.id);
    }
  }

  std::set<std::string> output_names;
  for (const WorkflowOutput& output : document.outputs) {
    if (!valid_text(output.name, 1024U) || output.port != "value" ||
        output.node_id == 0U || nodes_by_id.count(output.node_id) == 0U ||
        !output_names.insert(output.name).second) {
      return Result<SemanticGraphIR>(Status::failure(
          ErrorCode::InvalidArgument,
          "workflow output is malformed, missing, or duplicated"));
    }
  }

  std::priority_queue<std::uint64_t, std::vector<std::uint64_t>,
                      std::greater<std::uint64_t>>
      ready;
  for (const auto& entry : indegree) {
    if (entry.second == 0U) {
      ready.push(entry.first);
    }
  }

  SemanticGraphIR semantic;
  semantic.revision_ = snapshot.revision();
  semantic.nodes_.reserve(document.nodes.size());
  std::unordered_map<std::uint64_t, ValueDescriptor> output_by_node;
  output_by_node.reserve(document.nodes.size());
  while (!ready.empty()) {
    const std::uint64_t id = ready.top();
    ready.pop();
    const WorkflowNode& source = *nodes_by_id.at(id);
    auto traits = operations_->find_traits(source.operation);
    if (!traits.ok()) {
      return Result<SemanticGraphIR>(traits.status());
    }
    SemanticNode node;
    node.id = source.id;
    node.operation = source.operation;
    node.parameters = source.parameters;
    node.traits = traits.value();
    node.inputs.reserve(source.inputs.size());
    std::vector<ValueDescriptor> input_descriptors;
    input_descriptors.reserve(source.inputs.size());
    for (const WorkflowInput& input : source.inputs) {
      node.inputs.push_back(input.source_node);
      const auto descriptor = output_by_node.find(input.source_node);
      if (descriptor == output_by_node.end()) {
        return Result<SemanticGraphIR>(Status::failure(
            ErrorCode::Internal,
            "typed lowering could not find a producer descriptor"));
      }
      input_descriptors.push_back(descriptor->second);
    }
    auto output = infer_output_descriptor(node.traits, input_descriptors);
    if (!output.ok()) {
      return Result<SemanticGraphIR>(output.status());
    }
    node.output_descriptor = output.take_value();
    output_by_node.emplace(node.id, node.output_descriptor);
    semantic.nodes_.push_back(std::move(node));
    for (std::uint64_t dependent : dependents[id]) {
      std::size_t& count = indegree[dependent];
      if (count == 0U) {
        return Result<SemanticGraphIR>(Status::failure(
            ErrorCode::Internal, "workflow indegree underflow"));
      }
      --count;
      if (count == 0U) {
        ready.push(dependent);
      }
    }
  }
  if (semantic.nodes_.size() != document.nodes.size()) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::Cycle, "workflow graph contains a cycle"));
  }
  semantic.outputs_ = document.outputs;
  std::sort(semantic.outputs_.begin(), semantic.outputs_.end(),
            [](const WorkflowOutput& left, const WorkflowOutput& right) {
              return left.name < right.name;
            });
  semantic.digest_.value = semantic_digest(semantic.nodes_, semantic.outputs_);
  semantic.current_check_ = [snapshot]() noexcept {
    return snapshot.current();
  };
  semantic.operation_registry_ = operations_;
  if (!semantic.current()) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::Stale, "graph changed during analysis"));
  }
  return Result<SemanticGraphIR>(std::move(semantic));
}

/**
 * @brief Implements the conservative optimized-stage transition.
 * @copydetails Compiler::optimize
 */
Result<OptimizedGraphIR> Compiler::optimize(
    const SemanticGraphIR& semantic) const {
  const auto source_operations = semantic.operation_registry_.lock();
  if (semantic.revision() == 0U || semantic.nodes().empty() ||
      semantic.digest().value.empty() || !semantic.current() ||
      source_operations.get() != operations_.get()) {
    return Result<OptimizedGraphIR>(Status::failure(
        ErrorCode::Stale,
        "semantic IR is invalid, stale, or from another operation set"));
  }
  OptimizedGraphIR optimized;
  optimized.revision_ = semantic.revision();
  optimized.nodes_ = semantic.nodes();
  optimized.outputs_ = semantic.outputs();
  optimized.semantic_digest_ = semantic.digest();
  optimized.digest_.value = optimized_digest(
      optimized.semantic_digest_.value, optimized.nodes_, optimized.outputs_);
  optimized.current_check_ = semantic.current_check_;
  optimized.operation_registry_ = operations_;
  if (!optimized.current()) {
    return Result<OptimizedGraphIR>(
        Status::failure(ErrorCode::Stale, "graph changed during optimization"));
  }
  return Result<OptimizedGraphIR>(std::move(optimized));
}

/**
 * @brief Implements local physical plan construction.
 * @copydetails Compiler::plan
 */
Result<ExecutionPlan> Compiler::plan(const OptimizedGraphIR& optimized,
                                     const PlanningOptions& options) const {
  const auto source_operations = optimized.operation_registry_.lock();
  if (optimized.revision() == 0U || optimized.nodes().empty() ||
      optimized.digest().value.empty() || !optimized.current() ||
      source_operations.get() != operations_.get()) {
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::Stale,
        "optimized IR is invalid, stale, or from another operation set"));
  }
  std::unordered_map<std::uint64_t, std::size_t> step_by_node;
  step_by_node.reserve(optimized.nodes().size());
  ExecutionPlan plan;
  plan.revision_ = optimized.revision();
  plan.steps_.reserve(optimized.nodes().size());
  for (const SemanticNode& node : optimized.nodes()) {
    PlanStep step;
    step.node_id = node.id;
    step.operation = node.operation;
    step.parameters = node.parameters;
    step.traits = node.traits;
    step.output_descriptor = node.output_descriptor;
    step.backend = options.allow_gpu && node.traits.supports_gpu ? Backend::Gpu
                                                                 : Backend::Cpu;
    if (step.backend == Backend::Cpu && !node.traits.supports_cpu) {
      return Result<ExecutionPlan>(
          Status::failure(ErrorCode::BackendUnavailable,
                          "operation has no required CPU implementation"));
    }
    step.planned_bytes = node.traits.estimated_bytes;
    step.input_steps.reserve(node.inputs.size());
    for (std::uint64_t input : node.inputs) {
      const auto iterator = step_by_node.find(input);
      if (iterator == step_by_node.end() ||
          iterator->second >= plan.steps_.size()) {
        return Result<ExecutionPlan>(Status::failure(
            ErrorCode::Internal, "optimized IR input order is invalid"));
      }
      step.input_steps.push_back(iterator->second);
    }
    step_by_node.emplace(node.id, plan.steps_.size());
    plan.steps_.push_back(std::move(step));
  }
  for (const WorkflowOutput& output : optimized.outputs()) {
    const auto iterator = step_by_node.find(output.node_id);
    if (iterator == step_by_node.end() ||
        !plan.outputs_.emplace(output.name, iterator->second).second) {
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::Internal, "optimized output mapping is invalid"));
    }
  }
  plan.optimized_digest_ = optimized.digest();
  plan.digest_.value =
      physical_digest(plan.optimized_digest_.value, plan.steps_, plan.outputs_);
  plan.cache_key_.value = plan_cache_key(plan.digest_.value);
  plan.current_check_ = optimized.current_check_;
  plan.operation_registry_ = operations_;
  if (!plan.current()) {
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::Stale, "graph changed during planning"));
  }
  return Result<ExecutionPlan>(std::move(plan));
}

/**
 * @brief Implements the complete fail-before-publication compiler pipeline.
 * @copydetails Compiler::compile
 */
Result<CompiledWorkflow> Compiler::compile(
    const GraphContext& context, const PlanningOptions& options) const {
  const GraphSnapshot snapshot = context.snapshot();
  const auto analyze_start = std::chrono::steady_clock::now();
  auto semantic = analyze(snapshot);
  const auto analyze_end = std::chrono::steady_clock::now();
  if (!semantic.ok()) {
    return Result<CompiledWorkflow>(semantic.status());
  }
  const auto optimize_start = std::chrono::steady_clock::now();
  auto optimized = optimize(semantic.value());
  const auto optimize_end = std::chrono::steady_clock::now();
  if (!optimized.ok()) {
    return Result<CompiledWorkflow>(optimized.status());
  }
  const auto plan_start = std::chrono::steady_clock::now();
  auto physical = plan(optimized.value(), options);
  const auto plan_end = std::chrono::steady_clock::now();
  if (!physical.ok()) {
    return Result<CompiledWorkflow>(physical.status());
  }
  if (!snapshot.current()) {
    return Result<CompiledWorkflow>(
        Status::failure(ErrorCode::Stale, "graph changed during compilation"));
  }
  CompiledWorkflow compiled;
  compiled.semantic = semantic.take_value();
  compiled.optimized = optimized.take_value();
  compiled.plan = physical.take_value();
  compiled.diagnostics.analyze_us = microseconds(analyze_end - analyze_start);
  compiled.diagnostics.optimize_us =
      microseconds(optimize_end - optimize_start);
  compiled.diagnostics.plan_us = microseconds(plan_end - plan_start);
  return Result<CompiledWorkflow>(std::move(compiled));
}

}  // namespace ps
