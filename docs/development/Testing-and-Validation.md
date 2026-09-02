# Testing and Validation

This document defines maintained repository validation after the breaking
scope reset. Tests validate long-lived software behavior, not migration
completion or provenance.

## Development loop

During implementation, use scoped formatting/lint, affected targets, and
focused tests. After source and documentation freeze, run at most one native
clean configure, one full build, and one complete CTest/JUnit pass. Do not use
Docker or local architecture emulation for that final pass.

## Required behavior areas

Kernel tests cover:

- WorkflowDocument and graph/IR/plan validation;
- typed stage identity and canonical digest separation;
- cross-registry IR/plan rejection even for equal operation keys;
- CPU compile-plan-execute and optional GPU selection/fallback;
- multiple independent graph/execution contexts;
- one ExecutionContext-wide waiting-callback bound shared across deterministic
  CPU/GPU FIFOs, recovery after a worker pop, and ordinary mixed-lane
  concurrent Runs;
- bounded ready work and `ResourceLedger` settlement;
- cross-backend copy/backend labels, cancellation, stale completion, and
  exception fences;
- Value/Region/strided-layout/facet/buffer negative contracts;
- operation/provider ABI version/size/alignment/pointer/count/bounds/lifetime,
  including operation-v2 typed parameter schemas, demand views, and
  deterministic owner-allocation failure with exact destroy/close counts;
- parameter unknown/missing/wrong-type/conflict rejection before semantic IR;
- side-effecting/non-cacheable operation preservation across semantic,
  optimized, and plan stages, plus serial repeated-execution callback order,
  invocation counts, and absence of result reuse;
- Whole/Elementwise/Halo demand propagation and execution-time coverage;
- raw benchmark diagnostics with a named oracle or explicit `unchecked`
  identity and without verdict/evidence output, including rejected/throwing
  oracles that retain completed compile/plan/execute/operation/backend/digest
  observations while reporting correctness separately. An execution
  `Cancelled` result at any iteration aborts the whole run without publishing
  a partial/success report. The runner also observes cancellation immediately
  before and after a caller oracle and again at the final report-publication
  linearization point. The oracle receives no token and cannot be preempted;
  cancellation observed after it returns false or raises an ordinary exception
  takes precedence over that oracle outcome, while cancellation after the
  final publication observation does not revoke the returned report. Other
  execution failures remain samples and later iterations continue. Duration
  assertions permit zero because the monotonic clock may have microsecond
  resolution. Deterministic regressions cover an oracle barrier cancelled by
  another thread, self-cancelling true/false/throwing oracles, and the
  no-oracle post-execute window through the noninstalled test-kernel seam.

Daemon tests live in `photospider-daemon` and cover local frame validation,
nine-method routing, ephemeral Session/Job lifecycle, restart loss,
multi-Session behavior, cancellation, Session close, result release, shutdown,
and the isolated installed-kernel boundary.

## Installed boundary

The package gate configures and installs Photospider to a fresh prefix, then
configures an external C/C++17 consumer using only
`find_package(Photospider CONFIG REQUIRED)`. CI runs this gate for both the
default static kernel and `BUILD_SHARED_LIBS=ON`. It verifies:

- installed headers match the declared public inventory;
- exports contain no source/private paths;
- the linked C SDK compilation unit and C++ embedded compile/execute facade
  both run;
- `kernel`, `operation_sdk`, and `data_provider_sdk` component discovery
  exports exactly `Photospider::kernel`, `Photospider::operation_sdk`, and
  `Photospider::data_provider_sdk`; the SDK targets are header-only;
- removed targets, headers, components, and executables are absent.

Daemon validation must use that isolated prefix, never a sibling checkout or
private include directory.

The deterministic cross-lane waiting test uses a private callback-enqueue hook
compiled only into the noninstalled `photospider_test_kernel`. With
`BUILD_TESTING=ON`, the product archive, installed kernel, exports, and ordinary
consumer remain hook-free; with `BUILD_TESTING=OFF`, neither the test-kernel
target nor its execution-hook object exists.

## Sanitizers and malformed-input validation

ASAN and TSAN are scoped CMake modes where the toolchain supports them. The
ordinary tests exercise malformed Value/Region/layout, graph documents,
operation/provider records, and callback outputs. Malformed local IPC frames
belong to the daemon repository.

The long-lived manual target `photospider_operation_contract_ir_fuzz` exercises
operation-v2 trait/parameter vocabulary and compiler validation. It is
`EXCLUDE_FROM_ALL`, is never registered with CTest, and is enabled explicitly
with `-DPHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS=ON` under Clang. Seed inputs are
maintained in `tests/fuzz/corpus/operation_contract_ir/`; caller-selected crash
or artifact directories remain untracked. A bounded smoke run is:

```bash
cmake -S . -B <fuzz-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DPHOTOSPIDER_BUILD_MANUAL_FUZZ_TARGETS=ON -DBUILD_TESTING=OFF
cmake --build <fuzz-build> --target photospider_operation_contract_ir_fuzz -j
ps_operation_fuzz_corpus=$(mktemp -d)
cp -R tests/fuzz/corpus/operation_contract_ir/. \
  "$ps_operation_fuzz_corpus"/
<fuzz-build>/photospider_operation_contract_ir_fuzz \
  "$ps_operation_fuzz_corpus" -runs=1000 -max_len=256
```

The fixed negative DSO fixtures remain authoritative for raw ABI
pointer/size/alignment/count/bounds cases that a byte-only in-process harness
cannot construct safely.
Use a Clang distribution that actually ships its libFuzzer runtime; a compiler
identifying as Clang is insufficient when that archive is absent. The temporary
working corpus prevents generated mutations from entering the maintained seed
directory.

The ordinary `test_operation_contract_ir_seeds` CTest reads the two committed
seeds without generating mutations. It proves that `valid-source` reaches and
passes the compiler path while `malformed-schema` constructs a duplicate
parameter schema and reaches the named registry rejection. This deterministic
stage seam complements, but does not register, the manual libFuzzer target.

## CTest ownership

CTest/CI entries are reserved for correctness, performance, stability,
multithreading, error handling, package consumption, compilation, and runtime
boundaries. Do not register stale-term searches, source-layout audits,
migration checklists, Doxygen audits, Issue replay, or result/provenance
orchestration. Manual source-quality tools require maintained English and
Chinese documentation and remain outside CTest/CI.

## Final commands

The exact final build directory and optional capability flags are recorded in
the completion report. The normal shape is:

```bash
cmake -S . -B <clean-build> -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON
cmake --build <clean-build> -j
ctest --test-dir <clean-build> --output-on-failure --output-junit <report>
cmake --install <clean-build> --prefix <fresh-prefix>
```

Format changed C/C++ with ClangFormat 21 and lint the same files with
`python3 -m cpplint`. Record unsupported sanitizer/GPU platforms as limitations
rather than successful gates.
