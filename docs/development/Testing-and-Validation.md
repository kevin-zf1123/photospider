# Testing and Validation

This guide describes the registered behavior tests. CMake declarations in `CMakeLists.txt` and `cmake/BehaviorTests.cmake` define the executable, fixtures, dependencies, command, labels, timeout, and skip code for each test.

## Configure and run

Provide the pinned SLEEF 3.9.0 source under `third_party/sleef/` before configuring; CMake does not download it. A standard local build uses:

```sh
cmake -S . -B build/check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/check -j8
ctest --test-dir build/check --output-on-failure
```

The build's `photospider_tests` target builds the registered test executables. CTest labels let you run a behavior family with `ctest --test-dir build/check -L 'unit|execution' --output-on-failure`, or inspect label membership with `ctest --test-dir build/check --print-labels`. Unit tests have 120-second timeouts, integration tests 300 seconds, and large numeric or image tests 600 seconds. Sanitizer builds multiply each applicable timeout by three.

Tests that require private symbols link a noninstalled static test library built from the normal kernel objects; fault-injection variants replace only translation units that contain the relevant test seam. That library also compiles the private execution test hooks, which let concurrency tests wait for real events instead of timing. A phase work gate pauses the callback thread at a selected positive work charge inside an executor poll. The operation fixture DSO exports private arm, wait and release symbols, outside the operation ABI, that hold a native callback until the test releases it; the callback then calls the real cancellation service.

Repository `MODULE` fixtures receive the matching sanitizer instrumentation. C11 operation fixtures remain compiled as C. On macOS ASAN builds, each module also contains a test-only C++ atexit owner that unregisters compiler-rt image globals during module retirement by `dlclose`, before the image is unmapped; see the [module instrumentation helper](../../cmake/BehaviorTests.cmake) and [retirement owner](../../tests/support/asan_module_retirement.cpp).

## C Result table rejection

Bad Result table fixtures are compiled as C11 `MODULE` libraries by the shared [`photospider_add_result_table_fixture` helper](../../cmake/BehaviorTests.cmake). Two selector-driven DSOs cover 20 malformed-table cases: operation (14), repeated input (6). The two base fixtures are [`result_operation_fixture.c`](../../tests/fixtures/result_operation_fixture.c), [`result_repeated_fixture.c`](../../tests/fixtures/result_repeated_fixture.c); they share [bad-table selector support](../../tests/fixtures/bad_result_table_support.h). The [test helper](../../tests/support/bad_result_table_fixture.hpp) selects one case and calls the real `OperationRegistry::load_plugin`; each case checks the expected `Status.code`, an empty registry key list, and the exact plugin-destroy count. The registry and any candidate are destroyed before the observer handle is closed, so retirement is checked while the DSO is still mapped.

The root integration tests are [`test_result_plugin`](../../tests/integration/test_result_plugin.cpp), [`test_result_repeated`](../../tests/integration/test_result_repeated.cpp). Their fixture dependencies come from the [root CMake registrations](../../CMakeLists.txt). To build and run this focused set:

```sh
cmake --build build/check --target \
  test_result_plugin test_result_repeated -j8
ctest --test-dir build/check \
  -R '^(test_result_plugin|test_result_repeated)$' \
  --output-on-failure
```

## Behavior boundaries

The registered suite covers compiler validation and planning, Result operations and C ABI loading, execution and cancellation, dependency relations, resource limits, numeric and image behavior, and native backend dispatch.

Hardware-specific tests are registered with skip return code 77. Their software contract may still be covered by deterministic CPU or fault-injection tests, but those do not establish native device execution. Report CPU/mock coverage and actual hardware execution separately.

Tests assert observable values, error categories, resource limits, cancellation, publication, and ownership behavior. Cache internals such as block-cache hit counts are implementation details; performance benchmarking is tracked separately from this behavior suite.
