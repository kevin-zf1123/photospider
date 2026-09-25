# Testing and Validation

This document defines maintained repository validation after the breaking
scope reset. Tests validate long-lived software behavior, not migration
completion or provenance.

## Build prerequisite

Before configuring a checkout or source archive, download the pinned SLEEF
3.9.0 source into `third_party/sleef/` using the
[documented commands](../../third_party/SLEEF.md). This ignored directory must
also be supplied for offline builds. CI checks out the same upstream commit
separately before configuration; CMake never downloads it. Installed-package
consumers do not need this source dependency.

## Development loop

During implementation, use scoped formatting/lint, affected targets, and
focused tests. Only for an explicitly requested release pass, after source and documentation freeze, run at most one native
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
- first-failure priority across no-GPU fallback denial, waiting-admission
  rejection, backend queue rejection, and submission exception fallback:
  cancellation precedes graph `Stale`, which precedes the original failure,
  with no stale result and exact waiting/in-flight/resource recovery. Backend
  submit rejection and exception-fallback cases use a GPU-enabled context, an
  explicitly GPU physical plan, a present GPU lane, and exact `Backend::Gpu`
  hook consumption; they prove no CPU callback substitutes for the GPU path
  and that clearing the hook restores successful GPU execution. A separate
  CPU queue-rejection case retains independent CPU coverage. Separate
  no-sleep cancellation and stale races occupy the sole CPU worker, hold a GPU
  callback in the target Run, and fault only external-stop `Status`
  construction. After the target CPU callback enters the FIFO, an independent
  CPU Run queues a successor sentinel behind it before the worker occupant is
  released. Sentinel completion proves that the target callback was popped and
  finished abandonment. With the GPU callback still held, the target future
  remains incomplete; releasing that callback returns the selected code with
  an empty diagnostic. Both cancellation and stale cases retire every callback,
  and the same context succeeds with the current snapshot. A separate no-sleep
  queued-attempt regression occupies the sole CPU worker and holds the target
  Run immediately after its post-submit external-stop observation, while the
  Run mutex remains held and before condition-variable waiting. This proves the
  single-node side-effecting/non-cacheable target is already in the CPU FIFO
  after the coordinator's last check. Graph replacement followed by Run-gate
  and occupant release must return `Stale` with zero target entries; cancelling
  and replacing at the same boundary must return `Cancelled` with zero entries.
  Both futures converge and the same execution context runs a newly compiled
  current plan successfully;
- bounded ready work and `ResourceLedger` settlement;
- cross-backend copy/backend labels, cancellation, stale completion, and
  exception fences. A private condition-variable barrier holds a nontrivial
  vector result only after named Values, diagnostics, digests, and execute
  timing are complete; post-hook cancellation, graph replacement, and both
  return respectively `Cancelled`, `Stale`, and `Cancelled`, publish no
  `ExecutionResult`, and each is followed by a healthy execute proving exact
  cleanup;
- Value/Region/strided-layout/facet/buffer negative contracts;
- operation/provider ABI version/size/alignment/pointer/count/bounds/lifetime,
  including C operation ABI 9 typed parameter schemas, demand views, and
  deterministic owner-allocation failure with exact destroy/close counts. A
  copy-aware C++ embedding callable is registered by rvalue, armed to reject
  later copies, then survives freeze/invoke and a valid DSO load into an
  unfrozen registry with one invocation and no increased copy count. This
  proves registry/map/staging snapshots copy only immutable owning handles and
  keep DSO leases exact. A callback that throws `std::runtime_error` or a
  standard exception with null `what()` is fenced as `OperationFailed` with the
  exact or empty message, while `std::bad_alloc` remains observable by the
  embedding caller and input/parameter/output classification stays unchanged;
- operation-invocation prevalidation with a default-invalid Value in the first
  and final input position, a successful callback returning a default-invalid
  output, and an unknown backend against both C++ and a real GPU-capable DSO.
  Invalid inputs and unknown backends return `InvalidArgument` without a
  callback or CPU/GPU DSO counter increment; a known unsupported GPU remains
  `BackendUnavailable`. Preserve output-type conflicts and Match input
  type/shape conflicts return `TypeMismatch` before deliberately failing,
  side-effecting callbacks can run, while callback output `Value{}` remains
  post-entry `TypeMismatch`;
- exact operation/provider library path validation before native loading: an
  explicit-length valid fixture path followed by embedded NUL and suffix is
  `InvalidArgument`, publishes no operation key/provider schema, and reaches
  none of the owner-allocation, native-load, or native-close test hooks;
- real-DSO output-sink at-most-once enforcement: accepted-then-duplicate and
  rejected-then-duplicate callbacks record exact sink returns `(1,0)` and
  `(0,0)`, while success, backend unavailable, ordinary failure, and unknown
  callback results plus callback-reported cancellation all become the same
  stable terminal `OperationFailed`.
  Duplicate backend unavailability performs zero CPU invocations and publishes
  no output; a deterministic callback-held cancellation case proves host
  cancellation remains higher priority. A null sink context has no side effect,
  the following valid publication succeeds, and registry retirement still
  destroys descriptors and closes the native lease exactly once;
- real-DSO dense fixed-shape boundaries at `INT64_MAX + 1` bytes and rank-two
  `{2, 2^62}`, rejection immediately above both limits, 32-bit host-size
  representability through a compile-safe helper, and transactional
  multi-descriptor rejection with exact destroy/close counts;
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
  execution failures remain samples and later iterations continue. An
  ordinary oracle exception with null `what()` becomes an `OperationFailed`
  sample with an empty reason: its completed raw compilation/execution
  diagnostics stay intact and a later iteration can still succeed. A separate
  null-diagnostic cancellation case proves the post-oracle cancellation fence
  remains top-level authoritative. Duration
  assertions permit zero because the monotonic clock may have microsecond
  resolution. Deterministic regressions cover an oracle barrier cancelled by
  another thread, self-cancelling true/false/throwing oracles, a two-iteration
  null-diagnostic-then-success sequence with exact diagnostic-field checks,
  and the no-oracle post-execute window through the noninstalled test-kernel
  seam.

Daemon tests live in `photospider-daemon` and cover local frame validation,
local method routing, ephemeral Session/Job lifecycle, restart loss,
multi-Session behavior, cancellation, Session close, result release, shutdown,
and the isolated installed-kernel boundary.

## Installed boundary

The package gate configures and installs Photospider to a fresh prefix, then
configures an external C/C++ consumer using only
`find_package(Photospider CONFIG REQUIRED)`. CI runs this gate for both the
default static kernel and `BUILD_SHARED_LIBS=ON`. It verifies:

- installed public headers compile through the consumer sources;
  `operation_plugin.hpp` is the consumer's first include with compile-time
  `element_type_value` and `noexcept` assertions. Exhaustive header
  self-containment is a separate manual check, not this CTest;
- the linked C SDK compilation unit runs, and a downstream shared bridge links
  `Photospider::kernel`, executes the C++ compile/execute pipeline, and is
  called by the consumer executable; the default static archive is therefore
  exercised as position-independent input to a real shared library;
- `kernel`, `operation_sdk`, and `data_provider_sdk` component discovery
  exports exactly `Photospider::kernel`, `Photospider::operation_sdk`, and
  `Photospider::data_provider_sdk`; the SDK targets are header-only. Configure-
  time property checks require both the kernel and operation SDK targets to
  carry `cxx_std_17`, require the pure-C provider target not to carry that C++
  feature, and reject any stray `data_definition_sdk` target;
- the downstream bridge and final executable declare only
  `CXX_STANDARD=14`, `CXX_STANDARD_REQUIRED=ON`, and `CXX_EXTENSIONS=OFF` on
  themselves. Neither target privately requests C++17. The linked imported
  kernel and operation SDK usage requirements raise their actual compilation
  to C++17. Both translation units enforce that result with a compiler-
  appropriate static assertion: `_MSVC_LANG` when that macro is defined by an
  MSVC-compatible frontend, and `__cplusplus` otherwise. The consumer adds no
  private `/std:c++17` flag and does not require `/Zc:__cplusplus`; the imported
  usage requirements remain the source of dialect elevation. The linked pure-C
  SDK probe remains an explicit C11 translation unit and receives no C++
  standard flag;
- the removed `data_definition_sdk` target is not exported.

The nested consumer project exposes a generator-aware
`run_photospider_consumer` target whose command uses its executable target-file
expression. The outer gate passes its exact generator, platform/toolset when
present, and active configuration, then builds that run target. Single-config
and multi-config layouts therefore require no guessed build root, configuration
directory, executable suffix, or bundle path. The outer gate also passes the
closed producer sanitizer mode `none`, `address`, or `thread`. A sanitized
nested project applies matching compile instrumentation to its C SDK object,
shared bridge, and final executable and matching link instrumentation to both
linked products. The final executable therefore owns the sanitizer runtime
even though the static kernel is first embedded in a private downstream shared
bridge. An ordinary consumer receives no sanitizer option, and installed
`PhotospiderTargets.cmake` never contains a sanitizer flag.

Daemon validation must use that isolated prefix, never a sibling checkout or
private include directory.

The deterministic scheduler tests use private callback-enqueue, pre-failure,
queue-rejection, and diagnostic-construction hooks compiled only into the
noninstalled `photospider_test_kernel`. They expose the otherwise unobservable
no-GPU/admission/submit linearization windows and the allocation-free exception
fallback. The construction hook selects the exact exception-fence or run-loop
external-stop materialization point and fires immediately before owned
diagnostic and `Status` construction. External-stop fallback runs under the
already-held Run mutex without decrementing an in-flight slot; the exception
helper retains its callback-retirement ownership. The callback-enqueue observer
also records the target and successor admissions while the sole CPU worker is
occupied; waiting for the successor Run proves FIFO retirement before the held
GPU callback is released. The regression also feeds a null standard-exception
diagnostic through the pointer-only call boundary; normal completion proves the
failure is fenced, the in-flight count drains, and the empty-message fallback
remains usable. With `BUILD_TESTING=ON`, the product archive, installed kernel,
exports, and ordinary consumer remain hook-free; with `BUILD_TESTING=OFF`,
neither the test-kernel target nor its execution-hook object exists.

The same noninstalled execution-hook object exposes one no-throw
`final_result_ready` observer after complete local result assembly and before
the final stop check. It exists only to hold the success-publication
linearization window, plus one no-throw post-submit observer after the
coordinator reacquires the Run mutex and checks external stop but before it can
wait. The latter proves queued worker-entry admission without adding a
production notification path. The product archive and package contain neither
observer symbol nor test string. Native-library path regressions similarly
count loader, owner, and close boundaries only through the noninstalled
test-kernel hook object.

## Sanitizers and malformed-input validation

ASAN and TSAN are mutually exclusive scoped CMake modes where the C++ compiler
and linker support the requested instrumentation. Configuration fails instead
of silently producing an uninstrumented target when that support is absent.
Sanitizer compile and link options are private to the kernel product and are
published only through its build-tree interface so every in-tree executable is
closed over the runtime. The complete sanitizer CTest inventory retains the
installed-consumer gate, whose explicitly matching nested mode tests the
installed static/shared-bridge/final-executable topology without leaking
instrumentation into the installed package export. The ordinary tests exercise
malformed Value/Region/layout, graph documents, operation/provider records and
exact library paths, and callback outputs. Malformed local IPC frames belong to
the daemon repository.

The long-lived manual target `photospider_operation_contract_ir_fuzz` exercises
the current OperationTraits trait/parameter vocabulary and compiler validation. It is
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

CTest entries validate observable correctness, including numerical results,
resource bounds, multithreading, error handling, package consumption, compilation
and runtime boundaries. Performance measurements and timing-dependent diagnostics
remain opt-in tools; elapsed-time thresholds do not define correctness. Do not register stale-term searches, source-layout audits,
migration checklists, Doxygen audits, Issue replay, or result/provenance
orchestration. Manual source-quality tools require maintained English and
Chinese documentation and remain outside CTest/CI. Direct source-tree and
installed-tree header self-containment scans with Clang and GCC are such manual
checks; they are not registered with CTest or CI.

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

## Current registered correctness workflows

Registration names describe behavior, not historical G1/G4/S1/S4 milestones.
Use `ctest --test-dir <build> --show-only=json-v1` for the actual inventory of
that configuration. The maintained default has 78 entries; enabling the
supported SME build adds `test_numeric_conversion_sme`. All registered
executables, including the three numeric/expression/filter workflows, are
built by the default testing build.

- `test_workflow_numeric_reductions`, `test_workflow_expression_lut` and
  `test_workflow_filter_histogram` run public generic numerical oracles.
- `test_dependency_workflow` checks exact sparse reads, progressive control
  discovery, dynamic radius, demand replacement, waiter sharing, content cache,
  ordered reductions/scans and block-state reconvergence. Its former
  generic-image STMap positive fixture and timing mode are retired; planar
  STMap execution has not been implemented.
- `test_planar_image_workflow`, `test_planar_plugin`, `test_planar_import`,
  `test_planar_preparation`, `test_region_runs` and `test_data_movement_contract`
  cover current planar storage, C ABI 9 plugins, preparation seals, mapped ROI,
  raw bits, layout, ownership and publication boundaries.
- `test_gpu_fragment_execution`, `test_gpu_sync_fallback`,
  `test_gpu_discovery_workflow` and `test_gpu_c_abi_execution` exercise actual
  native dispatch, sparse atlases, restart/fallback, discovery and C services.
  `test_native_gpu`, `test_native_execution` and `test_fragment_atlas_gpu`
  complement them. No hardware returns 77 and is recorded as skipped.
  `test_dependency_gpu` and `test_gpu_discovery` retain CPU-runnable protocol
  validation. Protocol violations detected before cancellation remain Protocol
  failures; ordinary callback failures can be superseded by cancellation.
- `test_numeric_conversion_sme` checks bits and deterministic cancellation.
  Missing SME hardware or an unsupported streaming vector length returns 77.
  Its `--midflight` timing diagnostic is not registered with CTest.

The removed-format-key checklist is retired. `test_compiler` instead checks
`NotFound` for one synthetic unknown operation through lookup, invocation and
compilation. Current ABI/schema/package-version rejection, malformed input,
integer overflow, quality mathematics and owner-retirement tests remain
correctness tests; they are not migration checklists.

```sh
cmake --build <build> -j 8
ctest --test-dir <build> --output-on-failure
ctest --test-dir <build> -R '^test_(dependency_workflow|gpu_fragment_execution|gpu_sync_fallback|gpu_discovery_workflow|gpu_c_abi_execution)$' --output-on-failure
```

## Installed and optional coverage boundaries

The installed gate accepts package 0.24 and rejects a 0.23 request. Its actual
runtime inventory is the `COMMAND` list of `run_photospider_consumer` in
`tests/consumer/CMakeLists.txt`, including region runs, planar preparations,
mapped movement and maintained operator/registry workflows. The four GPU
workflow targets and foundations example are currently build dependencies only
in that nested gate, not installed runtime executions. Their in-tree CTests
remain the native execution coverage. Resource, Result, representation and
legacy multi-output example targets outside that run target are optional
consumers and are not evidence of executed installed coverage.

Old image-vertical and S3/S4 examples are unregistered migration sources. They
must not be listed as current passing gates. Their former `test_bindings`,
`test_image_vertical*`, `test_metal_images`, `test_native_cache` and `test_s4_*`
entries are absent from the current inventory. Maintained images use the planar
correctness entries above; old generic-image acceptance is not restored.

For a custom compiler/runtime, propagate the same `CC`/`CXX` environment to
CTest: the installed gate configures a fresh nested consumer. On Apple Silicon,
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` can check native dispatch. Unsupported
GPU/sanitizer capability is a limitation, not a successful test.
