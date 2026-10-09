# Testing and Validation

This guide describes the registered behavior tests and installed-package gates. CMake declarations in `CMakeLists.txt`, `cmake/BehaviorTests.cmake`, and `tests/consumer/CMakeLists.txt` define the executable, fixtures, dependencies, command, labels, timeout, and skip code for each test.

## Configure and run

Provide the pinned SLEEF 3.9.0 source under `third_party/sleef/` before configuring; CMake does not download it. A standard local build uses:

```sh
cmake -S . -B build/check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/check -j8
ctest --test-dir build/check --output-on-failure
```

The build's `photospider_tests` target builds the registered test executables. CTest labels let you run a behavior family with `ctest --test-dir build/check -L 'unit|execution' --output-on-failure`, or inspect label membership with `ctest --test-dir build/check --print-labels`. Unit tests have 120-second timeouts, integration tests 300 seconds, and large numeric or image tests 600 seconds. Only the top-level `test_installed_consumer` has a timeout of 1800 seconds. The nested `installed_*` tests use the normal family limits: 120 seconds for unit tests, 300 seconds for integration tests, and 600 seconds for large numeric or image tests. Sanitizer builds multiply each applicable timeout by three.

## Installed-package consumers

`test_installed_consumer` installs the just-built package to an isolated prefix, configures `tests/consumer` with `find_package(Photospider 0.33 CONFIG REQUIRED COMPONENTS kernel ops_headers operation_sdk data_provider_sdk)`, and builds and runs the default package gate. The gate uses only installed public headers and exported targets. Each consumer executable links `Photospider::kernel` and `Photospider::ops_headers`, so the installed `photospider/ops/` helper headers are checked together with the kernel headers. A consumer of the operation helpers outside this repository follows the same pattern. Its registration declarations derive test commands and build dependencies together, including fixture DSOs. Tests that require private symbols link a noninstalled static test library built from the normal kernel objects; fault-injection variants replace only translation units that contain the relevant test seam. That library also compiles the private execution test hooks, which let concurrency tests wait for real events instead of timing. A phase work gate pauses the callback thread at a selected positive work charge inside an executor poll. A shared-join event fires after a waiter has acquired its lease on a shared producer. The operation fixture DSO exports private arm, wait and release symbols, outside the operation ABI, that hold a native callback until the test releases it; the callback then calls the real cancellation service. The local `test_computed_scalar_sharing` registration (`test_computed_scalar --sharing-only`) is built with `PHOTOSPIDER_LOCAL_EXECUTION_SYNC_TESTS` and covers cancellation of one caller of a shared computed scalar. The installed `installed_computed_scalar` test builds the same source against the installed public package only, without these hooks, and rejects any command-line argument.

The consumer project exposes two explicit targets:

```sh
cmake -S tests/consumer -B build/consumer-build \
  -DCMAKE_PREFIX_PATH=/path/to/photospider-prefix
cmake --build build/consumer-build --target run_photospider_consumer
cmake --build build/consumer-build --target run_photospider_consumer_all
```

`run_photospider_consumer` runs the 12-test core package gate. `run_photospider_consumer_all` builds the registered installed targets and runs all 39 installed tests. The latter includes optional hardware checks; an unavailable device returns the registered skip code 77.

The top-level installed gate is run through CTest after configuring the kernel build. Its nested configure receives the active generator, platform, toolset, configuration, and sanitizer mode. Matching sanitizer instrumentation reaches the consumer's C/C++ compilation and link steps without adding sanitizer flags to the installed package export. Repository and installed-consumer `MODULE` fixtures receive the matching sanitizer instrumentation. C11 operation fixtures remain compiled as C. On macOS ASAN builds, each module also contains a test-only C++ atexit owner that unregisters compiler-rt image globals during module retirement by `dlclose`, before the image is unmapped; see the [module instrumentation helper](../../cmake/BehaviorTests.cmake) and [retirement owner](../../tests/support/asan_module_retirement.cpp).

## C Result table rejection

Bad Result table fixtures are compiled as C11 `MODULE` libraries by the shared [`photospider_add_result_table_fixture` helper](../../cmake/BehaviorTests.cmake). Four selector-driven DSOs cover 34 malformed-table cases: operation (14), joint (10), repeated input (6), and RequestRecord (4). The four base fixtures are [`result_operation_fixture.c`](../../tests/fixtures/result_operation_fixture.c), [`result_joint_fixture.c`](../../tests/fixtures/result_joint_fixture.c), [`result_repeated_fixture.c`](../../tests/fixtures/result_repeated_fixture.c), and [`result_request_record_fixture.c`](../../tests/fixtures/result_request_record_fixture.c); they share [bad-table selector support](../../tests/fixtures/bad_result_table_support.h). The [test helper](../../tests/support/bad_result_table_fixture.hpp) selects one case and calls the real `OperationRegistry::load_plugin`; each case checks the expected `Status.code`, an empty registry key list, and the exact plugin-destroy count. The registry and any candidate are destroyed before the observer handle is closed, so retirement is checked while the DSO is still mapped. The successful fixture modules and the separate throwing C++ joint fixture remain independent.

The root integration tests are [`test_result_plugin`](../../tests/integration/test_result_plugin.cpp), [`test_result_c_joint`](../../tests/integration/test_result_c_joint.cpp), [`test_result_repeated`](../../tests/integration/test_result_repeated.cpp), and [`test_result_request_record`](../../tests/integration/test_result_request_record.cpp). Their fixture dependencies come from the [root CMake registrations](../../CMakeLists.txt). To build and run this focused set:

```sh
cmake --build build/check --target \
  test_result_plugin test_result_c_joint test_result_repeated \
  test_result_request_record -j8
ctest --test-dir build/check \
  -R '^(test_result_plugin|test_result_c_joint|test_result_repeated|test_result_request_record)$' \
  --output-on-failure
```

The installed consumers in [`tests/consumer/CMakeLists.txt`](../../tests/consumer/CMakeLists.txt) reuse the same integration sources and bad-table helper: `installed_unified_result_c11`, `installed_result_c_joint`, `installed_result_repeated`, and `installed_result_request_record`. They are ordinary required tests in the installed suite, not hardware skips. After configuring `tests/consumer` as described above, run them with:

```sh
cmake --build build/consumer-build --target \
  photospider_result_c_consumer photospider_result_c_joint_consumer \
  photospider_repeated_result_consumer photospider_request_record_consumer
ctest --test-dir build/consumer-build \
  -R '^(installed_unified_result_c11|installed_result_c_joint|installed_result_repeated|installed_result_request_record)$' \
  --output-on-failure
```

## Behavior boundaries

The registered suite covers compiler validation and planning, Result operations and C ABI loading, execution and cancellation, dependency relations and shared work, resource limits, numeric and image behavior, and native backend dispatch. Installed consumers exercise package export, public C and C++ headers, expression metadata resolution, Result C and C++ workflows, repeated-input contracts, binding and demand behavior, cache behavior, cancellation, and backend admission.

Hardware-specific tests are registered with skip return code 77. Their software contract may still be covered by deterministic CPU or fault-injection tests, but those do not establish native device execution. Report CPU/mock coverage and actual hardware execution separately.

Tests assert observable values, error categories, resource limits, cancellation, publication, and ownership behavior. Cache internals such as block-cache hit counts are implementation details; performance benchmarking is tracked separately from this behavior suite.
