#include "photospider/plugin/data_provider_api.h"
static const ps_data_schema_v1 schemas[] = {
    {sizeof(ps_data_schema_v1), "fixture.uint8", 13, 1, 8},
    {sizeof(ps_data_schema_v1), "fixture.int64", 13, 2, 8},
    {sizeof(ps_data_schema_v1), "fixture.float64", 15, 3, 8},
    {sizeof(ps_data_schema_v1), "fixture.float32", 15, 4, 8}};
static void destroy(const ps_data_schema_v1* records, uint32_t count) {
  (void)records;
  (void)count;
}
static const ps_data_provider_api_v1 api = {sizeof(ps_data_provider_api_v1), 4,
                                            schemas, destroy};
PS_DATA_PROVIDER_EXPORT uint32_t ps_data_provider_get_abi_version(void) {
  return PS_DATA_PROVIDER_ABI_VERSION_1;
}
PS_DATA_PROVIDER_EXPORT const ps_data_provider_api_v1*
ps_data_provider_get_api_v1(void) {
  return &api;
}
