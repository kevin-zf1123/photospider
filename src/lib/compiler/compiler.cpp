#include "photospider/compiler/compiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "execution/native_gpu.hpp"
#include "plugin/operation_identity.hpp"

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

/**
 * @brief Appends one rank-general logical Region to stage identity.
 * @param digest Destination builder.
 * @param region Valid bounded logical Region.
 * @throws Nothing.
 * @note Logical offsets/extents are independent from storage layout.
 */
void append_region(DigestBuilder* digest, const Region& region) noexcept {
  digest->integer(region.rank());
  for (const RegionDimension& dimension : region.dimensions()) {
    digest->integer(dimension.offset);
    digest->integer(dimension.extent);
  }
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

/** @brief Encodes canonical declaration metadata without runtime payload. */
void append_declarations(
    DigestBuilder* digest,
    const std::vector<WorkflowInputDeclaration>& declarations) noexcept {
  digest->integer(declarations.size());
  for (const auto& declaration : declarations) {
    digest->integer(declaration.id);
    digest->text(declaration.name);
    append_descriptor(digest, declaration.descriptor);
    append_region(digest, declaration.region);
    digest->integer(declaration.layout.byte_offset);
    digest->integer(declaration.layout.byte_strides.size());
    for (auto stride : declaration.layout.byte_strides)
      digest->integer(static_cast<std::uint64_t>(stride));
    digest->integer(declaration.facets.size());
    for (const auto& facet : declaration.facets) {
      digest->text(facet.key);
      digest->integer(facet.version);
      digest->integer(facet.payload.size());
      digest->bytes(facet.payload.data(), facet.payload.size());
    }
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
  digest.text("semantic-graph-ir-v9");
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
    digest.integer(node.outputs.size());
    for (const auto& output : node.outputs) {
      digest.text(output.key);
      digest.integer(output.effective_atomic);
      append_descriptor(&digest, output.descriptor);
      contract_internal::append_facets(&digest, output.facets);
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
    const std::map<std::string, Region>& output_regions,
    std::uint64_t tile_height, std::uint64_t tile_width,
    ExecutionMode execution_mode, const std::vector<PhysicalStep>& physical) {
  DigestBuilder digest;
  digest.text("physical-plan-v9");
  digest.integer(static_cast<std::uint32_t>(execution_mode));
  digest.integer(physical.size());
  for (const auto& access : physical) {
    digest.integer(static_cast<std::uint32_t>(access.kind));
    digest.integer(access.step_index);
    digest.integer(access.input_index);
    digest.integer(static_cast<std::uint32_t>(access.source_backend));
    digest.integer(static_cast<std::uint32_t>(access.destination_backend));
    append_descriptor(&digest, access.descriptor);
    append_region(&digest, access.region);
    digest.integer(access.packed_bytes);
    digest.integer(access.allocation_bytes);
    digest.text(access.output_name);
    digest.integer(access.packed_layout.byte_offset);
    digest.integer(access.packed_layout.byte_strides.size());
    for (auto stride : access.packed_layout.byte_strides)
      digest.integer(static_cast<std::uint64_t>(stride));
    for (auto origin : access.packed_layout.origin)
      digest.integer(origin);
  }
  digest.integer(tile_height);
  digest.integer(tile_width);
  digest.integer(output_regions.size());
  for (const auto& requested : output_regions) {
    digest.text(requested.first);
    append_region(&digest, requested.second);
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
    digest.integer(step.planned_bytes);
    contract_internal::append_traits(&digest, step.traits);
    digest.integer(step.effective_atomic);
    append_descriptor(&digest, step.output_descriptor);
    contract_internal::append_facets(&digest, step.output_facets);
    append_region(&digest, step.output_demand);
    digest.integer(step.input_demands.size());
    for (const Region& demand : step.input_demands) {
      append_region(&digest, demand);
    }
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

/** @brief Builds explicit native access steps from validated logical producers.
 */
Result<std::vector<PhysicalStep>> native_access_plan(
    std::vector<PlanStep>* steps,
    const std::vector<WorkflowInputDeclaration>& declarations,
    const std::map<std::string, std::size_t>& outputs,
    const std::map<std::string, Region>& output_regions) {
  std::vector<PhysicalStep> result;
  std::uint64_t complete = 0;
  for (std::size_t i = 0; i < steps->size(); ++i) {
    auto& step = (*steps)[i];
    if (step.backend == Backend::Gpu) {
      // Every native allocation rounds by less than twice its payload. This
      // also bounds any split of declared scratch into multiple allocations.
      if (step.planned_bytes > static_cast<std::uint64_t>(INT64_MAX) / 2)
        return Result<std::vector<PhysicalStep>>(
            Status::failure(ErrorCode::ResourceExhausted,
                            "native workspace capacity overflows"));
      step.planned_bytes *= 2;
    }
    if (step.planned_bytes > UINT64_MAX - complete)
      return Result<std::vector<PhysicalStep>>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "native complete working set overflows"));
    complete += step.planned_bytes;
    for (std::size_t port = 0; port < step.inputs.size(); ++port) {
      const auto& input = step.inputs[port];
      const auto* producer = std::get_if<PlanStepInput>(&input);
      const auto backend =
          producer ? (*steps)[producer->step_index].backend : Backend::Cpu;
      if (backend == step.backend)
        continue;
      const auto& descriptor =
          producer ? (*steps)[producer->step_index].output_descriptor
                   : declarations[std::get<PlanWorkflowInput>(input)
                                      .declaration_index]
                         .descriptor;
      auto count = step.input_demands[port].element_count();
      const auto width = Value::element_size(descriptor.element_type);
      if (!count.ok() || count.value() > UINT64_MAX / width)
        return Result<std::vector<PhysicalStep>>(Status::failure(
            ErrorCode::ResourceExhausted, "native transfer size overflows"));
      const auto bytes = count.value() * width;
      const auto capacity = step.backend == Backend::Gpu
                                ? gpu_internal::allocation_capacity(bytes)
                                : 0;
      if (step.backend == Backend::Gpu && !capacity)
        return Result<std::vector<PhysicalStep>>(
            Status::failure(ErrorCode::ResourceExhausted,
                            "native transfer is not addressable"));
      result.push_back({step.backend == Backend::Gpu
                            ? PhysicalStepKind::Upload
                            : PhysicalStepKind::HostAccess,
                        i,
                        port,
                        input,
                        backend,
                        step.backend,
                        descriptor,
                        step.input_demands[port],
                        bytes,
                        capacity,
                        {}});
    }
    result.push_back({PhysicalStepKind::Operation,
                      i,
                      0,
                      PlanStepInput{i},
                      step.backend,
                      step.backend,
                      step.output_descriptor,
                      step.output_demand,
                      0,
                      step.planned_bytes,
                      {}});
  }
  for (const auto& output : outputs) {
    const auto& step = (*steps)[output.second];
    if (step.backend == Backend::Gpu)
      result.push_back({PhysicalStepKind::HostAccess, output.second, 0,
                        PlanStepInput{output.second}, Backend::Gpu,
                        Backend::Cpu, step.output_descriptor,
                        output_regions.at(output.first), 0, 0, output.first});
  }
  for (auto& action : result) {
    auto packed = action.descriptor;
    for (std::size_t axis = 0; axis < packed.shape.size(); ++axis)
      packed.shape[axis] = action.region.dimensions()[axis].extent;
    auto dense = input_internal::dense_metadata(packed);
    if (!dense.ok()) {
      if (action.destination_backend == Backend::Gpu)
        return Result<std::vector<PhysicalStep>>(dense.status());
      continue;
    }
    action.packed_bytes = dense.value().bytes;
    action.packed_layout = dense.value().layout;
    for (auto d : action.region.dimensions())
      action.packed_layout.origin.push_back(d.offset);
  }
  return Result<std::vector<PhysicalStep>>(std::move(result));
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
  digest.text("plan-cache-key-v9");
  digest.text(plan);
  return digest.finish();
}

/**
 * @brief Merges two valid demands into their conservative bounding Region.
 * @param left First logical demand.
 * @param right Second logical demand.
 * @param shape Common descriptor shape.
 * @return Bounding Region or a typed containment/rank failure.
 * @throws std::bad_alloc If result or diagnostic allocation fails.
 * @note The result may contain extra coordinates but never escapes `shape`.
 */
Result<Region> merge_regions(const Region& left, const Region& right,
                             const std::vector<std::uint64_t>& shape) {
  const Status left_status = left.validate(shape);
  const Status right_status = right.validate(shape);
  if (!left_status.ok()) {
    return Result<Region>(left_status);
  }
  if (!right_status.ok()) {
    return Result<Region>(right_status);
  }
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(shape.size());
  for (std::size_t axis = 0U; axis < shape.size(); ++axis) {
    const RegionDimension& left_axis = left.dimensions()[axis];
    const RegionDimension& right_axis = right.dimensions()[axis];
    const std::uint64_t start = std::min(left_axis.offset, right_axis.offset);
    const std::uint64_t left_end = left_axis.offset + left_axis.extent;
    const std::uint64_t right_end = right_axis.offset + right_axis.extent;
    const std::uint64_t end = std::max(left_end, right_end);
    dimensions.push_back(RegionDimension{start, end - start});
  }
  return Result<Region>(Region(std::move(dimensions)));
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

bool ExecutionPlan::dependency_network() const noexcept {
  if (dependency_protocol_)
    return true;
  for (const auto& step : steps_)
    if (step.traits.outputs[0].dependency_version || !step.effective_atomic)
      return true;
  return false;
}

Result<ExecutionPlan> ExecutionPlan::tile_plan(const std::string& name,
                                               const Region& region) const {
  if (!current() || operation_registry_.expired())
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::Stale, "tile parent is stale"));
  const auto named = outputs_.find(name);
  if (named == outputs_.end() || region.empty() ||
      !region.validate(steps_[named->second].output_descriptor.shape).ok())
    return Result<ExecutionPlan>(Status::failure(ErrorCode::InvalidArgument,
                                                 "invalid named tile Region"));
  const auto& requested = output_regions_.at(name);
  for (std::size_t axis = 0; axis < region.rank(); ++axis) {
    const auto part = region.dimensions()[axis],
               allowed = requested.dimensions()[axis];
    if (part.offset < allowed.offset ||
        part.offset + part.extent > allowed.offset + allowed.extent)
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::InvalidArgument, "tile exceeds requested output"));
  }
  if (!input_internal::complete_image_channels(
          steps_[named->second].output_descriptor,
          steps_[named->second].output_facets, region))
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::InvalidArgument, "tile must contain all image channels"));
  ExecutionPlan tile = *this;
  if (dependency_network()) {
    tile.outputs_ = {{name, named->second}};
    tile.output_regions_ = {{name, region}};
    tile.steps_[named->second].output_demand = region;
    tile.physical_steps_.clear();
    tile.digest_.value = physical_digest(
        tile.optimized_digest_.value, tile.steps_, tile.outputs_,
        tile.input_declarations_, tile.output_regions_, tile.tile_height_,
        tile.tile_width_, tile.execution_mode_, tile.physical_steps_);
    tile.cache_key_.value = plan_cache_key(tile.digest_.value);
    if (!current())
      return Result<ExecutionPlan>(
          Status::failure(ErrorCode::Stale, "dependency template changed"));
    return Result<ExecutionPlan>(std::move(tile));
  }
  std::vector<std::optional<Region>> demands(steps_.size());
  demands[named->second] = region;
  for (std::size_t reverse = steps_.size(); reverse > 0; --reverse) {
    const auto i = reverse - 1;
    if (!demands[i])
      continue;
    auto& step = tile.steps_[i];
    if (!input_internal::complete_image_channels(
            step.output_descriptor, step.output_facets, *demands[i]))
      return Result<ExecutionPlan>(
          Status::failure(ErrorCode::InvalidArgument,
                          "propagated tile demand omits image channels"));
    step.output_demand = step.whole_boundary
                             ? Region::whole(step.output_descriptor.shape)
                             : *demands[i];
    step.input_demands.clear();
    for (std::size_t port = 0; port < step.inputs.size(); ++port) {
      const auto* producer = std::get_if<PlanStepInput>(&step.inputs[port]);
      const auto& descriptor =
          producer ? steps_[producer->step_index].output_descriptor
                   : input_declarations_[std::get<PlanWorkflowInput>(
                                             step.inputs[port])
                                             .declaration_index]
                         .descriptor;
      auto demand = input_internal::derive_input_demand(
          step.traits, step.output_demand, step.output_descriptor.shape,
          descriptor.shape, step.traits.input_schema[port].kind);
      if (!demand.ok())
        return Result<ExecutionPlan>(demand.status());
      step.input_demands.push_back(demand.value());
      if (producer) {
        auto& prior = demands[producer->step_index];
        if (prior) {
          auto merged = merge_regions(*prior, demand.value(), descriptor.shape);
          if (!merged.ok())
            return Result<ExecutionPlan>(merged.status());
          prior = merged.take_value();
        } else {
          prior = demand.take_value();
        }
      }
    }
    ValueDescriptor packed = step.output_descriptor;
    for (std::size_t axis = 0; axis < packed.shape.size(); ++axis)
      packed.shape[axis] = step.output_demand.dimensions()[axis].extent;
    auto dense = input_internal::dense_metadata(packed);
    if (!dense.ok() && step.traits.outputs[0].output_schema.kind ==
                           OperationPortKind::RgbaFloat32)
      return Result<ExecutionPlan>(dense.status());
    step.planned_bytes =
        std::max(step.traits.estimated_bytes,
                 dense.ok() ? dense.value().bytes
                            : static_cast<std::uint64_t>(
                                  Value::element_size(packed.element_type)));
    std::uint64_t workspace = step.traits.workspace_bytes;
    for (std::size_t port = 0; port < step.inputs.size() &&
                               step.traits.workspace_input_multiplier != 0;
         ++port) {
      const auto* producer = std::get_if<PlanStepInput>(&step.inputs[port]);
      const auto type =
          producer ? steps_[producer->step_index].output_descriptor.element_type
                   : input_declarations_[std::get<PlanWorkflowInput>(
                                             step.inputs[port])
                                             .declaration_index]
                         .descriptor.element_type;
      auto count = step.input_demands[port].element_count();
      const auto factor =
          Value::element_size(type) * step.traits.workspace_input_multiplier;
      if (!count.ok() || count.value() > (UINT64_MAX - workspace) / factor)
        return Result<ExecutionPlan>(Status::failure(
            ErrorCode::ResourceExhausted, "tile workspace overflows"));
      workspace += count.value() * factor;
    }
    if (workspace > UINT64_MAX - step.planned_bytes)
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::ResourceExhausted, "tile output/workspace overflows"));
    step.planned_bytes += workspace;
  }
  std::vector<PlanStep> pruned;
  std::vector<std::size_t> mapping(steps_.size());
  for (std::size_t i = 0; i < steps_.size(); ++i) {
    if (!demands[i])
      continue;
    mapping[i] = pruned.size();
    auto step = std::move(tile.steps_[i]);
    for (auto& input : step.inputs)
      if (auto* producer = std::get_if<PlanStepInput>(&input))
        producer->step_index = mapping[producer->step_index];
    pruned.push_back(std::move(step));
  }
  tile.steps_ = std::move(pruned);
  tile.outputs_ = {{name, mapping[named->second]}};
  tile.output_regions_ = {{name, region}};
  auto access = native_access_plan(&tile.steps_, tile.input_declarations_,
                                   tile.outputs_, tile.output_regions_);
  if (!access.ok())
    return Result<ExecutionPlan>(access.status());
  tile.physical_steps_ = access.take_value();
  tile.digest_.value = physical_digest(
      tile.optimized_digest_.value, tile.steps_, tile.outputs_,
      tile.input_declarations_, tile.output_regions_, tile.tile_height_,
      tile.tile_width_, tile.execution_mode_, tile.physical_steps_);
  tile.cache_key_.value = plan_cache_key(tile.digest_.value);
  if (!current())
    return Result<ExecutionPlan>(
        Status::failure(ErrorCode::Stale, "graph changed while deriving tile"));
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
Result<SemanticGraphIR> Compiler::analyze(const GraphSnapshot& snapshot) const {
  if (!snapshot.current()) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::Stale, "graph snapshot is stale"));
  }
  const WorkflowDocument& document = snapshot.document();
  if (document.schema_version != 2U || document.nodes.empty() ||
      document.nodes.size() > 65536U || document.outputs.empty() ||
      document.outputs.size() > 4096U || document.inputs.size() > 4096U) {
    return Result<SemanticGraphIR>(
        Status::failure(ErrorCode::InvalidArgument,
                        "WorkflowDocument version/count bounds are invalid"));
  }

  input_internal::Float32Environment float_environment;
  if (!float_environment.active())
    return Result<SemanticGraphIR>(Status::failure(
        ErrorCode::OperationFailed, "cannot set binary32 environment"));
  auto declarations = document.inputs;
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
  std::map<ValueRef, ObservationKind> local_observations;
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
      local_observations.emplace(ref, output.observation_kind);
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
        if (local_observations.at(
                result_ports.at({source->source_node, source->source_port})) ==
            ObservationKind::RequestRecord)
          return Result<SemanticGraphIR>(Status::failure(
              ErrorCode::InvalidArgument,
              "RequestRecord output cannot feed any DAG consumer"));
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
  std::map<ValueRef, std::vector<ValueFacet>> output_facets;
  std::map<ValueRef, bool> effective_atomic;
  std::map<std::uint64_t, std::pair<float, float>> scalar_intervals;
  semantic.nodes_.reserve(document.nodes.size());
  std::map<ValueRef, ValueDescriptor> output_by_value;
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
    bool ancestors_atomic = true;
    node.inputs.reserve(source.inputs.size());
    std::vector<OperationMetadata> input_descriptors;
    input_descriptors.reserve(source.inputs.size());
    for (std::size_t position = 0; position < source.inputs.size();
         ++position) {
      const auto& input = source.inputs[position];
      const auto& port = node.traits.input_schema[position];
      node.inputs.push_back(input);
      ValueDescriptor descriptor;
      std::vector<ValueFacet> facets;
      if (const auto* producer = std::get_if<WorkflowNodeOutput>(&input)) {
        const auto ref =
            result_ports.at({producer->source_node, producer->source_port});
        if (!effective_atomic.at(ref))
          return Result<SemanticGraphIR>(Status::failure(
              ErrorCode::InvalidArgument,
              "consumer requires EffectiveAtomic input ancestry"));
        ancestors_atomic = ancestors_atomic && effective_atomic.at(ref);
        descriptor = output_by_value.at(ref);
        facets = output_facets.at(ref);
      } else {
        const auto id = std::get<WorkflowInputReference>(input).input_id;
        const auto& declaration = declarations[declaration_by_id.at(id)];
        descriptor = declaration.descriptor;
        facets = declaration.facets;
        if (port.kind == OperationPortKind::Float32Scalar) {
          auto inserted = scalar_intervals.emplace(
              id, std::make_pair(port.minimum, port.maximum));
          auto& interval = inserted.first->second;
          interval.first = std::max(interval.first, port.minimum);
          interval.second = std::min(interval.second, port.maximum);
          if (interval.first > interval.second) {
            return Result<SemanticGraphIR>(Status::failure(
                ErrorCode::InvalidArgument,
                "scalar consumer intervals have empty intersection"));
          }
        }
      }
      const auto status =
          input_internal::validate_port_metadata(port, descriptor, facets);
      if (!status.ok())
        return Result<SemanticGraphIR>(status);
      input_descriptors.push_back({std::move(descriptor), std::move(facets)});
    }
    auto output = infer_operation_outputs(node.traits, input_descriptors,
                                          node.parameters);
    if (!output.ok()) {
      return Result<SemanticGraphIR>(output.status());
    }
    if (node.traits.outputs[0].dependency_version) {
      const auto validated = operations_->validate_dependency_metadata(
          node.operation, input_descriptors, node.parameters);
      if (!validated.ok())
        return Result<SemanticGraphIR>(validated);
    }
    for (std::uint32_t oi = 0; oi < node.traits.outputs.size(); ++oi) {
      const auto& contract = node.traits.outputs[oi];
      const auto& metadata = output.value()[oi];
      auto selected = select_operation_output(node.traits, oi).take_value();
      for (std::size_t i = 0;
           i < input_descriptors.size() && !contract.dependency_version; ++i) {
        auto demand = input_internal::derive_input_demand(
            selected, Region::whole(metadata.descriptor.shape),
            metadata.descriptor.shape, input_descriptors[i].descriptor.shape,
            selected.input_schema[i].kind);
        if (!demand.ok())
          return Result<SemanticGraphIR>(demand.status());
      }
      const bool atomic =
          contract.observation_kind == ObservationKind::Atomic &&
          ancestors_atomic;
      const ValueRef ref{node.id, oi};
      effective_atomic.emplace(ref, atomic);
      output_facets.emplace(ref, metadata.facets);
      output_by_value.emplace(ref, metadata.descriptor);
      node.outputs.push_back(
          {contract.key, metadata.descriptor, metadata.facets, atomic});
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
      options.execution_mode != ExecutionMode::MetalFp32)
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::InvalidArgument, "unknown execution numeric mode"));
  if (options.tile_height == 0 || options.tile_width == 0)
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::InvalidArgument, "tile extents must be positive"));
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
  // Keep only output-reachable pure results. Side-effecting singleton roots
  // remain explicit even when no named result consumes them.
  std::set<ValueRef> needed;
  for (const auto& output : optimized.outputs())
    needed.insert(result_ports.at({output.node_id, output.port}));
  for (auto it = optimized.nodes().rbegin(); it != optimized.nodes().rend();
       ++it) {
    const auto& node = *it;
    if (!node.traits.side_effect_free)
      needed.insert({node.id, 0});
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
  // Pruning result tasks must not change the established Whole streaming
  // behavior of a graph compiled for the staged execution family.
  for (const auto& node : optimized.nodes())
    for (const auto& output : node.traits.outputs)
      plan.dependency_protocol_ =
          plan.dependency_protocol_ || output.dependency_version != 0;

  plan.tile_height_ = options.tile_height;
  plan.tile_width_ = options.tile_width;
  plan.revision_ = optimized.revision();
  plan.input_declarations_ = optimized.input_declarations();
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
      step.traits = select_operation_output(node.traits, oi).take_value();
      step.effective_atomic = output.effective_atomic;
      step.whole_boundary =
          step.traits.outputs[0].region_rule == OperationRegionRule::Whole ||
          !node.traits.deterministic || !node.traits.side_effect_free;
      step.output_descriptor = output.descriptor;
      step.output_facets = output.facets;
      step.backend = options.execution_mode == ExecutionMode::MetalFp32 &&
                             node.traits.supports_gpu
                         ? Backend::Gpu
                         : Backend::Cpu;
      if (step.backend == Backend::Cpu && !node.traits.supports_cpu) {
        return Result<ExecutionPlan>(
            Status::failure(ErrorCode::BackendUnavailable,
                            "operation has no required CPU implementation"));
      }
      auto dense_output =
          input_internal::dense_metadata(step.output_descriptor);
      if (!dense_output.ok() && step.traits.outputs[0].output_schema.kind ==
                                    OperationPortKind::RgbaFloat32)
        return Result<ExecutionPlan>(dense_output.status());
      step.planned_bytes = std::max(
          node.traits.estimated_bytes,
          dense_output.ok() ? dense_output.value().bytes
                            : static_cast<std::uint64_t>(Value::element_size(
                                  step.output_descriptor.element_type)));
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
  if (options.output_regions.size() > plan.outputs_.size()) {
    return Result<ExecutionPlan>(Status::failure(
        ErrorCode::InvalidArgument,
        "planning options contain too many named output Regions"));
  }
  if (plan.dependency_network()) {
    for (const auto& requested : options.output_regions)
      if (!plan.outputs_.count(requested.first))
        return Result<ExecutionPlan>(Status::failure(
            ErrorCode::InvalidArgument, "unknown dependency output"));
    for (auto& step : plan.steps_) {
      step.output_demand = Region::whole(step.output_descriptor.shape);
      step.input_demands.clear();
      step.planned_bytes =
          0;  // A template has no resolved live-set reservation.
    }
    for (const auto& output : plan.outputs_) {
      auto& step = plan.steps_[output.second];
      const auto requested = options.output_regions.find(output.first);
      const auto region = requested == options.output_regions.end()
                              ? Region::whole(step.output_descriptor.shape)
                              : requested->second;
      if (region.empty() ||
          !region.validate(step.output_descriptor.shape).ok() ||
          !input_internal::complete_image_channels(step.output_descriptor,
                                                   step.output_facets, region))
        return Result<ExecutionPlan>(Status::failure(
            ErrorCode::InvalidArgument, "invalid dependency output region"));
      plan.output_regions_.emplace(output.first, region);
      step.output_demand = region;
    }
    plan.optimized_digest_ = optimized.digest();
    plan.digest_.value = physical_digest(
        plan.optimized_digest_.value, plan.steps_, plan.outputs_,
        plan.input_declarations_, plan.output_regions_, plan.tile_height_,
        plan.tile_width_, plan.execution_mode_, plan.physical_steps_);
    plan.cache_key_.value = plan_cache_key(plan.digest_.value);
    plan.current_check_ = optimized.current_check_;
    plan.operation_registry_ = operations_;
    if (!plan.current())
      return Result<ExecutionPlan>(
          Status::failure(ErrorCode::Stale, "dependency template changed"));
    return Result<ExecutionPlan>(std::move(plan));
  }
  std::vector<std::optional<Region>> demand_by_step(plan.steps_.size());
  for (const auto& requested : options.output_regions) {
    if (plan.outputs_.count(requested.first) == 0U) {
      return Result<ExecutionPlan>(
          Status::failure(ErrorCode::InvalidArgument,
                          "planning options name an unknown workflow output"));
    }
  }
  for (const auto& output : plan.outputs_) {
    const PlanStep& step = plan.steps_[output.second];
    Region demand = Region::whole(step.output_descriptor.shape);
    const auto requested = options.output_regions.find(output.first);
    if (requested != options.output_regions.end()) {
      const Status status =
          requested->second.validate(step.output_descriptor.shape);
      if (!status.ok() || requested->second.empty()) {
        return Result<ExecutionPlan>(Status::failure(
            ErrorCode::InvalidArgument,
            "planned workflow output Region is empty or out of bounds"));
      }
      demand = requested->second;
      if (!input_internal::complete_image_channels(
              step.output_descriptor, step.output_facets, demand)) {
        return Result<ExecutionPlan>(
            Status::failure(ErrorCode::InvalidArgument,
                            "image demand must include all channels"));
      }
    }
    if (demand_by_step[output.second].has_value()) {
      auto merged = merge_regions(demand_by_step[output.second].value(), demand,
                                  step.output_descriptor.shape);
      if (!merged.ok()) {
        return Result<ExecutionPlan>(merged.status());
      }
      demand_by_step[output.second] = merged.take_value();
    } else {
      demand_by_step[output.second] = std::move(demand);
    }
  }
  for (std::size_t reverse = plan.steps_.size(); reverse > 0U; --reverse) {
    const std::size_t step_index = reverse - 1U;
    PlanStep& step = plan.steps_[step_index];
    if (!demand_by_step[step_index].has_value()) {
      demand_by_step[step_index] = Region::whole(step.output_descriptor.shape);
    }
    step.output_demand = demand_by_step[step_index].value();
    if (!input_internal::complete_image_channels(
            step.output_descriptor, step.output_facets, step.output_demand)) {
      return Result<ExecutionPlan>(
          Status::failure(ErrorCode::InvalidArgument,
                          "propagated image demand must include all channels"));
    }
    step.input_demands.reserve(step.inputs.size());
    for (std::size_t input_position = 0U; input_position < step.inputs.size();
         ++input_position) {
      const auto& input = step.inputs[input_position];
      const auto* producer = std::get_if<PlanStepInput>(&input);
      const auto& descriptor =
          producer ? plan.steps_[producer->step_index].output_descriptor
                   : plan.input_declarations_[std::get<PlanWorkflowInput>(input)
                                                  .declaration_index]
                         .descriptor;
      auto input_demand = input_internal::derive_input_demand(
          step.traits, step.output_demand, step.output_descriptor.shape,
          descriptor.shape, step.traits.input_schema[input_position].kind);
      if (!input_demand.ok())
        return Result<ExecutionPlan>(input_demand.status());
      step.input_demands.push_back(input_demand.value());
      if (!producer)
        continue;
      const std::size_t producer_index = producer->step_index;
      if (demand_by_step[producer_index].has_value()) {
        auto merged = merge_regions(demand_by_step[producer_index].value(),
                                    input_demand.value(), descriptor.shape);
        if (!merged.ok()) {
          return Result<ExecutionPlan>(merged.status());
        }
        demand_by_step[producer_index] = merged.take_value();
      } else {
        demand_by_step[producer_index] = input_demand.take_value();
      }
    }
  }
  std::uint64_t complete_working_set = 0;
  for (auto& step : plan.steps_) {
    ValueDescriptor packed = step.output_descriptor;
    const auto coverage =
        step.whole_boundary ? Region::whole(packed.shape) : step.output_demand;
    for (std::size_t axis = 0; axis < packed.shape.size(); ++axis)
      packed.shape[axis] = coverage.dimensions()[axis].extent;
    auto dense = input_internal::dense_metadata(packed);
    if (!dense.ok() && step.traits.outputs[0].output_schema.kind ==
                           OperationPortKind::RgbaFloat32)
      return Result<ExecutionPlan>(dense.status());
    step.planned_bytes =
        std::max(step.traits.estimated_bytes,
                 dense.ok() ? dense.value().bytes
                            : static_cast<std::uint64_t>(
                                  Value::element_size(packed.element_type)));
    std::uint64_t workspace = step.traits.workspace_bytes;
    for (std::size_t i = 0; i < step.inputs.size(); ++i) {
      if (step.traits.workspace_input_multiplier == 0)
        break;
      const auto* producer = std::get_if<PlanStepInput>(&step.inputs[i]);
      const auto& descriptor =
          producer ? plan.steps_[producer->step_index].output_descriptor
                   : plan.input_declarations_[std::get<PlanWorkflowInput>(
                                                  step.inputs[i])
                                                  .declaration_index]
                         .descriptor;
      auto count = step.input_demands[i].element_count();
      const auto width = Value::element_size(descriptor.element_type);
      const auto factor = width * step.traits.workspace_input_multiplier;
      if (!count.ok() || count.value() > (UINT64_MAX - workspace) / factor)
        return Result<ExecutionPlan>(Status::failure(
            ErrorCode::ResourceExhausted, "workspace byte bound overflows"));
      workspace += count.value() * factor;
    }
    if (workspace > UINT64_MAX - step.planned_bytes)
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::ResourceExhausted, "step working set overflows"));
    step.planned_bytes += workspace;
    if (step.planned_bytes > UINT64_MAX - complete_working_set)
      return Result<ExecutionPlan>(Status::failure(
          ErrorCode::ResourceExhausted, "complete working set overflows"));
    complete_working_set += step.planned_bytes;
  }
  for (const auto& output : plan.outputs_) {
    const auto requested = options.output_regions.find(output.first);
    plan.output_regions_.emplace(
        output.first,
        requested == options.output_regions.end()
            ? Region::whole(plan.steps_[output.second].output_descriptor.shape)
            : requested->second);
  }
  auto access = native_access_plan(&plan.steps_, plan.input_declarations_,
                                   plan.outputs_, plan.output_regions_);
  if (!access.ok())
    return Result<ExecutionPlan>(access.status());
  plan.physical_steps_ = access.take_value();
  plan.optimized_digest_ = optimized.digest();
  plan.digest_.value = physical_digest(
      plan.optimized_digest_.value, plan.steps_, plan.outputs_,
      plan.input_declarations_, plan.output_regions_, plan.tile_height_,
      plan.tile_width_, plan.execution_mode_, plan.physical_steps_);
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
