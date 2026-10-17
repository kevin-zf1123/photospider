#include "photospider/compiler/compiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugin/dependency_identity.hpp"
#include "plugin/operation_identity.hpp"
#include "plugin/port_validation.hpp"

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
  void text(std::string_view value) noexcept {
    integer(value.size());
    bytes(value.data(), value.size());
  }

  /**
   * @brief Returns fixed lowercase hexadecimal text.
   * @return Sixteen-character digest.
   * @throws std::bad_alloc If stream/string allocation fails.
   * @note Stage domain/version text, not hexadecimal formatting, separates
   * intentionally incompatible compiler identities.
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
 * @note Full Unicode normalization is outside source schema validation.
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
    digest->integer(bits);
  } else if (const auto* boolean = std::get_if<bool>(&value)) {
    digest->integer(3U);
    digest->integer(*boolean ? 1U : 0U);
  } else {
    digest->integer(4U);
    digest->text(std::get<std::string>(value));
  }
}

/** @brief Encodes canonical declaration metadata without runtime payload. */
void append_declarations(
    DigestBuilder* digest,
    const std::vector<WorkflowInputDeclaration>& declarations) {
  digest->integer(declarations.size());
  for (const auto& declaration : declarations) {
    digest->integer(declaration.id);
    digest->text(declaration.name);
    digest->text(declaration.result_schema->canonical());
  }
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
std::string semantic_digest(
    const std::vector<SemanticNode>& nodes,
    const std::vector<WorkflowOutput>& outputs,
    const std::vector<WorkflowInputDeclaration>& declarations) {
  DigestBuilder digest;
  digest.text("semantic-graph-ir-v23");
  append_declarations(&digest, declarations);
  digest.integer(nodes.size());
  for (const SemanticNode& node : nodes) {
    digest.integer(node.id);
    digest.text(node.operation);
    digest.integer(node.inputs.size());
    for (const WorkflowInput& input : node.inputs) {
      if (const auto* node_source = std::get_if<WorkflowNodeOutput>(&input)) {
        digest.integer(1);
        digest.integer(node_source->source_node);
        digest.text(node_source->source_port);
      } else {
        digest.integer(2);
        digest.integer(std::get<WorkflowInputReference>(input).input_id);
      }
    }
    digest.integer(node.parameters.size());
    for (const auto& parameter : node.parameters) {
      digest.text(parameter.first);
      append_parameter(&digest, parameter.second);
    }
    contract_internal::append_traits(&digest, node.traits);
    const auto& resources = node.prepared->resource_ids();
    digest.integer(resources.size());
    for (const auto& resource : resources) {
      digest.integer(resource.byte_length);
      for (auto byte : resource.sha256)
        digest.integer(byte);
    }
    digest.integer(node.outputs.size());
    for (const auto& output : node.outputs) {
      digest.text(output.key);
      digest.integer(static_cast<bool>(output.result_schema));
      if (output.result_schema)
        digest.text(output.result_schema->canonical());
    }
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
std::string optimized_digest(
    const std::string& semantic, const std::vector<SemanticNode>& nodes,
    const std::vector<WorkflowOutput>& outputs,
    const std::vector<WorkflowInputDeclaration>& declarations) {
  DigestBuilder digest;
  digest.text("optimizer-v5-canonical-noop");
  digest.text(semantic);
  digest.text(semantic_digest(nodes, outputs, declarations));
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
std::string physical_digest(
    const std::string& optimized, const std::vector<PlanStep>& steps,
    const std::map<std::string, std::size_t>& outputs,
    const std::vector<WorkflowInputDeclaration>& declarations,
    const std::map<std::string, TileRequest>& output_requests,
    std::uint64_t tile_extent, ExecutionMode execution_mode) {
  DigestBuilder digest;
  digest.text("physical-plan-v23");
  digest.integer(static_cast<std::uint32_t>(execution_mode));
  digest.integer(tile_extent);
  digest.integer(output_requests.size());
  for (const auto& requested : output_requests) {
    digest.text(requested.first);
    digest.integer(requested.second.members.size());
    for (const auto& member : requested.second.members) {
      digest.integer(member.first);
      digest.integer(member.second.intervals().size());
      for (const auto interval : member.second.intervals()) {
        digest.integer(interval.begin);
        digest.integer(interval.end);
      }
    }
  }
  append_declarations(&digest, declarations);
  digest.text(optimized);
  digest.integer(steps.size());
  for (const PlanStep& step : steps) {
    digest.integer(step.node_id);
    digest.integer(step.output_index);
    digest.text(step.operation);
    digest.integer(step.inputs.size());
    for (const PlanInput& input : step.inputs) {
      if (const auto* source = std::get_if<PlanStepInput>(&input)) {
        digest.integer(1);
        digest.integer(source->step_index);
      } else {
        digest.integer(2);
        digest.integer(std::get<PlanWorkflowInput>(input).declaration_index);
      }
    }
    digest.integer(static_cast<std::uint32_t>(step.backend));
    contract_internal::append_traits(&digest, step.traits);
    digest.integer(static_cast<bool>(step.output_result_schema));
    if (step.output_result_schema)
      digest.text(step.output_result_schema->canonical());
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
  digest.text("plan-cache-key-v18");
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

Result<ExecutionPlan> ExecutionPlan::tile_plan(
    const std::string& name, const TileRequest& request) const {
  if (!current() || operation_registry_.expired())
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::Stale, "tile parent is stale"));
  const auto named = outputs_.find(name);
  if (named == outputs_.end())
    return Result<ExecutionPlan>(
        Status{ErrorCode::InvalidArgument, "unknown tile output"});
  const auto& step = steps_[named->second];
  auto resolved = request.resolve(step.tile_grids);
  if (!resolved.ok())
    return Result<ExecutionPlan>(resolved.status());
  if (!output_requests_.at(name).contains(resolved.value()))
    return Result<ExecutionPlan>(
        Status{ErrorCode::InvalidArgument, "tile exceeds requested output"});
  ExecutionPlan tile = *this;
  tile.outputs_ = {{name, named->second}};
  tile.output_requests_ = {{name, resolved.take_value()}};
  tile.digest_.value =
      physical_digest(tile.optimized_digest_.value, tile.steps_, tile.outputs_,
                      tile.input_declarations_, tile.output_requests_,
                      tile.tile_extent_, tile.execution_mode_);
  tile.cache_key_.value = plan_cache_key(tile.digest_.value);
  if (!current())
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::Stale, "dependency template changed"));
  return Result<ExecutionPlan>(std::move(tile));
}

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
Result<SemanticGraphIR> Compiler::analyze(const GraphSnapshot& snapshot,
                                          ResourceSet resources) const {
  if (!snapshot.current()) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::Stale, "graph snapshot is stale"));
  }
  const WorkflowDocument& document = snapshot.document();
  if (document.schema_version != 7U || document.nodes.empty() ||
      document.nodes.size() > 65536U || document.outputs.empty() ||
      document.outputs.size() > 4096U || document.inputs.size() > 4096U) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::InvalidArgument,
                        "WorkflowDocument version/count bounds are invalid"));
  }

  auto declarations = document.inputs;
  ResourceSet retained_resources;
  std::sort(declarations.begin(), declarations.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  std::map<std::uint64_t, std::size_t> declaration_by_id;
  std::set<std::string> input_names;
  for (std::size_t i = 0; i < declarations.size(); ++i) {
    const auto& declaration = declarations[i];
    if (declaration.id == 0 ||
        !input_internal::valid_input_name(declaration.name) ||
        !declaration_by_id.emplace(declaration.id, i).second ||
        !input_names.insert(declaration.name).second) {
      return Result<SemanticGraphIR>(Status::failure(
          ErrorCode::InvalidArgument, "invalid or duplicate input id/name"));
    }
  }
  for (auto& declaration : declarations) {
    const auto status = input_internal::validate_declaration(&declaration);
    if (!status.ok())
      return Result<SemanticGraphIR>(status);
  }

  std::unordered_map<std::uint64_t, const WorkflowNode*> nodes_by_id;
  nodes_by_id.reserve(document.nodes.size());
  std::unordered_map<std::uint64_t, std::size_t> indegree;
  std::map<std::pair<std::uint64_t, std::string>, ValueRef> result_ports;
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
    if ((!traits.value().repeated_maximum &&
         traits.value().input_count != node.inputs.size()) ||
        (traits.value().repeated_maximum &&
         (node.inputs.size() <
              traits.value().input_count + traits.value().repeated_minimum ||
          node.inputs.size() >
              traits.value().input_count + traits.value().repeated_maximum))) {
      return Result<SemanticGraphIR>(Status::failure(
          ErrorCode::TypeMismatch, "workflow operation input count mismatch"));
    }
    const Status parameter_status =
        validate_operation_parameters(traits.value(), node.parameters);
    if (!parameter_status.ok()) {
      return Result<SemanticGraphIR>(parameter_status);
    }
    indegree.emplace(node.id, 0);
    for (std::uint32_t i = 0; i < traits.value().outputs.size(); ++i) {
      const auto& output = traits.value().outputs[i];
      const ValueRef ref{node.id, i};
      result_ports.emplace(std::make_pair(node.id, output.key), ref);
    }
  }

  for (const WorkflowNode& node : document.nodes) {
    for (const WorkflowInput& input : node.inputs) {
      if (const auto* source = std::get_if<WorkflowNodeOutput>(&input)) {
        if (source->source_node == 0 ||
            !result_ports.count({source->source_node, source->source_port}) ||
            nodes_by_id.count(source->source_node) == 0) {
          return Result<SemanticGraphIR>(
              Status::failure(ErrorCode::NotFound,
                              "workflow input references a missing producer"));
        }
        dependents[source->source_node].push_back(node.id);
        ++indegree[node.id];
      } else if (declaration_by_id.count(
                     std::get<WorkflowInputReference>(input).input_id) == 0) {
        return Result<SemanticGraphIR>(
            Status::failure(ErrorCode::NotFound,
                            "workflow input references a missing declaration"));
      }
    }
  }

  std::set<std::string> output_names;
  for (const WorkflowOutput& output : document.outputs) {
    if (!valid_text(output.name, 1024U) ||
        !result_ports.count({output.node_id, output.port}) ||
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
  semantic.input_declarations_ = declarations;
  semantic.nodes_.reserve(document.nodes.size());
  std::map<ValueRef, std::shared_ptr<const SchemaTemplate>> result_schemas;
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
    auto resolved = resolve_operation_traits(
        traits.value(), source.inputs.size(), source.parameters);
    if (!resolved.ok())
      return Result<SemanticGraphIR>(resolved.status());
    node.traits = resolved.take_value();
    node.inputs.reserve(source.inputs.size());
    std::vector<OperationMetadata> input_metadata;
    input_metadata.reserve(source.inputs.size());
    for (std::size_t position = 0; position < source.inputs.size();
         ++position) {
      const auto& input = source.inputs[position];
      const auto& port = node.traits.input_schema[position];
      node.inputs.push_back(input);
      std::shared_ptr<const SchemaTemplate> result_schema;
      if (const auto* producer = std::get_if<WorkflowNodeOutput>(&input)) {
        const auto ref =
            result_ports.at({producer->source_node, producer->source_port});
        result_schema = result_schemas.at(ref);
      } else {
        const auto id = std::get<WorkflowInputReference>(input).input_id;
        const auto& declaration = declarations[declaration_by_id.at(id)];
        result_schema = declaration.result_schema;
      }
      auto status = input_internal::validate_port_metadata(
          port, OperationMetadata{result_schema});
      if (!status.ok()) {
        if (status.detail.origin == FailureOrigin::Unspecified &&
            (status.code == ErrorCode::TypeMismatch ||
             status.code == ErrorCode::InvalidArgument)) {
          status.detail.origin = FailureOrigin::Schema;
          if (status.code == ErrorCode::InvalidArgument &&
              status.reason == FailureReason::None)
            status.reason = FailureReason::InvalidDomain;
        }
        return Result<SemanticGraphIR>(status);
      }
      input_metadata.push_back({std::move(result_schema)});
    }
    auto specialized = operations_->prepare_operation(
        source.operation, input_metadata, source.parameters, resources);
    if (!specialized.ok())
      return Result<SemanticGraphIR>(specialized.status());
    node.prepared = specialized.take_value();
    auto selected_resources = resources.select(node.prepared->resource_ids());
    if (!selected_resources.ok())
      return Result<SemanticGraphIR>(selected_resources.status());
    auto joined_resources =
        retained_resources.unite(selected_resources.value());
    if (!joined_resources.ok())
      return Result<SemanticGraphIR>(joined_resources.status());
    retained_resources = joined_resources.take_value();
    node.traits = node.prepared->traits();
    auto output =
        infer_operation_outputs(node.traits, input_metadata, node.parameters);
    if (!output.ok())
      return Result<SemanticGraphIR>(output.status());
    for (std::uint32_t oi = 0; oi < node.traits.outputs.size(); ++oi) {
      const auto& contract = node.traits.outputs[oi];
      const auto& metadata = output.value()[oi];
      const ValueRef ref{node.id, oi};
      result_schemas.emplace(ref, metadata.result_schema);
      node.outputs.push_back({contract.key, metadata.result_schema});
    }
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
  semantic.resources_ = std::move(retained_resources);
  semantic.outputs_ = document.outputs;
  std::sort(semantic.outputs_.begin(), semantic.outputs_.end(),
            [](const WorkflowOutput& left, const WorkflowOutput& right) {
              return left.name < right.name;
            });
  semantic.digest_.value = semantic_digest(semantic.nodes_, semantic.outputs_,
                                           semantic.input_declarations_);
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
  optimized.input_declarations_ = semantic.input_declarations();
  optimized.resources_ = semantic.resources();
  optimized.outputs_ = semantic.outputs();
  optimized.semantic_digest_ = semantic.digest();
  optimized.digest_.value =
      optimized_digest(optimized.semantic_digest_.value, optimized.nodes_,
                       optimized.outputs_, optimized.input_declarations_);
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
  if (options.execution_mode != ExecutionMode::CpuExact &&
      options.execution_mode != ExecutionMode::NativeGpu)
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::InvalidArgument, "unknown execution mode"));
  if (!options.tile_extent || (options.tile_extent & (options.tile_extent - 1)))
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::InvalidArgument,
                        "tile extent must be a positive power of two"));
  const auto source_operations = optimized.operation_registry_.lock();
  if (optimized.revision() == 0U || optimized.nodes().empty() ||
      optimized.digest().value.empty() || !optimized.current() ||
      source_operations.get() != operations_.get()) {
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::Stale,
        "optimized IR is invalid, stale, or from another operation set"));
  }
  std::map<ValueRef, std::size_t> step_by_value;
  std::map<std::pair<std::uint64_t, std::string>, ValueRef> result_ports;
  for (const auto& node : optimized.nodes())
    for (std::uint32_t oi = 0; oi < node.outputs.size(); ++oi)
      result_ports.emplace(std::make_pair(node.id, node.outputs[oi].key),
                           ValueRef{node.id, oi});
  // Retain the metadata closure of output-reachable results, including excluded
  // ports. Runtime demand only follows selected relevant ports. Side-effecting
  // singleton roots remain explicit even without a named consumer.
  std::set<ValueRef> needed, executable;
  for (const auto& output : optimized.outputs())
    executable.insert(result_ports.at({output.node_id, output.port}));
  needed = executable;
  for (auto it = optimized.nodes().rbegin(); it != optimized.nodes().rend();
       ++it) {
    const auto& node = *it;
    if (!node.traits.side_effect_free ||
        std::any_of(node.traits.shared_stages.begin(),
                    node.traits.shared_stages.end(),
                    [](const auto& stage) { return stage.mandatory; })) {
      needed.insert({node.id, 0});
      executable.insert({node.id, 0});
    }
    for (std::uint32_t oi = 0; oi < node.outputs.size(); ++oi) {
      if (!executable.count({node.id, oi}))
        continue;
      const auto& projection = node.traits.outputs[oi].input_indices;
      for (std::uint32_t input = 0; input < node.inputs.size(); ++input) {
        if (projection && std::find(projection->begin(), projection->end(),
                                    input) == projection->end())
          continue;
        if (const auto* producer =
                std::get_if<WorkflowNodeOutput>(&node.inputs[input]))
          executable.insert(
              result_ports.at({producer->source_node, producer->source_port}));
      }
    }
    bool used = false;
    for (std::uint32_t oi = 0; oi < node.outputs.size(); ++oi)
      used = used || needed.count({node.id, oi});
    if (used)
      for (const auto& input : node.inputs)
        if (const auto* producer = std::get_if<WorkflowNodeOutput>(&input))
          needed.insert(
              result_ports.at({producer->source_node, producer->source_port}));
  }
  ExecutionPlan plan;
  plan.execution_mode_ = options.execution_mode;
  plan.tile_extent_ = options.tile_extent;
  plan.revision_ = optimized.revision();
  plan.input_declarations_ = optimized.input_declarations();
  plan.resources_ = optimized.resources();
  for (const auto& input : plan.input_declarations_) {
    auto grid = TileGrid::members(*input.result_schema, options.tile_extent);
    if (!grid.ok())
      return Result<ExecutionPlan>(grid.status());
    plan.input_tile_grids_.push_back(grid.take_value());
  }
  std::map<std::uint64_t, std::size_t> declaration_by_id;
  for (std::size_t i = 0; i < plan.input_declarations_.size(); ++i)
    declaration_by_id.emplace(plan.input_declarations_[i].id, i);
  plan.steps_.reserve(optimized.nodes().size());
  for (const SemanticNode& node : optimized.nodes()) {
    for (std::uint32_t oi = 0; oi < node.outputs.size(); ++oi) {
      if (!needed.count({node.id, oi}))
        continue;
      const auto& output = node.outputs[oi];
      PlanStep step;
      step.output_index = oi;
      step.node_id = node.id;
      step.operation = node.operation;
      step.parameters = node.parameters;
      step.prepared = node.prepared;
      step.traits = select_operation_output(node.traits, oi).take_value();
      step.output_result_schema = output.result_schema;
      if (step.output_result_schema) {
        auto grid =
            TileGrid::members(*step.output_result_schema, options.tile_extent);
        if (!grid.ok())
          return Result<ExecutionPlan>(grid.status());
        step.tile_grids = grid.take_value();
      }
      step.backend =
          executable.count({node.id, oi}) &&
                  options.execution_mode == ExecutionMode::NativeGpu &&
                  node.traits.supports_gpu
              ? Backend::Gpu
              : Backend::Cpu;
      if (executable.count({node.id, oi}) && step.backend == Backend::Cpu &&
          !node.traits.supports_cpu) {
        return Result<ExecutionPlan>(
            Status::failure(ErrorCode::BackendUnavailable,
                            "operation has no required CPU implementation"));
      }
      step.inputs.reserve(node.inputs.size());
      for (const WorkflowInput& input : node.inputs) {
        if (const auto* source = std::get_if<WorkflowNodeOutput>(&input)) {
          const auto iterator = step_by_value.find(
              result_ports.at({source->source_node, source->source_port}));
          if (iterator == step_by_value.end() ||
              iterator->second >= plan.steps_.size()) {
            return Result<ExecutionPlan>(Status::failure(
                ErrorCode::Internal, "optimized IR input order is invalid"));
          }
          step.inputs.push_back(PlanStepInput{iterator->second});
        } else {
          step.inputs.push_back(PlanWorkflowInput{declaration_by_id.at(
              std::get<WorkflowInputReference>(input).input_id)});
        }
      }
      step_by_value.emplace(ValueRef{node.id, oi}, plan.steps_.size());
      plan.steps_.push_back(std::move(step));
    }
  }
  for (const WorkflowOutput& output : optimized.outputs()) {
    const auto iterator =
        step_by_value.find(result_ports.at({output.node_id, output.port}));
    if (iterator == step_by_value.end() ||
        !plan.outputs_.emplace(output.name, iterator->second).second) {
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::Internal, "optimized output mapping is invalid"));
    }
  }
  if (options.output_requests.size() > plan.outputs_.size()) {
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::InvalidArgument,
        "planning options contain too many named output requests"));
  }
  for (const auto& requested : options.output_requests)
    if (!plan.outputs_.count(requested.first))
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::InvalidArgument, "unknown dependency output"));
  for (const auto& output : plan.outputs_) {
    const auto& step = plan.steps_[output.second];
    const auto requested = options.output_requests.find(output.first);
    auto resolved = requested == options.output_requests.end()
                        ? TileRequest::all(step.tile_grids, ResourceBudget{})
                        : requested->second.resolve(step.tile_grids);
    if (!resolved.ok())
      return Result<ExecutionPlan>(resolved.status());
    plan.output_requests_.emplace(output.first, resolved.take_value());
  }
  plan.optimized_digest_ = optimized.digest();
  plan.digest_.value =
      physical_digest(plan.optimized_digest_.value, plan.steps_, plan.outputs_,
                      plan.input_declarations_, plan.output_requests_,
                      plan.tile_extent_, plan.execution_mode_);
  plan.cache_key_.value = plan_cache_key(plan.digest_.value);
  plan.current_check_ = optimized.current_check_;
  plan.operation_registry_ = operations_;
  for (auto& step : plan.steps_) {
    auto metadata = std::make_shared<ResultProgramMetadata>();
    metadata->output = {step.output_result_schema};
    metadata->output_tile_grids = step.tile_grids;
    for (const auto& input : step.inputs) {
      if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
        const auto& source = plan.steps_[producer->step_index];
        metadata->input_tile_grids.push_back(source.tile_grids);
        metadata->inputs.push_back({source.output_result_schema});
      } else {
        metadata->input_tile_grids.push_back(
            plan.input_tile_grids_[std::get<PlanWorkflowInput>(input)
                                       .declaration_index]);
        const auto& source =
            plan.input_declarations_[std::get<PlanWorkflowInput>(input)
                                         .declaration_index];
        metadata->inputs.push_back({source.result_schema});
      }
    }
    step.structured_metadata = std::move(metadata);
  }
  std::map<std::uint64_t, std::vector<std::shared_ptr<const PlanStep>>>
      stage_plans;
  for (auto& step : plan.steps_) {
    auto found = stage_plans.find(step.node_id);
    if (found == stage_plans.end()) {
      std::vector<std::shared_ptr<const PlanStep>> stages;
      for (const auto& declared : step.prepared->traits().shared_stages) {
        auto stage = std::make_shared<PlanStep>(step);
        stage->shared_stages.clear();
        stage->output_result_schema =
            std::make_shared<const SchemaTemplate>(declared.result_schema);
        auto grids =
            TileGrid::members(declared.result_schema, plan.tile_extent_);
        if (!grids.ok())
          return Result<ExecutionPlan>(grids.status());
        stage->tile_grids = grids.take_value();
        stage->backend = declared.backend;
        stage->traits.cacheable = false;
        stage->traits.workspace_bytes = declared.workspace_bytes;
        auto& output = stage->traits.outputs[0];
        output.result_schema = declared.result_schema;
        output.output_schema = {};
        output.output_schema.result_schema_id = declared.result_schema.id;
        output.output_schema.result_schema_version =
            declared.result_schema.version;
        output.input_indices = declared.input_indices;
        output.continuation_bytes = declared.continuation_bytes;
        output.maximum_dependency_stages = declared.maximum_dependency_stages;
        output.maximum_output_payload_bytes =
            declared.maximum_output_payload_bytes;
        auto metadata =
            std::make_shared<ResultProgramMetadata>(*step.structured_metadata);
        metadata->output = {stage->output_result_schema};
        metadata->output_tile_grids = stage->tile_grids;
        stage->structured_metadata = std::move(metadata);
        stages.push_back(std::move(stage));
      }
      found = stage_plans.emplace(step.node_id, std::move(stages)).first;
    }
    step.shared_stages = found->second;
  }
  auto work = UINT64_MAX;
  plan.structured_templates_ =
      contract_internal::dependency_cache_templates(plan, {}, &work, {});
  if (!plan.current())
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::Stale, "dependency template changed"));
  return Result<ExecutionPlan>(std::move(plan));
}

/**
 * @brief Implements the complete fail-before-publication compiler pipeline.
 * @copydetails Compiler::compile
 */
Result<CompiledWorkflow> Compiler::compile(const GraphContext& context,
                                           const PlanningOptions& options,
                                           ResourceSet resources) const {
  const GraphSnapshot snapshot = context.snapshot();
  const auto analyze_start = std::chrono::steady_clock::now();
  auto semantic = analyze(snapshot, std::move(resources));
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
