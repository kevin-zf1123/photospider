# Testing and Validation

CMake registrations in `cmake/KernelTests.cmake`, `cmake/PackageTests.cmake`, `cmake/BehaviorTests.cmake` and optional `cmake/BenchmarkTools.cmake` define executable tests, dependencies, labels, timeouts and skips. Registration is not evidence that a test has run.

## Test ownership and lifetime

For new or remigrated permanent cases, retain only minimal regressions for confirmed defects and existing package contracts. A necessary positive control must directly support the defect witness. Review historical cases individually: renaming a file, moving directories or adapting an ABI neither creates nor proves a historical defect. A mixed file does not justify retaining every scenario. Failure to compile against a removed API and failure after an artificial mutation alone are insufficient evidence.

General feature coverage, preventive negative cases, combinations and performance comparisons may be temporary acceptance checks. Remove their source, data, hooks, fixture targets and CMake/CTest registrations after validation. Ordinary ABI coverage cannot evade this policy by being renamed a package contract. Keep independent operation oracles, mathematical references, tolerances and existing reference data under `oracle/ops`; do not change them to fit an implementation.

Kernel regressions live under `tests/unit/kernel` and `tests/integration/kernel` and build without bundled ops. Existing package contracts live under `tests/package`. The version contract checks generated SameMinorVersion behavior without installing or building an external consumer. Do not add, restore or run installation-consumer tests while kernel development remains incomplete.

Bundled-operation numerical checks, cross-operation workflows and example checks are written locally when needed and removed when the change is validated. Acceptance scenarios in a specification are not a permanent-test checklist. Deleted whole-result, dynamic-field and default semantic-certification assertions are not mechanically replaced by equally broad preventive coverage.

Benchmark runner/oracle/report types belong to optional `Photospider::benchmark_tools`, disabled by default. Tool regressions under `tests/tools` follow the same confirmed-defect policy; enabling the target does not authorize retaining every benchmark scenario. The kernel keeps raw timing, counters and resource observations.

## Configure and run

Reuse an existing configured build and select affected targets and tests:

```sh
cmake --build build/kernel-dev --target <affected-targets> -j 8
ctest --test-dir build/kernel-dev -R '<focused-test-regex>' --output-on-failure
```

Build the actual test targets before interpreting CTest results. Configure only when needed. Full CTest, clean builds, sanitizers and platform matrices are not default steps. They require explicit user authorization or a task directly targeting the corresponding validation artifact. Reviewed-mode risk assessment alone does not authorize them. Formatting of changed C++ files uses ClangFormat 21 and cpplint.

Concurrency regressions use events or condition-variable gates to establish interleavings, not guessed sleeps. Remove orphan helpers, hooks and DSO targets only after retained regressions no longer depend on them. Private test seams do not enter the public plugin ABI.

## C Result table rejection

Retained C11 fixtures exercise actual importer defects through `OperationRegistry::load_plugin`. `test_result_repeated` retains a valid-table control and the historical missing-resolver case 6. Other retained table cases follow their own confirmed history; fixture case counts are not a compatibility promise. See `cmake/KernelTests.cmake` for current fixture dependencies.

Module tables and state retire before their DSO owner. Lifecycle assertions must observe real destruction while the observer module is still mapped. ABI-size rejection alone does not prove a previously reported behavioral defect.

## Behavior boundaries

Tests assert observable values, coverage, errors, cancellation, ownership and resource effects. Tile Needs and immutable shared evidence define dependencies; restoring evidence does not restore read authority. Cache hits alone are not a correctness oracle. Before/after failure evidence and the smallest input/assertion identify a retained regression.

Report CPU, mock, native pass, native failure and native skip separately. Hardware-dependent tests may use skip code 77 when the backend/device is unavailable. Once native work is admitted, unexpected failure remains a failure. A mock or mathematical oracle does not establish native GPU execution. Report only commands actually run, with untested backends stated explicitly.

Error regressions isolate the relevant condition and compare the original structured status where propagation is the contract. Exact counts are appropriate for specified charges or lifecycle events; implementation-only hit-rate or scheduler timing expectations are not permanent behavior guarantees.
