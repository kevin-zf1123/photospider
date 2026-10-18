#pragma once

#include <map>
#include <string>
#include <vector>

#include "photospider/compiler/artifact.hpp"

namespace ps::compiler_internal {
inline constexpr std::size_t kMaximumArtifactBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumIRNodes = 65536;
inline constexpr std::size_t kMaximumPlanSteps = kMaximumIRNodes * 64;

/** @brief Portable records; runtime-owned members of node/step remain empty. */
struct IRRecord final {
  SemanticNode node;
  std::vector<ResourceIdentity> resources;
};
struct PlanRecord final {
  PlanStep step;
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
  std::map<IRBlockId, std::vector<PlanTileInstruction>> code;
  std::vector<PlanRecord> steps;
  std::map<std::string, std::size_t> plan_outputs;
  PlanningOptions options;
};
Result<ArtifactData> decode_artifact(std::string_view text);
Result<std::string> encode_artifact(const ArtifactData& data);
Result<std::string> read_artifact_file(const std::string& path);
Status write_artifact_file(const std::string& path,
                           const Result<std::string>& text);
}  // namespace ps::compiler_internal
