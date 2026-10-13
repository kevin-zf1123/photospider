# Testing and Validation

This guide describes the registered behavior tests. CMake declarations in `cmake/KernelTests.cmake`, `cmake/PackageTests.cmake` and
`cmake/BehaviorTests.cmake` define the executable, fixtures, dependencies, command, labels, timeout, and skip code for each test.

## Test ownership and lifetime

Permanent tests cover kernel behavior and package contracts. Kernel unit tests
live in `tests/unit/kernel/`; kernel integration tests live in
`tests/integration/kernel/`. They use synthetic operations and fixtures to test
compiler, execution, ABI, resource and ownership behavior, and build with
`PHOTOSPIDER_BUILD_OPS=OFF`. Package contract tests live in `tests/package/`.
The package version check evaluates the generated SameMinorVersion configuration;
it does not install the package or compile an external consumer.

Bundled-operation numerical tests, operation execution integration tests,
cross-operation workflows, examples and benchmarks are not retained as a permanent
suite. When implementing or changing an operation, create the necessary
public-path checks for that change. Keep temporary test sources
and generated test data under ignored build/output directories. Remove the temporary
checker, data and any build/test registrations as soon as validation and the
operation change are complete. Keep mathematical definitions, tolerances and
behavioral acceptance criteria in the operation specification.

A kernel fixture may perform simple arithmetic to expose scheduling or Result
behavior; that does not make it a numerical acceptance test for a bundled operator.
Tests must check program behavior, not migration status, deleted-file inventories,
workflow process text or source-quality scores.

Independent operation oracles and their reference data are retained for future
operator implementation, including Proposed specifications. The temporary-test
lifetime above applies to tests of the implementation. Oracle references live in
[`oracle/ops/`](../../oracle/ops/README.md).

## Configure and run

A standard local build uses:

```sh
cmake -S . -B build/check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DPHOTOSPIDER_BUILD_OPS=OFF
cmake --build build/check --target photospider_kernel_tests -j8
ctest --test-dir build/check -L '^(kernel|package)$' --output-on-failure
```

The build's `photospider_tests` target builds the registered test executables. CTest labels let you run a behavior family with `ctest --test-dir build/check -L 'kernel|package' --output-on-failure`, or inspect label membership with `ctest --test-dir build/check --print-labels`. Suite labels are `kernel` and `package`; level labels are `unit`, `integration` and
`contract`. GPU, mock, native, backend and ABI labels are explicit registrations,
not guesses from test names. Unit tests have 120-second timeouts, integration tests
600 seconds and package contract tests 60 seconds. Sanitizer builds multiply each applicable timeout by three.

Tests that require private symbols link a noninstalled static test library built from the normal kernel objects; fault-injection variants replace only translation units that contain the relevant test seam. That library also compiles the private execution test hooks, which let concurrency tests wait for real events instead of timing. A phase work gate pauses the callback thread at a selected positive work charge inside an executor poll. The operation fixture DSO exports private arm, wait and release symbols, outside the operation ABI, that hold a native callback until the test releases it; the callback then calls the real cancellation service.

Repository `MODULE` fixtures receive the matching sanitizer instrumentation. C11 operation fixtures remain compiled as C. On macOS ASAN builds, each module also contains a test-only C++ atexit owner that unregisters compiler-rt image globals during module retirement by `dlclose`, before the image is unmapped; see the [module instrumentation helper](../../cmake/BehaviorTests.cmake) and [retirement owner](../../tests/support/asan_module_retirement.cpp).

## C Result table rejection

Bad Result table fixtures are compiled as C11 `MODULE` libraries by the shared [`photospider_add_result_table_fixture` helper](../../cmake/BehaviorTests.cmake). Two selector-driven DSOs cover 20 malformed-table cases: operation (14), repeated input (6). The two base fixtures are [`result_operation_fixture.c`](../../tests/fixtures/result_operation_fixture.c), [`result_repeated_fixture.c`](../../tests/fixtures/result_repeated_fixture.c); they share [bad-table selector support](../../tests/fixtures/bad_result_table_support.h). The [test helper](../../tests/support/bad_result_table_fixture.hpp) selects one case and calls the real `OperationRegistry::load_plugin`; each case checks the expected `Status.code`, an empty registry key list, and the exact plugin-destroy count. The registry and any candidate are destroyed before the observer handle is closed, so retirement is checked while the DSO is still mapped.

The root integration tests are [`test_result_plugin`](../../tests/integration/kernel/test_result_plugin.cpp), [`test_result_repeated`](../../tests/integration/kernel/test_result_repeated.cpp). Their fixture dependencies come from the [root CMake registrations](../../CMakeLists.txt). To build and run this focused set:

```sh
cmake --build build/check --target \
  test_result_plugin test_result_repeated -j8
ctest --test-dir build/check \
  -R '^(test_result_plugin|test_result_repeated)$' \
  --output-on-failure
```

## Behavior boundaries

The registered suite covers compiler validation and planning, Result operations and C ABI loading, execution and cancellation, dependency relations, resource limits, and native backend dispatch.

Mock/service checks and native GPU checks have separate registrations. Only
hardware-dependent entries use skip return code 77; a native skip does not
replace a passing mock/service result. Their software contract may still be covered by deterministic CPU or fault-injection tests, but those do not establish native device execution. Report CPU/mock coverage and actual hardware execution separately.

`test_gpu_c_abi_bridge` runs `test_gpu_c_abi_execution --bridge-only` with
`kernel`, `integration`, `mock`, `abi` and `gpu` labels. It needs no GPU and has
no skip code. It checks that invalid writable flags and forged or expired tokens
are rejected before host callbacks, with valid token translation as a positive
control. The separate `test_gpu_c_abi_execution` registration covers native
execution. Run the software bridge check with:

```sh
cmake --build build/check --target test_gpu_c_abi_execution -j8
ctest --test-dir build/check -R '^test_gpu_c_abi_bridge$' -LE native --output-on-failure
```

The MSL integration tests in `tests/integration/kernel/gpu/` run their preceding
CPU checks on other builds, then skip the native phase unless Metal is compiled
(`PHOTOSPIDER_ENABLE_METAL` and `APPLE`) and the context has an available GPU.
Metal unit tests check the actual device backend. Once a native phase is admitted,
unexpected backend failures remain test failures; they are not converted to skips.

Tests assert observable values, error categories, resource limits, cancellation, publication, and ownership behavior. Cache internals such as block-cache hit counts are implementation details; performance benchmarking is tracked separately from this behavior suite.

Negative cases isolate one invalid condition while keeping other inputs valid,
so another rejection cannot satisfy the same structured error assertion. Error
propagation tests compare the captured original `Status`, including its message,
instead of hard-coding generated English text. Exact counts remain appropriate for
specified charges and direct lifecycle events. Where the contract specifies only
an upper bound, assert that bound and exercise an explicit rejection; use actual
computation and ownership observations to establish reuse and retirement.
