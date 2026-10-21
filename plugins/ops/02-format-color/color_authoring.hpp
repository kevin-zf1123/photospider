#include <memory>
#pragma once

#include <map>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/format/rgb_basis.hpp"
#include "photospider/ops/tensor_description.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::basis_ops {
// These wrappers only inspect structural metadata. Their private preparations
// have known provenance even when the caller supplies a custom registry.
Result<OperationMetadata> workflow_metadata(const WorkflowDocument& document,
                                            const WorkflowInput& input,
                                            const OperationRegistry& registry,
                                            const ResourceSet& resources = {});
Result<TensorDescription> output_description(
    const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters, unsigned member);
}  // namespace ps::plugin_internal::basis_ops

namespace ps::plugin_internal::basis_ops {
Result<WorkflowNodeOutput> author_view_conversion(
    WorkflowDocument&, WorkflowInput, const format::RgbBasisOptions&,
    std::shared_ptr<OperationRegistry>, const ResourceSet&);
}
namespace ps::plugin_internal::ocio_ops {
// Build the known implementation, independently of caller registry state.
// Returns the effective endpoint assertion after checking canonical routing.
Result<std::string> output_endpoint(
    const OperationMetadata&, const std::map<std::string, ParameterValue>&,
    unsigned, const ResourceSet&);
}  // namespace ps::plugin_internal::ocio_ops

namespace ps::plugin_internal::icc_ops {
Result<std::string> construction_trace(
    const OperationMetadata&, const std::map<std::string, ParameterValue>&,
    unsigned, const ResourceSet&);
}

namespace ps::plugin_internal::ocio_ops {
Result<std::string> construction_trace(
    const OperationMetadata&, const std::map<std::string, ParameterValue>&,
    unsigned, const ResourceSet&);
}
