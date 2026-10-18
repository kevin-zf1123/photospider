#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
using IRValueId = std::uint64_t;
using IRBlockId = std::uint64_t;
using IRCallId = std::uint64_t;

/** @brief One SSA Result definition. Zero is never a valid value identifier.
 * Call results may omit the expected schema before binding. Block parameters
 * require a validated fixed schema, including on loop backedges.
 */
struct PHOTOSPIDER_API SemanticOutput final {
  std::string key;
  std::shared_ptr<const SchemaTemplate> result_schema = {};
  IRValueId value = 0;
};

/** @brief A static operation call, independent of any frontend graph.
 * The block's call sequence defines execution order. This table's order only
 * defines metadata preparation order. Binding replaces caller-supplied traits
 * and preparation with registry-owned contracts; each call is prepared once.
 */
struct PHOTOSPIDER_API SemanticNode final {
  IRCallId id = 0;
  std::string operation;
  std::vector<IRValueId> inputs;
  std::map<std::string, ParameterValue> parameters;
  OperationTraits traits;
  std::shared_ptr<const PreparedOperation> prepared = {};
  std::vector<SemanticOutput> outputs;
};

struct PHOTOSPIDER_API IRBlockParameter final {
  IRValueId value = 0;
  std::shared_ptr<const SchemaTemplate> result_schema;
};
struct PHOTOSPIDER_API IRInputBinding final {
  std::uint64_t declaration_id = 0;
  IRValueId value = 0;
};
/** @brief Parallel value transfer to a block's parameters.
 * Every source is captured before any destination is replaced. Argument
 * schemas must exactly match the destination parameter schemas.
 */
struct PHOTOSPIDER_API IREdge final {
  IRBlockId target = 0;
  std::vector<IRValueId> arguments;
};
struct PHOTOSPIDER_API IRJump final {
  IREdge edge;
};
/** @brief Branch on a Result containing exactly one UInt8 sample.
 * Zero selects if_false; every other byte selects if_true. Neither successor
 * executes speculatively. Other tensor shapes and implicit truthiness fail
 * binding. The condition is a normal SSA use and must dominate this terminator.
 */
struct PHOTOSPIDER_API IRConditionalJump final {
  IRValueId condition = 0;
  IREdge if_true;
  IREdge if_false;
};
struct PHOTOSPIDER_API IRReturn final {
  std::vector<IRValueId> values;
};
using IRTerminator = std::variant<IRJump, IRConditionalJump, IRReturn>;

/** @brief A straight-line sequence ending in exactly one terminator.
 * All blocks must be reachable from entry. Loops are permitted; carried values
 * use typed block parameters. A call belongs to exactly one block.
 */
struct PHOTOSPIDER_API IRBlock final {
  IRBlockId id = 0;
  std::vector<IRBlockParameter> parameters;
  std::vector<IRCallId> calls;
  IRTerminator terminator = IRReturn{};
};
struct PHOTOSPIDER_API IRReturnDeclaration final {
  std::string name;
  std::shared_ptr<const SchemaTemplate> result_schema;
};
/** @brief Editable frontend-neutral SSA program accepted by Compiler::bind_ir.
 * Caller-supplied traits and preparation are ignored and rebound from the
 * registry. Mutable execution state and runtime payload are not part of the
 * program.
 * Input bindings cover external declarations exactly once. Return declarations
 * form the ordered function signature. Binding checks definitions, dominance,
 * edge arity, schemas and operation contracts before returning immutable IR.
 * Calls and values have independent nonzero identifier namespaces.
 */
struct PHOTOSPIDER_API IRProgram final {
  std::vector<WorkflowInputDeclaration> input_declarations;
  std::vector<IRInputBinding> input_bindings;
  std::vector<SemanticNode> calls;
  std::vector<IRBlock> blocks;
  IRBlockId entry = 0;
  std::vector<IRReturnDeclaration> returns;
};
}  // namespace ps
