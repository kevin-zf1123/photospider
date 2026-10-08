#include <atomic>
#include <cstdint>

#include "./result_registry_fixture.hpp"

#ifndef PS_DENSE_LIMIT_CASE
#error "PS_DENSE_LIMIT_CASE must select one packed-record boundary fixture"
#endif

namespace {
constexpr std::uint64_t kSignedLimit = UINT64_C(1) << 63U;
constexpr std::uint64_t kQuarterRange = UINT64_C(1) << 62U;
std::atomic<std::uint32_t> destroy_count{0};
std::atomic<std::uint32_t> callback_count{0};

#if PS_DENSE_LIMIT_CASE == 1
const std::uint64_t primary_shape[] = {kSignedLimit - 1};
constexpr char kPrimaryKey[] = "fixture.dense_rank1_limit";
#elif PS_DENSE_LIMIT_CASE == 2
const std::uint64_t primary_shape[] = {kSignedLimit};
constexpr char kPrimaryKey[] = "fixture.dense_rank1_overflow";
#elif PS_DENSE_LIMIT_CASE == 3
const std::uint64_t primary_shape[] = {2, kQuarterRange - 1};
constexpr char kPrimaryKey[] = "fixture.dense_rank2_limit";
#elif PS_DENSE_LIMIT_CASE == 4
const std::uint64_t primary_shape[] = {2, kQuarterRange};
constexpr char kPrimaryKey[] = "fixture.dense_rank2_overflow";
#elif PS_DENSE_LIMIT_CASE == 5
const std::uint64_t primary_shape[] = {1};
constexpr char kPrimaryKey[] = "fixture.dense_multi_valid";
const std::uint64_t secondary_shape[] = {kSignedLimit};
constexpr char kSecondaryKey[] = "fixture.dense_multi_invalid";
#else
#error "PS_DENSE_LIMIT_CASE must be in 1..5"
#endif

int never(void*, void*, const ps_result_query_v2*,
          const ps_result_services_v2*) {
  ++callback_count;
  return 1;
}
ps_result_field_spec_v2 make_field(const std::uint64_t* shape,
                                   std::uint32_t rank) {
  ps_result_field_spec_v2 field{};
  field.struct_size = sizeof(field);
  field.key = "record";
  field.key_size = 6;
  field.element_type = PS_RESULT_ELEMENT_UINT8_V2;
  field.record_rank = rank;
  for (std::uint32_t axis = 0; axis < rank; ++axis)
    field.record_shape[axis] = shape[axis];
  field.rows.kind = PS_RESULT_EXTENT_FIXED_V2;
  field.rows.value = 1;
  field.rows.divisor = 1;
  return field;
}
ps_result_schema_v2 make_schema(const ps_result_field_spec_v2* field) {
  ps_result_schema_v2 schema{};
  schema.struct_size = sizeof(schema);
  schema.id = "fixture.record";
  schema.id_size = 14;
  schema.version = 1;
  schema.publication = PS_RESULT_COMPLETE_BUNDLE_V2;
  schema.fields = field;
  schema.field_count = 1;
  return schema;
}
ps_result_operation_v2 make_operation(const char* key, std::uint32_t size,
                                      const ps_result_output_v2* output) {
  auto operation =
      result_registry_fixture::make_operation(key, size, nullptr, 0, output);
  operation.start = never;
  operation.poll = never;
  return operation;
}
// NOLINTBEGIN(whitespace/indent_namespace)
const auto primary =
    make_field(primary_shape, sizeof(primary_shape) / sizeof(primary_shape[0]));
const auto primary_schema = make_schema(&primary);
const auto primary_output =
    result_registry_fixture::make_output(&primary_schema);
#if PS_DENSE_LIMIT_CASE == 5
const auto secondary = make_field(secondary_shape, 1);
const auto secondary_schema = make_schema(&secondary);
const auto secondary_output =
    result_registry_fixture::make_output(&secondary_schema);
const ps_result_operation_v2 descriptors[] = {
    make_operation(kPrimaryKey, sizeof(kPrimaryKey) - 1, &primary_output),
    make_operation(kSecondaryKey, sizeof(kSecondaryKey) - 1,
                   &secondary_output)};
#else
const ps_result_operation_v2 descriptors[] = {
    make_operation(kPrimaryKey, sizeof(kPrimaryKey) - 1, &primary_output)};
#endif
// NOLINTEND
int context_marker;
void destroy_fixture(void* context) {
  if (context == &context_marker)
    ++destroy_count;
}
// NOLINTBEGIN(whitespace/indent_namespace)
const ps_result_operation_plugin_api_v2 api = {
    sizeof(api),     PS_RESULT_OPERATION_ABI_VERSION_2,
    descriptors,     sizeof(descriptors) / sizeof(descriptors[0]),
    &context_marker, destroy_fixture};
// NOLINTEND
}  // namespace

extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  return &api;
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_dense_limit_fixture_destroy_count(void) {
  return destroy_count.load();
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_dense_limit_fixture_callback_count(void) {
  return callback_count.load();
}
