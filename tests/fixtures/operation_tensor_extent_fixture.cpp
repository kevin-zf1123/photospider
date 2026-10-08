#include <array>
#include <atomic>
#include <cstdint>

#include "./result_registry_fixture.hpp"

#ifndef PS_TENSOR_EXTENT_CASE
#error "PS_TENSOR_EXTENT_CASE must select a tensor schema case"
#endif

namespace {
std::atomic<std::uint32_t> destroy_count{0};
std::atomic<std::uint32_t> callback_count{0};
int context_marker;
int never(void*, void*, const ps_result_query_v2*,
          const ps_result_services_v2*) {
  ++callback_count;
  return 1;
}
std::array<ps_result_tensor_spec_v2, 6> make_tensors() {
  const char* keys[] = {"rank1_limit", "rank1_large",   "rank2_limit",
                        "rank2_large", "float64_large", "product_overflow"};
  const std::uint32_t sizes[] = {11, 11, 11, 11, 13, 16};
  std::array<ps_result_tensor_spec_v2, 6> tensors{};
  for (std::uint32_t i = 0; i < tensors.size(); ++i) {
    tensors[i] = result_registry_fixture::make_tensor();
    tensors[i].key = keys[i];
    tensors[i].key_size = sizes[i];
    tensors[i].element_type = PS_RESULT_ELEMENT_UINT8_V2;
  }
  tensors[0].shape[0] = UINT64_C(1) << 63U;
  tensors[1].shape[0] = (UINT64_C(1) << 63U) + 1;
  tensors[2].rank = tensors[3].rank = tensors[5].rank = 2;
  tensors[2].shape[0] = tensors[3].shape[0] = 2;
  tensors[2].shape[1] = UINT64_C(1) << 62U;
  tensors[3].shape[1] = (UINT64_C(1) << 62U) + 1;
  tensors[4].element_type = PS_RESULT_ELEMENT_FLOAT64_V2;
  tensors[4].shape[0] = UINT64_MAX;
  tensors[5].shape[0] = tensors[5].shape[1] = UINT64_MAX;
#if PS_TENSOR_EXTENT_CASE == 2
  tensors[5].shape[1] = 0;
#elif PS_TENSOR_EXTENT_CASE == 3
  tensors[5].rank = 9;
#elif PS_TENSOR_EXTENT_CASE != 1
#error "PS_TENSOR_EXTENT_CASE must be in 1..3"
#endif
  return tensors;
}
const auto tensors = make_tensors();
ps_result_schema_v2 make_schema() {
  auto schema = result_registry_fixture::make_schema();
  schema.tensors = tensors.data();
  schema.tensor_count = tensors.size();
  return schema;
}
const auto schema = make_schema();
const auto output = result_registry_fixture::make_output(&schema);
ps_result_operation_v2 make_operation() {
  auto operation = result_registry_fixture::make_operation(
      "fixture.tensor.extents", 22, nullptr, 0, &output);
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
ps_operation_tensor_extent_fixture_destroy_count(void) {
  return destroy_count.load();
}
extern "C" PS_RESULT_EXPORT std::uint32_t
ps_operation_tensor_extent_fixture_callback_count(void) {
  return callback_count.load();
}
