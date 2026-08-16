#pragma once

#include <functional>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
#include "photospider/data/image_view.hpp"
#include "photospider/data/region.hpp"

namespace ps::ops {

/**
 * @brief Logical descriptor passed between dense-image inference and execute.
 *
 * @throws std::bad_alloc when copying the tensor shape allocates and fails.
 * @note Physical layout, payload bytes, graph state, device routing, and
 *       readiness are deliberately absent from inference.
 */
struct DenseImageDescriptor {
  /** @brief Complete logical DenseTensor descriptor. */
  DenseTensorDescriptor tensor;

  /** @brief Complete bounded ordinary-image interpretation of the tensor. */
  ImageFacet image;

  /**
   * @brief Compares every logical tensor and ordinary-image metadata fact.
   *
   * @param other Descriptor to compare.
   * @return True when tensor descriptor and ImageFacet both match.
   * @throws Nothing under DenseTensorDescriptor and ImageFacet equality.
   */
  bool operator==(const DenseImageDescriptor& other) const noexcept {
    return tensor == other.tensor && image == other.image;
  }
};

/**
 * @brief Immutable operation configuration visible to dense callbacks.
 *
 * @throws std::bad_alloc when copying the effective parameter tree allocates
 *         and fails.
 * @note This source-tree-private snapshot deliberately omits Node output,
 *       cache, topology, registry, graph, and device state, so inference
 *       cannot reach payload bytes through its configuration argument.
 */
struct CpuDenseImageConfiguration {
  /** @brief Deep-owned request-effective parameter snapshot. */
  plugin::ParameterMap parameters;

  /**
   * @brief Exact normalized logical work selection for this invocation.
   * @note Absence at higher layers is normalized to canonical Whole before
   *       constructing this immutable operation snapshot.
   */
  RegionSet region = RegionSet::whole();
};

/**
 * @brief Pure logical inference callback for one CPU dense-image operation.
 *
 * @param configuration Deep-owned request-effective parameter snapshot.
 * @param inputs Logical input descriptors in destination-slot order.
 * @return Exact logical output descriptor.
 * @throws Any exception emitted by the operation definition.
 * @note The callback type exposes no payload, view, graph, registry, device,
 *       or IO object, structurally preventing byte access during inference.
 */
using CpuDenseImageInferFunc = std::function<DenseImageDescriptor(
    const CpuDenseImageConfiguration& configuration,
    const std::vector<DenseImageDescriptor>& inputs)>;

/**
 * @brief Stride-aware execute callback for one CPU dense-image operation.
 *
 * @param configuration Deep-owned request-effective parameter snapshot.
 * @param inputs Retaining checked read-only image views.
 * @param inferred Exact logical output descriptor returned by inference.
 * @return Independently owned immutable output Value.
 * @throws Any exception emitted by the operation definition.
 * @note The callback receives no ImageBuffer, registry, graph, provider, or
 *       mutable payload authority.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using CpuDenseImageExecuteFunc =
    std::function<Value(const CpuDenseImageConfiguration& configuration,
                        const std::vector<ImageView>& inputs,
                        const DenseImageDescriptor& inferred)>;
// NOLINTEND

/**
 * @brief Private production definition for a Value-backed CPU image operation.
 *
 * @throws std::bad_alloc or callback-defined copy exceptions when copied.
 * @note This type is source-tree private and does not modify operation plugin
 *       ABI v2 or create a second registry callback slot.
 */
struct CpuDenseImageOperation {
  /** @brief Pure descriptor-only output inference callback. */
  CpuDenseImageInferFunc infer;

  /** @brief Checked stride-aware immutable Value execution callback. */
  CpuDenseImageExecuteFunc execute;
};

/**
 * @brief Runs a private dense operation behind the current ImageBuffer edge.
 *
 * The runner reuses each sealed NodeOutput image Value when present and
 * otherwise snapshots its CPU ImageBuffer, invokes descriptor-only inference,
 * executes through checked ImageViews, validates the complete result against
 * inference, and publishes that exact sealed Value plus an independent current
 * NodeOutput ImageBuffer compatibility snapshot.
 *
 * @param node Node whose request-effective parameters are copied into the
 *        configuration passed to both callbacks.
 * @param inputs Current graph/cache image inputs in destination-slot order.
 * @param operation Dense operation definition to run.
 * @param region Exact normalized logical work selection; Whole preserves the
 *        previous full-output behavior.
 * @return Newly owned validated current-boundary NodeOutput.
 * @throws GraphError with InvalidParameter for malformed caller inputs or
 *         inference rejection.
 * @throws GraphError with ComputeError for an invalid operation definition or
 *         inferred descriptor, execute failure, mismatched result, or
 *         unadaptable result.
 * @throws std::bad_alloc unchanged for resource exhaustion in any phase.
 * @note Legacy input snapshots share no writable ImageBuffer owner. Sealed
 * Value inputs preserve allocation/revision identity without a second copy.
 * No partially validated output is published.
 */
NodeOutput execute_cpu_dense_image_operation(
    const Node& node, const std::vector<const NodeOutput*>& inputs,
    const CpuDenseImageOperation& operation,
    const RegionSet& region = RegionSet::whole());

/**
 * @brief Creates the dependency-neutral unsigned-8 dense invert definition.
 *
 * @return Reentrant operation whose inference preserves the exact logical
 *         descriptor and whose execute computes 255 minus every active channel.
 * @throws std::bad_alloc if std::function target construction allocates.
 * @note The returned callbacks use only copied effective parameters, logical
 *       descriptors, checked ImageViews, and immutable Values.
 */
CpuDenseImageOperation make_dense_invert_operation();

}  // namespace ps::ops
