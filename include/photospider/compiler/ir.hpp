#pragma once

#include <cstdint>
#include <functional>
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

/** @brief Interpretation of the program's block call lists.
 * DependencyPaths is a raw, linear sequence of dependency paths. Repeated
 * call IDs request the same DAG invocation and SSA results, including effects.
 * Planning or an optional optimizer resolves requests to unique call sites.
 * ControlFlow supports branches and loops; each call has exactly one site.
 */
enum class IRForm { ControlFlow, DependencyPaths };

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
 * ControlFlow block calls define execution order; DependencyPaths block calls
 * are repeated requests for the same invocation. This table's order only
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
 * use typed block parameters. In ControlFlow a call belongs to exactly one
 * block. DependencyPaths permits repeated call IDs in linear, parameterless
 * blocks; these are dependency requests, not repeated operation invocations.
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
  IRForm form = IRForm::ControlFlow;
};

/**
 * @brief Typed non-security identity of canonical semantic graph bytes.
 *
 * @note This value is not substitutable for plan or cache identity.
 */
struct PHOTOSPIDER_API IRDigest final {
  /** @brief Fixed lowercase hexadecimal representation. */
  std::string value;
};

/** @brief Optional nonsemantic origin of an immutable IR version. */
struct PHOTOSPIDER_API IRProvenance final {
  std::string parent_digest;
  std::string producer;
  std::string version;
  std::map<std::string, ParameterValue> options;
};

/**
 * @brief Immutable typed semantic graph or raw dependency-path program.
 *
 * @note Call-table order is deterministic metadata order. Blocks own path
 * requests or executable control flow as selected by form().
 */
class PHOTOSPIDER_API ValidatedIR final {
 public:
  /**
   * @brief Constructs an invalid semantic IR placeholder.
   * @throws Nothing.
   * @note Analysis and artifact loading produce IR that may be optimized.
   */
  ValidatedIR() noexcept = default;

  /**
   * @brief Returns a captured graph revision, or one for a loaded artifact.
   * @return Source revision, or zero for a default placeholder.
   * @throws Nothing.
   * @note The value participates in stale-publication rejection.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  /**
   * @brief Returns prepared call declarations in deterministic metadata order.
   * @return Immutable borrowed node sequence owned by this IR.
   * @throws Nothing.
   * @note The reference remains valid for this IR object's lifetime.
   */
  [[nodiscard]] const std::vector<SemanticNode>& nodes() const noexcept {
    return nodes_;
  }
  /**
   * @brief Returns the ordered function result signature.
   * @return Immutable borrowed output sequence owned by this IR.
   * @throws Nothing.
   * @note Every return terminator must satisfy this signature.
   */
  [[nodiscard]] const std::vector<IRReturnDeclaration>& outputs()
      const noexcept {
    return outputs_;
  }
  /**
   * @brief Returns the non-security semantic digest.
   * @return Immutable borrowed digest owned by this IR.
   * @throws Nothing.
   * @note The digest supports reproducibility and is not a trust identity.
   */
  [[nodiscard]] const IRDigest& digest() const noexcept { return digest_; }
  /**
   * @brief Returns canonical declaration metadata sorted by id.
   * @return Immutable vector owned by this stage, valid for its lifetime.
   * @throws Nothing.
   * @note Contains no runtime payload; concurrent immutable reads are safe.
   */
  [[nodiscard]] const std::vector<WorkflowInputDeclaration>&
  input_declarations() const noexcept {
    return input_declarations_;
  }
  /** @brief Frozen resources explicitly selected by operation preparation.
   * Owners are separate from per-run numeric inputs and survive this stage's
   * copies. Unused caller bindings and allocation addresses are not identity.
   */
  const ResourceSet& resources() const noexcept { return resources_; }
  /**
   * @brief Reports whether this stage remains valid for its owning context.
   * @return True for unchanged source context or a linked standalone artifact.
   * @throws Nothing.
   * @note A default placeholder is never current.
   */
  [[nodiscard]] bool current() const noexcept {
    return current_check_ && current_check_();
  }

  /** @brief Nonsemantic provenance of this version; excluded from digest. */
  const IRProvenance& provenance() const noexcept { return provenance_; }

  /** @brief Interpretation of blocks; dependency paths may repeat call IDs. */
  IRForm form() const noexcept { return form_; }
  /** @brief Ordered dependency paths or explicit control-flow blocks. */
  const std::vector<IRBlock>& blocks() const noexcept { return blocks_; }
  IRBlockId entry() const noexcept { return entry_; }
  const std::vector<IRInputBinding>& input_bindings() const noexcept {
    return input_bindings_;
  }
  /** @brief Owning editable copy for another frontend or transformation pass.
   */
  IRProgram program() const {
    return {input_declarations_,
            input_bindings_,
            nodes_,
            blocks_,
            entry_,
            outputs_,
            form_};
  }

 private:
  friend class Compiler;
  friend class CompilerArtifact;

  IRProvenance provenance_;
  IRForm form_ = IRForm::ControlFlow;
  std::vector<IRBlock> blocks_;
  IRBlockId entry_ = 0;
  std::vector<IRInputBinding> input_bindings_;
  /** @brief Immutable resources explicitly selected by operation preparation.
   */
  ResourceSet resources_;
  /** @brief Canonical copied input metadata, with no per-run sample owners. */
  std::vector<WorkflowInputDeclaration> input_declarations_;
  /** @brief Captured graph revision. */
  std::uint64_t revision_ = 0;
  /** @brief Prepared semantic calls; blocks define their execution order. */
  std::vector<SemanticNode> nodes_;
  /** @brief Named requested outputs. */
  std::vector<IRReturnDeclaration> outputs_;
  /** @brief Canonical non-security stage digest. */
  IRDigest digest_;
  /** @brief Runtime-only currentness predicate excluded from identity. */
  std::function<bool()> current_check_;
  /** @brief Runtime-only frozen operation-set identity excluded from digest. */
  std::weak_ptr<OperationRegistry> operation_registry_;
};

}  // namespace ps
