#include <atomic>
#include <cstdint>

#include "./result_registry_fixture.hpp"

namespace {
std::atomic<std::uint32_t> destroy_count{0};
std::atomic<std::uint32_t> callback_count{0};
int context_marker;
int never(void*, void*, const ps_result_query_v2*,
          const ps_result_services_v2*) {
  ++callback_count;
  return 1;
}
ps_result_field_spec_v2 make_field() {
  ps_result_field_spec_v2 field{};
  field.struct_size = sizeof(field);
  field.key = "record";
  field.key_size = 6;
  field.element_type = PS_RESULT_ELEMENT_FLOAT64_V2;
  field.record_rank = 1;
  field.record_shape[0] = UINT64_MAX;
  field.rows.kind = PS_RESULT_EXTENT_FIXED_V2;
  field.rows.value = 1;
  field.rows.divisor = 1;
  return field;
}
const auto field = make_field();
ps_result_schema_v2 make_schema() {
  ps_result_schema_v2 schema{};
  schema.struct_size = sizeof(schema);
  schema.id = "fixture.record";
  schema.id_size = 14;
  schema.version = 1;
  schema.publication = PS_RESULT_COMPLETE_BUNDLE_V2;
  schema.fields = &field;
  schema.field_count = 1;
  return schema;
}
const auto schema = make_schema();
const auto output = result_registry_fixture::make_output(&schema);
ps_result_operation_v2 make_operation() {
  auto operation = result_registry_fixture::make_operation(
      "fixture.dense_overflow", 22, nullptr, 0, &output);
  operation.start = never;
  operation.poll = never;
  return operation;
}
const auto operation = make_operation();
void destroy_fixture(void* context) {
  if (context == &context_marker)
    ++destroy_count;
}
// NOLINTBEGIN(whitespace/indent_namespace)
const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),     PS_RESULT_OPERATION_ABI_VERSION_2,
    &operation,      1,
    &context_marker, destroy_fixture};
// NOLINTEND
}  // namespace

extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_dense_overflow_fixture_destroy_count(void) {
  return destroy_count.load();
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_dense_overflow_fixture_callback_count(void) {
  return callback_count.load();
}
