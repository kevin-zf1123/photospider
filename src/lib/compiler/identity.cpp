#include "compiler/identity.hpp"

#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/operation_identity.hpp"

namespace ps::compiler_internal {
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
   * @throws std::bad_alloc If string allocation fails.
   * @note Stage domain/version text, not hexadecimal formatting, separates
   * intentionally incompatible compiler identities.
   */
  [[nodiscard]] std::string finish() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    auto remaining = value_;
    for (std::size_t index = result.size(); index > 0; --index) {
      result[index - 1] = digits[remaining & 0xfU];
      remaining >>= 4U;
    }
    return result;
  }

 private:
  /** @brief Current FNV-1a state. */
  std::uint64_t value_ = 14695981039346656037ULL;
};

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

namespace {
template <class Values>
void append_ids(DigestBuilder* d, const Values& values) {
  d->integer(values.size());
  for (auto value : values)
    d->integer(value);
}
void append_input(DigestBuilder* d, const PlanInput& input) {
  d->integer(input.index());
  if (const auto* value = std::get_if<PlanStepInput>(&input))
    d->integer(value->step_index);
  else if (const auto* value = std::get_if<PlanWorkflowInput>(&input))
    d->integer(value->declaration_index);
  else
    d->integer(std::get<PlanArgumentInput>(input).parameter_index);
}
void append_request(DigestBuilder* d, const TileRequest& request) {
  d->integer(request.members.size());
  for (const auto& member : request.members) {
    d->integer(member.first);
    d->integer(member.second.intervals().size());
    for (auto interval : member.second.intervals()) {
      d->integer(interval.begin);
      d->integer(interval.end);
    }
  }
}
void append_samples(DigestBuilder* d, const Footprint& samples) {
  d->integer(samples.valid());
  if (!samples.valid())
    return;
  append_ids(d, samples.shape());
  d->integer(samples.boxes().size());
  for (const auto& box : samples.boxes()) {
    d->integer(box.rank());
    for (const auto& dimension : box.dimensions()) {
      d->integer(dimension.offset);
      d->integer(dimension.extent);
    }
  }
}
void append_sample_members(DigestBuilder* d,
                           const std::vector<Footprint>& members) {
  d->integer(members.size());
  for (const auto& samples : members)
    append_samples(d, samples);
}
void append_edge(DigestBuilder* d, const IREdge& edge) {
  d->integer(edge.target);
  append_ids(d, edge.arguments);
}
void append_edge(DigestBuilder* d, const PlanEdge& edge) {
  d->integer(edge.target);
  d->integer(edge.arguments.size());
  for (const auto& input : edge.arguments)
    append_input(d, input);
}
}  // namespace

std::string ir_digest(const IRProgram& program) {
  DigestBuilder digest;
  digest.text("validated-ssa-ir-v26");
  digest.integer(static_cast<std::uint64_t>(program.form));
  append_declarations(&digest, program.input_declarations);
  digest.integer(program.input_bindings.size());
  for (const auto& binding : program.input_bindings) {
    digest.integer(binding.declaration_id);
    digest.integer(binding.value);
  }
  digest.integer(program.calls.size());
  for (const auto& node : program.calls) {
    digest.integer(node.id);
    digest.text(node.operation);
    append_ids(&digest, node.inputs);
    digest.integer(node.parameters.size());
    for (const auto& parameter : node.parameters) {
      digest.text(parameter.first);
      append_parameter(&digest, parameter.second);
    }
    contract_internal::append_traits(&digest, node.traits);
    digest.integer(node.prepared->resource_ids().size());
    for (const auto& resource : node.prepared->resource_ids()) {
      digest.integer(resource.byte_length);
      digest.bytes(resource.sha256.data(), resource.sha256.size());
    }
    digest.integer(node.outputs.size());
    for (const auto& output : node.outputs) {
      digest.integer(output.value);
      digest.text(output.key);
      digest.text(output.result_schema->canonical());
    }
  }
  digest.integer(program.returns.size());
  for (const auto& output : program.returns) {
    digest.text(output.name);
    digest.text(output.result_schema->canonical());
  }
  digest.integer(program.entry);
  digest.integer(program.blocks.size());
  for (const auto& block : program.blocks) {
    digest.integer(block.id);
    digest.integer(block.parameters.size());
    for (const auto& parameter : block.parameters) {
      digest.integer(parameter.value);
      digest.text(parameter.result_schema->canonical());
    }
    append_ids(&digest, block.calls);
    digest.integer(block.terminator.index());
    if (const auto* jump = std::get_if<IRJump>(&block.terminator)) {
      append_edge(&digest, jump->edge);
    } else if (const auto* branch =
                   std::get_if<IRConditionalJump>(&block.terminator)) {
      digest.integer(branch->condition);
      append_edge(&digest, branch->if_true);
      append_edge(&digest, branch->if_false);
    } else {
      append_ids(&digest, std::get<IRReturn>(block.terminator).values);
    }
  }
  return digest.finish();
}
std::string physical_digest(const ExecutionPlan& plan) {
  DigestBuilder digest;
  digest.text("static-tile-program-v27");
  digest.text(plan.input_ir_digest().value);
  digest.integer(plan.tile_extent());
  digest.integer(static_cast<std::uint32_t>(plan.execution_mode()));
  append_declarations(&digest, plan.input_declarations());
  digest.integer(plan.parameters().size());
  for (const auto& parameter : plan.parameters()) {
    digest.integer(parameter.value);
    digest.integer(parameter.block);
    digest.text(parameter.result_schema->canonical());
  }
  digest.integer(plan.steps().size());
  for (const auto& step : plan.steps()) {
    digest.integer(step.node_id);
    digest.integer(step.block);
    digest.integer(step.value);
    digest.integer(step.output_index);
    digest.text(step.operation);
    digest.integer(static_cast<std::uint32_t>(step.backend));
    contract_internal::append_traits(&digest, step.traits);
    digest.integer(step.parameters.size());
    for (const auto& parameter : step.parameters) {
      digest.text(parameter.first);
      append_parameter(&digest, parameter.second);
    }
    digest.integer(step.inputs.size());
    for (const auto& input : step.inputs)
      append_input(&digest, input);
    digest.text(step.output_result_schema->canonical());
    digest.integer(step.prepared->resource_ids().size());
    for (const auto& resource : step.prepared->resource_ids()) {
      digest.integer(resource.byte_length);
      digest.bytes(resource.sha256.data(), resource.sha256.size());
    }
  }
  digest.integer(plan.return_signature().size());
  for (const auto& output : plan.return_signature()) {
    digest.text(output.name);
    digest.text(output.result_schema->canonical());
  }
  digest.integer(plan.outputs().size());
  for (const auto& named : plan.outputs()) {
    digest.text(named.first);
    digest.integer(named.second);
    append_request(&digest, plan.output_requests().at(named.first));
  }
  digest.integer(plan.entry());
  digest.integer(plan.blocks().size());
  for (const auto& block : plan.blocks()) {
    digest.integer(block.id);
    append_ids(&digest, block.parameters);
    append_ids(&digest, block.calls);
    digest.integer(block.instructions.size());
    for (const auto& instruction : block.instructions) {
      digest.integer(instruction.step_index);
      digest.integer(instruction.shared_stage);
      digest.integer(instruction.tensor_slot);
      digest.integer(instruction.tile_unit);
      digest.integer(instruction.cpu_fallback);
      append_ids(&digest, instruction.dependencies);
      append_request(&digest, instruction.publication);
      append_sample_members(&digest, instruction.publication_samples);
      digest.integer(instruction.tensor_outputs.has_value());
      if (instruction.tensor_outputs)
        append_samples(&digest, *instruction.tensor_outputs);
      digest.integer(instruction.inputs.size());
      for (const auto& read : instruction.inputs) {
        digest.integer(read.input);
        digest.integer(read.tensor_slot);
        digest.integer(read.descriptor_only);
        TileRequest request;
        request.members.emplace(read.tensor_slot, read.tiles);
        append_request(&digest, request);
        append_samples(&digest, read.samples);
      }
      digest.integer(instruction.shared_inputs.size());
      for (const auto& read : instruction.shared_inputs) {
        digest.integer(read.stage);
        append_request(&digest, read.tiles);
        append_sample_members(&digest, read.samples);
      }
    }
    digest.integer(block.terminator.index());
    if (const auto* jump = std::get_if<PlanJump>(&block.terminator)) {
      append_edge(&digest, jump->edge);
    } else if (const auto* branch =
                   std::get_if<PlanConditionalJump>(&block.terminator)) {
      append_input(&digest, branch->condition);
      append_edge(&digest, branch->if_true);
      append_edge(&digest, branch->if_false);
    } else {
      const auto& returned = std::get<PlanReturn>(block.terminator);
      digest.integer(returned.values.size());
      for (const auto& value : returned.values)
        append_input(&digest, value);
    }
  }
  return digest.finish();
}
std::string plan_cache_key(const std::string& plan) {
  DigestBuilder digest;
  digest.text("plan-cache-key-v20");
  digest.text(plan);
  return digest.finish();
}
std::string operation_digest(const OperationTraits& traits) {
  DigestBuilder digest;
  digest.text("artifact-static-operation-contract-v2");
  contract_internal::append_traits(&digest, traits);
  return digest.finish();
}
}  // namespace ps::compiler_internal
