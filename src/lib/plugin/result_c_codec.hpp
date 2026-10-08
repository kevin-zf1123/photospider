#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "plugin/result_plugin.hpp"

namespace ps::plugin_internal::result_c {
// Immutable definition retains the DSO until the last C continuation destroys.
struct Definition {
  ps_result_operation_v2 api;
  ps_result_joint_program_v2 joint{};
  std::shared_ptr<void> library;
  OperationTraits traits;
  std::vector<OperationMetadata> input_prototypes, output_prototypes;
};
// C views borrow metadata strings and payloads. Containers own only view
// arrays; the definition/query must outlive the synchronous C callback.
struct MetadataView {
  struct Port {
    ps_result_schema_v2 schema{};
    ResourceVector<ps_result_field_spec_v2> fields;
    ResourceVector<ps_result_tensor_spec_v2> tensors;
    ResourceVector<ps_result_extent_v2> domain;
    ResourceVector<ps_result_facet_view_v2> facets, metadata;
    ResourceVector<ResourceVector<ps_result_group_v2>> groups;
    ResourceVector<ResourceVector<ps_result_facet_view_v2>> image_facets;
  };
  Status status;
  ResourceVector<Port> storage;
  ResourceVector<ps_result_port_v2> ports;
  static ps_result_extent_v2 encode(const ResultExtent& e) {
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
                            ResourceVector<ps_result_facet_view_v2>& target) {
    target.reserve(source.size());
    for (const auto& f : source) {
      ps_result_facet_view_v2 view{};
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
  ResourceVector<ps_result_region_v2> requested;
  ResourceVector<ps_result_parameter_value_v2> parameters;
  ps_result_query_v2 query{};
  ps_result_output_v2 resolved_output{};
  QueryFrame(const ResultProgramQuery&, const Definition&,
             const ResourceBudget&);
};
template <class T>
bool array(const T* pointer, std::uint64_t count, std::uint64_t maximum) {
  return count <= maximum && ((count == 0) == (pointer == nullptr)) &&
         (!pointer ||
          reinterpret_cast<std::uintptr_t>(pointer) % alignof(T) == 0);
}
Status invalid(const char* message);
Status outcome(int code);
int code(const Status& status);
ps_result_atom_key_v2 atom_view(const AtomKey&);
AtomKey atom_key(const ps_result_atom_key_v2&);
bool empty_atom(const ps_result_atom_key_v2&);
bool empty_failure(const ps_result_atom_failure_v2&);
Result<std::string> text(const char*, std::uint32_t,
                         std::uint32_t maximum = 128);
Result<SchemaTemplate> schema(const ps_result_schema_v2*);
Result<OperationMetadata> port(const ps_result_port_v2&);
Result<OperationPortConstraint> constraint(const ps_result_port_v2&);
Result<OperationPreparation> prepare_metadata(
    const Definition&, const std::vector<OperationMetadata>&,
    const std::map<std::string, ParameterValue>&);
}  // namespace ps::plugin_internal::result_c
