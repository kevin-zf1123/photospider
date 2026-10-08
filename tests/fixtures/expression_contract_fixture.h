#ifndef TESTS_FIXTURES_EXPRESSION_CONTRACT_FIXTURE_H_
#define TESTS_FIXTURES_EXPRESSION_CONTRACT_FIXTURE_H_
#include "photospider/plugin/result_operation_plugin_api.h"
#ifdef __cplusplus
extern "C" {
#endif
const ps_result_port_v2* fixture_expression_prototype(void);
int fixture_expression_metadata(void*, const ps_result_port_v2*, uint32_t,
                                const ps_result_parameter_value_v2*, uint32_t,
                                const ps_result_port_v2*, uint32_t,
                                const ps_result_metadata_sink_v2*);
#ifdef __cplusplus
}
#endif
#endif  // TESTS_FIXTURES_EXPRESSION_CONTRACT_FIXTURE_H_
