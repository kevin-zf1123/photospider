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
  ps_result_operation_v4 api;
  std::shared_ptr<void> library;
  OperationTraits traits;
  std::vector<OperationMetadata> input_prototypes, output_prototypes;
};
// C views borrow metadata strings and payloads. Containers own only view
// arrays; the definition/query must outlive the synchronous C callback.
struct MetadataView {
  struct Port {
    ps_result_schema_v4 schema{};
    ResourceVector<ps_result_field_spec_v4> fields;
    ResourceVector<ps_result_tensor_spec_v4> tensors;
    ResourceVector<ps_result_extent_v4> domain;
    ResourceVector<ps_result_extension_identity_v4> validators;
    ResourceVector<ResourceVector<ps_result_group_v4>> groups;
  };
  Status status;
  ResourceVector<Port> storage;
  ResourceVector<ps_result_port_v4> ports;
  static ps_result_extent_v4 encode(const ResultExtent& e) {
    return {static_cast<uint32_t>(e.kind),
            e.input,
            e.axis,
            e.field,
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
  TileSet requested;
  ps_result_tile_grid_v4 output_tile_grid{};
  ResourceVector<ps_result_tile_grid_v4> input_tile_grids;
  ResourceVector<ps_result_parameter_value_v4> parameters;
  ResourceVector<ps_result_resource_view_v4> resources;
  ps_result_query_v4 query{};
  ps_result_output_v4 resolved_output{};
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
    const ps_result_extension_identity_v4*, std::uint32_t);
ResourceVector<ps_result_resource_view_v4> resource_views(const ResourceSet&);
ResourceVector<ps_result_parameter_value_v4> parameters_view(
    const std::map<std::string, ParameterValue>&);
Result<ValidationDefinition> validation_definition(
    const ps_result_validation_definition_v4&, std::shared_ptr<void>);
Result<DiagnosticDefinition> diagnostic_definition(
    const ps_result_diagnostic_definition_v4&, std::shared_ptr<void>);
Result<SchemaTemplate> schema(const ps_result_schema_v4*);
Result<OperationMetadata> port(const ps_result_port_v4&);
Result<OperationPortConstraint> constraint(const ps_result_port_v4&);
Result<OperationPreparation> prepare_metadata(
    const Definition&, const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&);
}  // namespace ps::plugin_internal::result_c
