#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/status_helpers.hpp"
#include "plugin/result_plugin.hpp"

namespace ps::plugin_internal::result_c {
// Immutable definition retains the DSO until the last C continuation destroys.
struct Definition {
  ps_result_operation_v3 api;
  std::shared_ptr<void> library;
  OperationTraits traits;
  std::vector<OperationMetadata> input_prototypes, output_prototypes;
};
// C views borrow metadata strings and payloads. Containers own only view
// arrays; the definition/query must outlive the synchronous C callback.
struct MetadataView {
  struct Port {
    ps_result_schema_v3 schema{};
    ResourceVector<ps_result_field_spec_v3> fields;
    ResourceVector<ps_result_tensor_spec_v3> tensors;
    ResourceVector<ps_result_extent_v3> domain;
    ResourceVector<ps_result_facet_view_v3> facets, metadata;
    ResourceVector<ResourceVector<ps_result_group_v3>> groups;
    ResourceVector<ResourceVector<ps_result_facet_view_v3>> image_facets;
  };
  Status status;
  ResourceVector<Port> storage;
  ResourceVector<ps_result_port_v3> ports;
  static ps_result_extent_v3 encode(const ResultExtent& e) {
    return {static_cast<uint32_t>(e.kind),
            e.input,
            e.axis,
            e.field,
            e.value,
            e.divisor,
            e.offset};
  }
  template <class Facets>
  static void encode_facets(const Facets& source,
                            ResourceVector<ps_result_facet_view_v3>& target) {
    target.reserve(source.size());
    for (const auto& f : source) {
      ps_result_facet_view_v3 view{};
      view.struct_size = sizeof(view);
      view.key = f.key.data();
      view.key_size = f.key.size();
      view.version = f.version;
      view.payload = f.payload.empty() ? nullptr : f.payload.data();
      view.payload_size = f.payload.size();
      target.push_back(view);
    }
  }
  explicit MetadataView(const std::vector<OperationMetadata>& source);
  MetadataView(const OperationMetadata* source, size_t count);
};
struct QueryFrame {
  Status status;
  MetadataView input_views, output_view;
  TileSet requested;
  ps_result_tile_grid_v3 output_tile_grid{};
  ResourceVector<ps_result_tile_grid_v3> input_tile_grids;
  ResourceVector<ps_result_parameter_value_v3> parameters;
  ps_result_query_v3 query{};
  ps_result_output_v3 resolved_output{};
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
Result<SchemaTemplate> schema(const ps_result_schema_v3*);
Result<OperationMetadata> port(const ps_result_port_v3&);
Result<OperationPortConstraint> constraint(const ps_result_port_v3&);
Result<OperationPreparation> prepare_metadata(
    const Definition&, const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&);
}  // namespace ps::plugin_internal::result_c
