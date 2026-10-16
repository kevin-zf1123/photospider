#include "photospider/plugin/operation_registry.hpp"
#include "support/bad_result_table_fixture.hpp"
#include "support/test_support.hpp"

int main() {
  ps::OperationRegistry registry;
  PS_CHECK(registry.load_plugin(PS_REPEATED_FIXTURE).ok());
  PS_CHECK(ps::test::check_bad_result_tables(
      PS_BAD_REPEATED_FIXTURE, {{6, "unresolved heterogeneous group"}}));
  return 0;
}
