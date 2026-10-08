#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "./expression_contract_fixture.h"
#include "photospider/photospider.hpp"

namespace {
struct Prototype {
  ps::ValueFacet facet;
  ps_result_facet_view_v2 view{};
  ps_result_tensor_spec_v2 tensor{};
  ps_result_schema_v2 schema{};
  ps_result_port_v2 port{};
  Prototype(ps::ValueFacet facet, std::uint64_t count)
      : facet(std::move(facet)) {
    view = {sizeof(view),
            this->facet.key.data(),
            static_cast<std::uint32_t>(this->facet.key.size()),
            this->facet.version,
            this->facet.payload.data(),
            static_cast<std::uint32_t>(this->facet.payload.size())};
    tensor.struct_size = sizeof(tensor);
    tensor.key = "samples";
    tensor.key_size = 7;
    tensor.element_type = PS_RESULT_ELEMENT_FLOAT32_V2;
    tensor.rank = 1;
    tensor.shape[0] = count;
    tensor.channel_axis = PS_RESULT_NO_CHANNEL_V2;
    tensor.facets = &view;
    tensor.facet_count = 1;
    schema.struct_size = sizeof(schema);
    schema.id = "fixture.expression";
    schema.id_size = 18;
    schema.version = 1;
    schema.publication = PS_RESULT_COMPLETE_BUNDLE_V2;
    schema.tensors = &tensor;
    schema.tensor_count = 1;
    port.struct_size = sizeof(port);
    port.kind = PS_RESULT_OBJECT_V2;
    port.element_type = PS_RESULT_ELEMENT_FLOAT32_V2;
    port.rank = 1;
    port.requires_semantics = 1;
    port.schema = &schema;
  }
};
ps::ValueFacet prototype_facet() {
  ps::SemanticDescriptor semantic;
  semantic.kind = ps::SemanticKind::SampledSignal;
  semantic.sample_step = 1;
  semantic.sample_axis_unit = "dimensionless";
  semantic.channels = {{"value", "value", "dimensionless"}};
  auto facet = ps::encode_semantic(semantic);
  if (!facet.ok())
    throw std::runtime_error(facet.status().message);
  return facet.take_value();
}
int status_code(const ps::Status& status) {
  return status.ok()                                       ? 0
         : status.code == ps::ErrorCode::InvalidArgument   ? 6
         : status.code == ps::ErrorCode::TypeMismatch      ? 5
         : status.code == ps::ErrorCode::ResourceExhausted ? 4
         : status.code == ps::ErrorCode::Cancelled         ? 2
                                                           : 1;
}
}  // namespace

extern "C" const ps_result_port_v2* fixture_expression_prototype(void) {
  try {
    static const Prototype prototype(prototype_facet(), 1);
    return &prototype.port;
  } catch (...) {
    return nullptr;
  }
}
extern "C" int fixture_expression_metadata(
    void*, const ps_result_port_v2* inputs, uint32_t input_count,
    const ps_result_parameter_value_v2* parameters, uint32_t parameter_count,
    const ps_result_port_v2*, uint32_t output_count,
    const ps_result_metadata_sink_v2* sink) {
  try {
    if (input_count != 1 || output_count != 1 || !inputs[0].schema ||
        inputs[0].schema->field_count || inputs[0].schema->tensor_count != 1)
      return 5;
    const auto& tensor = inputs[0].schema->tensors[0];
    if (tensor.rank != 1 || tensor.batch_rank ||
        tensor.element_type != PS_RESULT_ELEMENT_FLOAT64_V2)
      return 5;
    std::map<std::string, ps::ParameterValue> values;
    for (std::uint32_t i = 0; i < parameter_count; ++i) {
      const auto& p = parameters[i];
      std::string key(p.key, p.key_size);
      if (p.type == PS_RESULT_PARAMETER_STRING_V2)
        values.emplace(key, std::string(p.string_value, p.string_size));
      else if (p.type == PS_RESULT_PARAMETER_INT64_V2)
        values.emplace(key, p.int64_value);
      else if (p.type == PS_RESULT_PARAMETER_FLOAT64_V2)
        values.emplace(key, p.float64_value);
      else
        return 6;
    }
    if (std::get<std::int64_t>(values.at("count")) > 1048576)
      return 5;
    ps::SchemaTemplate schema;
    schema.id.assign(inputs[0].schema->id, inputs[0].schema->id_size);
    schema.version = inputs[0].schema->version;
    ps::ResultTensorSpec member;
    member.key.assign(tensor.key, tensor.key_size);
    member.descriptor = {ps::ElementType::Float64, {tensor.shape[0]}};
    for (std::uint32_t i = 0; i < tensor.facet_count; ++i) {
      const auto& f = tensor.facets[i];
      ps::ValueFacet facet;
      facet.key.assign(f.key, f.key_size);
      facet.version = f.version;
      if (f.payload_size)
        facet.payload.assign(f.payload, f.payload + f.payload_size);
      member.facets.push_back(std::move(facet));
    }
    schema.tensors.push_back(std::move(member));
    ps::OperationMetadata input;
    input.result_schema =
        std::make_shared<const ps::SchemaTemplate>(std::move(schema));
    static const auto registry = ps::make_default_operation_registry();
    auto traits =
        registry->resolve_traits("numeric.sample_expression", {input}, values);
    if (!traits.ok())
      return status_code(traits.status());
    const auto& output = traits.value().outputs[0].result_schema->tensors[0];
    Prototype resolved(output.facets.at(0), output.descriptor.shape.at(0));
    return sink->set_output(sink->context, 0, &resolved.port);
  } catch (const std::bad_alloc&) {
    return 4;
  } catch (...) {
    return 1;
  }
}
