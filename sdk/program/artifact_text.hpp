#pragma once

#include <map>
#include <string>
#include <vector>

#include "photospider/compiler/artifact.hpp"

namespace ps::compiler_internal {
inline constexpr std::size_t kMaximumArtifactBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumIRNodes = 65536;
inline constexpr std::size_t kMaximumPhysicalRecords = kMaximumIRNodes * 64;

/** @brief Portable records; runtime-owned members of node/binding remain empty.
 */
struct IRRecord final {
  SemanticNode node;
  std::vector<ResourceIdentity> resources;
};
struct ArtifactData final {
  ArtifactManifest manifest;
  std::string parent_digest;
  IRProvenance provenance;
  std::vector<WorkflowInputDeclaration> declarations;
  std::vector<IRRecord> nodes;
  std::vector<IRReturnDeclaration> ir_outputs;
  std::vector<IRInputBinding> input_bindings;
  std::vector<IRBlock> blocks;
  IRBlockId entry = 0;
  IRForm form = IRForm::ControlFlow;
  physical::Program physical;
  std::vector<PlanCallBinding> bindings;
  std::map<std::string, std::size_t> plan_outputs;
  PlanningOptions options;
};
// Physical table transport for planner providers. It has no artifact envelope.
Result<std::string> encode_physical_tables(const physical::Program&,
                                           const std::vector<PlanCallBinding>&,
                                           uint64_t byte_limit);
Result<ArtifactData> decode_physical_tables(std::string_view, uint64_t extent,
                                            const physical::ValidationLimits&,
                                            uint64_t byte_limit);
uint64_t physical_transfer_limit(const physical::ValidationLimits&) noexcept;
Result<ArtifactData> decode_artifact(std::string_view text);
Result<std::string> encode_artifact(const ArtifactData& data);
Result<std::string> read_artifact_file(const std::string& path);
Status write_artifact_file(const std::string& path,
                           const Result<std::string>& text);
}  // namespace ps::compiler_internal
