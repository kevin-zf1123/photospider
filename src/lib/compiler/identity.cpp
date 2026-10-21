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
void append_edge(DigestBuilder* d, const IREdge& edge) {
  d->integer(edge.target);
  append_ids(d, edge.arguments);
}
}  // namespace

std::string ir_digest(const IRProgram& program) {
  DigestBuilder digest;
  digest.text("ordered-retirement-ir-v5");
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
  DigestBuilder d;
  d.text("physical-task-program-v5");
  d.text(plan.input_ir_digest().value);
  d.integer(static_cast<std::uint32_t>(plan.execution_mode()));
  append_declarations(&d, plan.input_declarations());
  const auto& p = plan.program();
  d.integer(p.entry);
  d.integer(p.tile_extent);
  d.integer(p.schemas.size());
  for (const auto& schema : p.schemas)
    d.text(schema->canonical());
  d.integer(p.layouts.size());
  for (const auto& layout : p.layouts) {
    d.integer(static_cast<std::uint32_t>(layout.element_type));
    d.integer(layout.rank);
    append_ids(&d, layout.shape);
    append_ids(&d, layout.origin);
    append_ids(&d, layout.strides);
    d.integer(layout.byte_offset);
  }
  d.integer(p.tiles.size());
  for (const auto& tile : p.tiles) {
    d.integer(tile.value);
    d.integer(tile.member);
    d.integer(tile.layout);
    d.integer(tile.producer);
    d.integer(static_cast<std::uint32_t>(tile.domain));
    d.integer(tile.tile_index);
    d.integer(tile.bytes);
    d.integer(tile.alignment);
  }
  d.integer(p.values.size());
  for (const auto& value : p.values) {
    d.integer(value.source_value);
    d.integer(value.schema);
    d.integer(value.owner);
    d.integer(static_cast<std::uint32_t>(value.kind));
    d.integer(value.import_source);
    d.integer(static_cast<std::uint32_t>(value.input_layout));
    append_ids(&d, value.tiles);
  }
  d.integer(p.placements.size());
  for (const auto& placement : p.placements) {
    d.integer(static_cast<std::uint32_t>(placement.backend));
    d.integer(static_cast<std::uint32_t>(placement.domain));
    d.text(placement.profile);
    d.text(placement.queue);
  }
  d.integer(p.kernels.size());
  for (const auto& kernel : p.kernels) {
    d.text(kernel.operation);
    d.text(kernel.symbol);
    d.text(kernel.contract);
    d.integer(kernel.abi_version);
  }
  d.integer(p.tasks.size());
  for (const auto& task : p.tasks) {
    d.integer(static_cast<std::uint32_t>(task.kind));
    d.integer(task.call);
    d.integer(task.kernel);
    d.integer(task.placement);
    d.integer(task.reads.size());
    for (const auto& read : task.reads) {
      d.integer(read.port);
      d.integer(read.tile);
      d.integer(read.layout);
    }
    append_ids(&d, task.writes);
    append_ids(&d, task.schema_reads);
    append_ids(&d, task.parameters);
    append_ids(&d, task.explicit_dependencies);
    append_ids(&d, task.predecessors);
    append_ids(&d, task.successors);
    append_ids(&d, task.scratch);
  }
  d.integer(p.calls.size());
  for (const auto& call : p.calls) {
    d.integer(call.source_call);
    d.integer(call.block);
    d.integer(call.ordinal);
    d.integer(call.effect_task);
    append_ids(&d, call.tasks);
    append_ids(&d, call.returns);
  }
  d.integer(p.blocks.size());
  for (const auto& block : p.blocks) {
    d.integer(block.source_block);
    append_ids(&d, block.tasks);
    append_ids(&d, block.calls);
    append_ids(&d, block.arguments);
    append_ids(&d, block.live_ins);
    d.integer(static_cast<std::uint32_t>(block.end));
    d.integer(block.condition);
    d.integer(block.edges.size());
    for (const auto& edge : block.edges) {
      d.integer(edge.target);
      append_ids(&d, edge.arguments);
      append_ids(&d, edge.live_values);
    }
    append_ids(&d, block.return_values);
    append_ids(&d, block.reservation.outputs);
    for (const auto& scratch : block.reservation.scratch)
      append_ids(&d, scratch);
  }
  append_ids(&d, p.inputs);
  append_ids(&d, p.return_schemas);
  d.integer(plan.calls().size());
  for (const auto& call : plan.calls()) {
    d.text(call.operation);
    append_ids(&d, call.inputs);
    append_ids(&d, call.outputs);
    contract_internal::append_traits(&d, call.prepared->traits());
    d.integer(call.parameters.size());
    for (const auto& parameter : call.parameters) {
      d.text(parameter.first);
      append_parameter(&d, parameter.second);
    }
    d.integer(call.prepared->resource_ids().size());
    for (const auto& resource : call.prepared->resource_ids()) {
      d.integer(resource.byte_length);
      d.bytes(resource.sha256.data(), resource.sha256.size());
    }
  }
  d.integer(plan.return_signature().size());
  for (const auto& output : plan.return_signature()) {
    d.text(output.name);
    d.text(output.result_schema->canonical());
  }
  d.integer(plan.outputs().size());
  for (const auto& output : plan.outputs()) {
    d.text(output.first);
    d.integer(output.second);
    append_request(&d, plan.output_requests().at(output.first));
  }
  return d.finish();
}
std::string operation_digest(const OperationTraits& traits) {
  DigestBuilder digest;
  digest.text("fixed-operation-contract-v3");
  contract_internal::append_traits(&digest, traits);
  return digest.finish();
}
}  // namespace ps::compiler_internal
