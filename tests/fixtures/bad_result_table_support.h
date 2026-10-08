#ifndef TESTS_FIXTURES_BAD_RESULT_TABLE_SUPPORT_H_
#define TESTS_FIXTURES_BAD_RESULT_TABLE_SUPPORT_H_

#include <stdatomic.h>
#include <stdint.h>

// Bad fixtures have independent mappings from the successful runtime fixture.
// Selection and loading are serial within one observer lifetime. Each getter
// restores canonical records before applying exactly one table defect.
#ifdef PS_RESULT_BAD_TABLES
static uint32_t ps_test_bad_case;
static _Atomic uint64_t ps_test_plugin_retirements;
PS_RESULT_EXPORT int ps_test_select_bad_result_case(uint32_t id) {
  if (!id || id > PS_BAD_RESULT_TABLE_COUNT)
    return 1;
  ps_test_bad_case = id;
  return 0;
}
PS_RESULT_EXPORT uint64_t ps_test_bad_result_destroy_count(void) {
  return atomic_load(&ps_test_plugin_retirements);
}
static void ps_test_bad_result_retired(void) {
  atomic_fetch_add(&ps_test_plugin_retirements, 1);
}
#else
static void ps_test_bad_result_retired(void) {}
#endif

#endif  // TESTS_FIXTURES_BAD_RESULT_TABLE_SUPPORT_H_
