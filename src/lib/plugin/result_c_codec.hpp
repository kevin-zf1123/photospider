#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/core/status_helpers.hpp"
#include "plugin/result_plugin.hpp"

namespace ps::plugin_internal::result_c {
// Immutable definition retains the DSO until the last C continuation destroys.
struct Definition {
  ps_result_operation_v5 api;
  std::shared_ptr<void> library;
  OperationTraits traits;
  std::vector<OperationMetadata> input_prototypes, output_prototypes;
};
// C views borrow metadata strings and payloads. Containers own only view
// arrays; the definition/query must outlive the synchronous C callback.
struct MetadataView {
  struct Port {
    ps_result_schema_v5 schema{};
    ResourceVector<ps_result_tensor_spec_v5> tensors;
    ResourceVector<ps_result_extent_v5> domain;
    ResourceVector<ps_result_publication_scope_v5> publication_scopes;
    ResourceVector<ps_result_extension_identity_v5> validators;
  };
  Status status;
  ResourceVector<Port> storage;
  ResourceVector<ps_result_port_v5> ports;
  static ps_result_extent_v5 encode(const ResultExtent& e) {
    return {static_cast<uint32_t>(e.kind),
            e.input,
            e.axis,
            e.value,
            e.divisor,
            e.offset};
  }
  explicit MetadataView(const std::vector<OperationMetadata>& source);
  MetadataView(const OperationMetadata* source, size_t count);
};
struct QueryFrame {
  Status status;
  MetadataView input_views, output_view;
  std::vector<OperationMetadata> stage_metadata;
  std::unique_ptr<MetadataView> stage_views;
  ResourceVector<ps_result_shared_stage_v5> stage_contracts;
  ResourceVector<ps_result_tile_grid_v5> output_grid_members;
  ps_result_tile_grids_v5 output_tile_grids{};
  ResourceVector<ResourceVector<ps_result_tile_grid_v5>> input_grid_members;
  ResourceVector<ps_result_tile_grids_v5> input_tile_grids;
  ResourceVector<ps_result_parameter_value_v5> parameters;
  ResourceVector<ps_result_resource_view_v5> resources;
  ps_result_query_v5 query{};
  ps_result_output_v5 resolved_output{};
  QueryFrame(const ResultProgramQuery&, const Definition&,
             const ResourceBudget&);
};
template <class T>
bool array(const T* pointer, std::uint64_t count, std::uint64_t maximum) {
  return count <= maximum && ((count == 0) == (pointer == nullptr)) &&
         (!pointer ||
          reinterpret_cast<std::uintptr_t>(pointer) % alignof(T) == 0);
}
using core_internal::invalid;
Status outcome(int code);
int code(const Status& status);
Result<std::string> text(const char*, std::uint32_t,
                         std::uint32_t maximum = 128);
Result<std::vector<ExtensionIdentity>> extension_ids(
    const ps_result_extension_identity_v5*, std::uint32_t);
ResourceVector<ps_result_resource_view_v5> resource_views(const ResourceSet&);
ResourceVector<ps_result_parameter_value_v5> parameters_view(
    const std::map<std::string, ParameterValue>&);
Result<ValidationDefinition> validation_definition(
    const ps_result_validation_definition_v5&, std::shared_ptr<void>);
Result<DiagnosticDefinition> diagnostic_definition(
    const ps_result_diagnostic_definition_v5&, std::shared_ptr<void>);
Result<OperationSharedStage> shared_stage(const ps_result_shared_stage_v5&);
Result<SchemaTemplate> schema(const ps_result_schema_v5*);
Result<OperationMetadata> port(const ps_result_port_v5&);
Result<OperationPortConstraint> constraint(const ps_result_port_v5&);
Result<OperationPreparation> prepare_metadata(
    const Definition&, const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&);
}  // namespace ps::plugin_internal::result_c
