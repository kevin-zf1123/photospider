#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "photospider/compiler/compiler.hpp"

namespace ps {
/** @brief Independently loadable compiler stage stored by a text artifact. */
enum class ArtifactKind { SemanticIR, OptimizedIR, ExecutionPlan };

/** @brief External operation symbol and compiler-visible contract identity.
 * Includes registered traits and validator/report references. It neither
 * identifies nor authenticates native callback code. Hosts resolve symbols by
 * registering operations or loading libraries of their choosing.
 */
struct ArtifactOperation final {
  std::string key;
  std::string contract_digest;
};
/** @brief Dependencies inspectable before native operations are loaded.
 * Resource bytes and per-run inputs are supplied separately. No native
 * pointers, library paths, cached results or Runs enter a compiler artifact.
 */
struct ArtifactManifest final {
  ArtifactKind kind = ArtifactKind::SemanticIR;
  std::uint32_t format_version = 3;
  std::vector<ArtifactOperation> operations;
  std::vector<ResourceIdentity> resources;
};
/** @brief Exact IR stage declared by a .psir file. */
using ImportedIR = std::variant<SemanticGraphIR, OptimizedGraphIR>;

/** @brief Editable, versioned .psir and .psxp compiler artifacts.
 *
 * Format v3 .psir stores typed SSA calls and an explicit program form: raw
 * dependency paths or control flow. Optimized IR and .psxp require control
 * flow. Earlier artifact versions are rejected. .psxp stores ordered tile and
 * shared-stage instructions, read grants, publication targets, instruction
 * dependencies, backend/fallback choices and output selections. Import links
 * against the compiler's frozen registry, validates contracts/schemas/resources
 * and recreates pure static preparation. Plan import does not invoke the DAG
 * frontend, optimizer or physical planner.
 *
 * Imported stages own metadata/preparation, have local revision one, and are
 * independent of GraphContext lifetime. The registry must remain alive and
 * execution must use that registry. Concurrent immutable calls are safe.
 * Parse errors return InvalidArgument with line/column diagnostics; missing
 * symbols/resources return NotFound; changed contracts return TypeMismatch.
 * Allocation failure returns ResourceExhausted (diagnostics may allocate).
 * Text import performs no runtime callbacks, library loading or resource I/O.
 * File helpers only read/write the explicit path.
 */
class PHOTOSPIDER_API CompilerArtifact final {
 public:
  /** @brief Encode current IR, preserving parameter bits, into at most 64 MiB.
   * Syntax accepts semicolon line comments, escaped strings and decimal f64.
   * f64_bits preserves exact IEEE-754 bits. Digests are recomputed on import;
   * parent identity records provenance. Default/stale objects fail Stale.
   */
  static Result<std::string> export_ir(const SemanticGraphIR& ir);
  static Result<std::string> export_ir(const OptimizedGraphIR& ir);
  /** @brief Encode the complete static instruction program and control flow,
   * including explicit empty output selections.
   */
  static Result<std::string> export_plan(const ExecutionPlan& plan);
  /** @brief Parse syntax/dependencies without a registry or operation calls.
   * Full graph/plan validation occurs during import. Unknown versions,
   * duplicate records and text over 64 MiB are rejected.
   */
  static Result<ArtifactManifest> inspect(std::string_view text);
  /** @brief Link the declared IR stage directly, without reconstructing a DAG.
   * Resources match byte length/SHA-256 and are selected per operation. No
   * optimization occurs; an optimized artifact stays optimized.
   */
  static Result<ImportedIR> import_ir(const Compiler& compiler,
                                      std::string_view text,
                                      ResourceSet resources = {});
  /** @brief Restore declarations and instruction blocks without dependency
   * mapping, tile expansion or backend selection. Saved reads, publications,
   * dependencies and ready-before-use ordering are validated. Unsupported
   * executable choices fail BackendUnavailable. Grids and static preparation
   * are reconstructed; instruction order and declaration indices are retained.
   */
  static Result<ExecutionPlan> import_plan(const Compiler& compiler,
                                           std::string_view text,
                                           ResourceSet resources = {});
  /** @brief Encode completely, then replace the explicit destination file.
   * Encoding failure leaves the file untouched. I/O failure may leave partial
   * text and returns OperationFailed with Io/ShortIo detail.
   */
  static Status save_ir(const SemanticGraphIR& ir, const std::string& path);
  static Status save_ir(const OptimizedGraphIR& ir, const std::string& path);
  static Status save_plan(const ExecutionPlan& plan, const std::string& path);
  /** @brief Read bounded text then import; unreadable paths fail NotFound.
   * Extensions are conventional; magic/version inside the file select format.
   */
  static Result<ImportedIR> load_ir(const Compiler& compiler,
                                    const std::string& path,
                                    ResourceSet resources = {});
  /** @brief Read/import a plan, with the file/error rules of load_ir. */
  static Result<ExecutionPlan> load_plan(const Compiler& compiler,
                                         const std::string& path,
                                         ResourceSet resources = {});
};
}  // namespace ps
