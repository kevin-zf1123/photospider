# Testing and Validation

This document defines repository-level testing and validation behavior. It is
development guidance rather than a description of kernel runtime architecture.

## Mainline macOS Architecture

Mainline macOS development targets Apple Silicon `arm64`.

The project does not intend to preserve mainline `x86_64` macOS support. If
future users need `x86_64`, it can be handled by a branch, fork, or dedicated
compatibility effort.

On Apple Silicon, the compiler target, terminal architecture, and dependency
architecture should all agree on `arm64`. Architecture mismatch between an
`x86_64` build and `arm64` Homebrew libraries is not a supported mainline setup.

## Build Configuration Direction

Developer setup should make architecture selection explicit. CMake presets or
bootstrap notes should default macOS to `arm64`.

The repo should also document or provide:

- `compile_commands.json` generation
- lint and formatting commands
- the expected local validation command set

The root CMake configuration exports `compile_commands.json` and defaults
mainline macOS builds to `arm64` when no `CMAKE_OSX_ARCHITECTURES` value is
provided.

On macOS, each install-consumer smoke reads the selected producer's resolved
`CMAKE_OSX_ARCHITECTURES` cache value and passes the exact meaningful value to
every external CMake configure as one argument. Semicolon-separated universal
architecture lists therefore remain intact, and the producer, installed static
archives, and all consumers stay on one architecture profile even when a
Rosetta-launched outer runner would choose another compiler default. This
propagation is Darwin-only; Linux and Windows children never receive the
macOS-specific option. It does not create or preserve a supported mainline
`x86_64` path.

The declared CMake 3.16 minimum is a compatibility floor for the installable
static product's producer path and downstream package consumption; it is not a
fixed toolchain that every pull request must run. Any policy introduced after
that floor, such as `CMP0135`, must be guarded with `if(POLICY <policy>)`.
Compatibility is maintained by that policy guard, the current GitHub
integration package consumer, and a targeted native old-version run only when
a compatibility-sensitive change or release check warrants one.

When a targeted minimum-version run is performed, it starts from a fresh producer
build tree, configures the top-level project with CMake 3.16 and
`BUILD_TESTING=OFF`, builds the real `photospider` target, installs to a fresh
prefix, and only then configures, builds, and runs an external
`find_package(Photospider)` consumer. It must not reuse a producer tree
configured by a newer CMake or substitute an internal helper target. If no
natively compatible old CMake runtime is available locally, skip that targeted
local run; architecture emulation is not required.

The package-consumer smoke recreates its transient install, consumer source,
and consumer build directories without suppressing cleanup failures. It checks
the observed producer/install/consumer behavior in memory and streams commands,
child output, and assertion diagnostics to stdout/stderr for CTest to capture.
All generated files remain in its transient work directory and are discarded
after the run; the repository does not retain per-run reports for this test.

`BUILD_TESTING` controls availability of internal test products, not how the
installed `photospider` archive compiles the Issue #72 observation seams. The
product source inventory is divided into common objects, compiled once, and
production objects for `graph_cache_service.cpp`,
`graph_state_executor.cpp`, and `kernel_compute.cpp`. The real archive always
uses the production form of those three translation units, with no
`PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING`,
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING`, or
`PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING` declarations, globals, branches,
or symbols. Focused tests link a non-installed
`photospider_internal_test_product` that reuses the same common objects and
recompiles only those three translation units with the deterministic seams.
No target links both complete archives, and the test product is absent from
install and export sets.

`StaticProductConsumerSmoke` enforces that boundary for both
`BUILD_TESTING=ON` and `BUILD_TESTING=OFF` producer configurations. After the
real product is installed, Darwin first invokes and validates
`xcrun --find llvm-nm`, then falls back to PATH `llvm-nm` and PATH `nm`;
non-Darwin platforms never invoke `xcrun` and use the two PATH candidates in
that order. Canonically identical executable paths run once. A candidate is
usable only when it starts, exits successfully, emits symbols, and exposes
defined anchors from all three production seam objects. Otherwise the smoke
records a path-free failure reason and tries the next candidate; no candidate
or all unusable candidates fail closed. The first usable full symbol table is
authoritative and rejects every hook function/helper/global fragment. The
raw table is used only in memory for that decision; a forbidden symbol in the
first usable table fails the verdict without trying a later candidate. The
retained scan observation has a closed, path-free schema: stable `tool_source`,
ordered structured attempt reasons, status and aggregate line/anchor/prohibited
counts, plus counts keyed only by controlled symbol tokens. It retains no tool,
archive, object, build, install, or workspace path; no raw symbol line or
captured stdout/stderr; and no environment `PATH`. If aggregate package behavior
fails, the JSON diagnostic is a whitelist projection of failed check labels,
command statuses, and that sanitized scan observation rather than the complete
transient observations. The smoke also rejects an installed test product
archive, exported test target, or exported internal seam definition. This
remains a labelled `build-smoke`; ordinary complete CTest selection does not
make package construction part of runtime-test ownership.

The smoke inspects every installed `Photospider*Targets*.cmake` file because
the package separates base, OpenCV-dependent, and embedded-product targets
into distinct export sets. Its dependency classifier recognizes only the exact
OpenCV component target spellings that the producer accepts: bare lowercase
names, lowercase `OpenCV::opencv_*` targets, and component-specific CamelCase
targets such as `OpenCV::Core`; partial-name matches remain rejected. This is
validated through the real exported package/consumer behavior rather than a
synthetic verifier self-test. With OpenCV discovery disabled, a consumer
requesting `COMPONENTS operation_sdk OPTIONAL_COMPONENTS operation_opencv`
must keep the package and `operation_sdk` found, mark `operation_opencv` not
found, import the dependency-free SDK/runtime targets, and omit
`Photospider::operation_opencv`. Requiring `operation_opencv` under the same
condition must fail package discovery. With OpenCV available, the adapter
consumer imports that target through only the OpenCV `core` component and does
not discover unrelated packages.

When IPC is enabled, the package smoke builds and installs `photospider`,
`photospider_ipc_client`, and `photospiderd`. It independently configures a
default embedded consumer of `Photospider::photospider` and an IPC-only project
that requests `COMPONENTS ipc_client`, disables OpenCV/`yaml-cpp` discovery,
and links only `Photospider::photospider_ipc_client`. The latter therefore
resolves only Threads and does not inherit the backend or JSON implementation
target. That IPC-only consumer includes the installed protocol, Client, and Host-adapter headers,
constructs `create_ipc_host()` without contacting a daemon, executes every
safe public Client lifecycle symbol, and links a reference-only branch for all
exact unique inventories of 60 typed Client calls plus all 58 non-destructor
Host virtuals. Package
inspection also requires the IPC archive and exact three-header surface,
permits only `Threads::Threads` in the exported IPC link interface, positively
allows only the current C++ standard-library and installed `photospider/`
public includes, and rejects raw JSON, socket-address/descriptor, file-identity,
file-mapping, and backend declarations. This is the gate's exact boundary, not
an exhaustive promise about every possible POSIX spelling. With backend
discovery disabled, `COMPONENTS ipc_client OPTIONAL_COMPONENTS embedded`
succeeds with only `ipc_client` found, and an unknown optional component
remains not-found without invalidating the package.

The same smoke independently configures a C11 project that requests only
`COMPONENTS policy_sdk`, builds a pure-C ABI-v1 policy DSO against
`Photospider::policy_sdk`, and rejects OpenCV, yaml-cpp, or Threads leakage.
The generated source probes the exact policy ABI constants and layouts. The
external embedded consumer then loads that installed policy DSO and an
installed operation DSO, configures policy and execution defaults, validates
their public snapshots, and computes through both extensions. No generated
consumer receives a source-tree include directory.

The durable
`IpcDisabledInstallSmoke` configures a separate clean producer with
`PHOTOSPIDER_BUILD_IPC=OFF` and `BUILD_TESTING=OFF`; it verifies that no IPC
build forwarder, installed header, archive, executable, or exported target is
advertised, a required `ipc_client` component fails discovery, and an external
default embedded Host consumer still links and runs. Required unknown package
components fail as well; optional disabled `ipc_client` and unknown components
remain not-found without failing discovery; omitting components or requesting
`embedded` retains the existing backend dependency resolution.

The durable `DependencyDisabledInstallSmoke` configures a clean producer with
OpenCV and YAML capabilities disabled, disables both package discoveries,
turns off IPC/testing, and builds the real `photospider_kernel` aggregate and
`photospider` product. It verifies the derived provider/plugin/CLI defaults and
the precise diagnostics for three invalid explicit combinations. After a clean
install it rejects OpenCV headers, targets, export references, and yaml-cpp
link leakage; optional `operation_opencv` remains unavailable while the
required component fails. An external consumer configures with both discoveries
disabled, links/runs `Photospider::photospider`, allocates a neutral image,
loads and closes an empty Host session, and observes `GraphErrc::Io` from an
explicit YAML operation. CI may reuse a producer only after its cache identity,
configuration, and complete capability profile are validated.

When the selected CMake generator exposes multiple configurations, the smoke
uses that same generator for producer and consumer, checks each
`CMAKE_GENERATOR` and `CMAKE_CONFIGURATION_TYPES` cache value, and resolves the
consumer executable from the configuration-specific `$<TARGET_FILE:...>`
manifest.

Migration residue, phase completion, stale-term, and source-layout checks are
temporary development checks. They must not be registered with CTest or CI.
Issue-specific replay, provenance, helper, and output artifacts must neither
enter the primary repository nor remain as long-lived personal-overlay
content. Long-lived runtime, public-header, and package-consumer tests own the
durable product boundaries.

## Build-Smoke CI Classification

A build smoke is a durable CTest whose primary boundary delegates to a CMake
configure/build/install, an exported-package or external-consumer build, or a
dedicated compile target. Every such test carries the exact stable CTest label
`build-smoke`. A companion that only calls the driver's Python cleanup or
layout helpers in-process remains an ordinary safety regression in the full
CTest shard.

The maintained labelled inventory is
`DependencyDisabledInstallSmoke`,
`ImageArtifactCodecDependencyDisabledBuild`,
`IpcDisabledInstallSmoke`,
`OpenCvOperationProviderDisabledBuild`,
`PublicHeaderSelfContainment`, and
`StaticProductConsumerSmoke`. `PublicHeaderSelfContainment` belongs because its
CTest command builds the dedicated self-containment target; ordinary
GoogleTest binaries, daemon/CLI process tests, and
`PhotospiderdCapabilityHelp` do not create a child build and remain in the main
CTest shard. `OpenCvOperationProviderBuildSmokeSafety` also remains there: it
is the ordinary safety regression for the OpenCV build-smoke driver and does
not itself start CMake, CTest, an install, or a compile target.
`InstallConsumerArchitecturePropagationSafety` likewise remains in the main
shard: it runs the three install-consumer drivers' real command-construction
paths against disposable producer cache fixtures while replacing subprocess
execution, so it verifies cache-to-child-argv propagation without launching a
configure, build, or install. The same process injects executable lookup,
validation, and captured-command callbacks into the static-product driver's
production archive-symbol helpers. It locks Darwin xcrun-first fallback,
non-Darwin independence, all-candidate failure, and canonical path
de-duplication without changing process PATH or replacing the real installed
archive scan. When CMake registers the safety test, it also supplies the
current build tree, CTest executable, configuration, and Python launcher. The
test queries that tree through
`ctest --show-only=json-v1` and the production inventory parser. It requires
`DependencyDisabledInstallSmoke` and `IpcDisabledInstallSmoke` exactly once in
every profile, requires `StaticProductConsumerSmoke` exactly once only when
IPC is enabled and absent otherwise, then requires every expected entry to
remain enabled and labelled and to start with the exact `python -B` driver
path. Commented or inactive CMake source cannot satisfy this
generated-inventory check because it produces no CTest entry. The inventory
query executes none of the real smokes and does not change the six-test
build-smoke classification.

CTest keeps every labelled test registered for direct local use. CI's
`full-ctest` shard excludes the exact label. Configuration planning parses
`ctest --show-only=json-v1` only as an allow-empty preflight because default
`gtest_discover_tests` entries may still be unlabelled `_NOT_BUILT`
placeholders. After the complete default build, build integrity repeats the
query in strict mode and publishes one independent matrix job per labelled
test. Adding another maintained build smoke therefore requires its CTest
registration and the same label, but no workflow test-name edit. Preflight
fails closed on malformed inventory, duplicates, invalid label shape, or
disabled/commandless labelled entries, but not on an empty selection. The
post-build authority rejects those states and an empty labelled set. Before
execution, the runner re-queries the inventory and rejects a selected name that
is absent, duplicate, disabled, commandless, or no longer labelled. After that
exact label check, it selects only the validated numeric CTest index, so
arbitrary test-name characters are not interpreted by a shell or regular
expression.

The published-image workflow fans out the strict build-integrity output after
restoring the same reusable default producer. An empty include fallback keeps
`fromJSON` well-formed when that producer job is intentionally skipped; a
successful producer cannot publish an empty strict matrix. Each CTest
registration retains its own timeout and `RUN_SERIAL` behavior; each matrix
item also has an independent workflow timeout and result artifact. The
local-image fallback reads the same post-build NUL-delimited names and executes
them sequentially because it has only one Docker-capable runner. Nested drivers
must continue to use disjoint work directories, validate any reusable producer
identity they accept, and clean up without following or deleting unrelated
symlink targets.

## Validation Ownership

Primary-repository CTest and CI entries are reserved for long-lived software
behavior: correctness, performance, stability, multithreaded execution, error
handling, compile boundaries, package consumption, and runtime API boundaries.
`PhotospiderdCapabilityHelp`, `StaticProductConsumerSmoke`,
`GraphCliOptionBadAlloc`, GoogleTest discovery, and
`PublicHeaderSelfContainment` satisfy that rule because they execute or compile
the maintained product. The daemon help test uses a CMake script driver to run
the real configuration-specific `photospiderd --help`, captures stdout and
stderr, requires a numeric zero process result before matching the stable
capability sentence, and diagnoses launch failure separately from nonzero exit.
`IpcDisabledInstallSmoke`, `DependencyDisabledInstallSmoke`, focused
`test_ipc_protocol`/`test_ipc_host` cases, and real-process `test_ipc_daemon`
cases follow the same rule: they exercise
package, framing, typed client, complete IPC Host dispatch/polling/stop/artifact
ownership, daemon lifecycle, concurrency, and cleanup behavior. Daemon tests
use CTest timeouts plus bounded
SIGTERM-to-SIGKILL-to-waitpid cleanup; they do not depend on fixed readiness
sleeps.
`StaticProductConsumerSmoke` is limited to producer configure/build/install,
external `find_package`, public-header compile/link/run, installed export and
dependency boundaries, platform archive/link behavior, and multi-configuration
target discovery. Its behavior verdict must not include Git identity, staged or
unstaged patch hashes, invocation replay, environment fingerprints, or
synthetic verifier self-tests. It uses a transient work directory and emits
commands plus assertion diagnostics directly to CTest's captured streams. A
phase name, migration-residue search, stale-term detector, source-layout
completion check, or issue replay is not a software behavior test and must not
be registered with CTest or invoked by CI.

The CLI/Host Doxygen AST tool is a long-lived manual developer tool, not a
test. Run it explicitly when the corresponding declarations, definitions,
exception contracts, or target source closures change:

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-cli-host-doxygen
```

The CLI/Host audit treats
`apps/graph_cli/src/cli_config.cpp::apply_cli_policy_execution_defaults` as the
canonical policy/execution-default definition and validates its complete
Doxygen in that translation unit. It also audits
`load_configured_policy_plugins`, `run_graph_cli`, the root CLI
resource-exhaustion policy, temporary-then-commit configuration parsing, and
the complete catalog of CLI/benchmark broad catches. Every broad catch must be
preceded on the same chain by an exact `std::bad_alloc` rethrow.

Its file may remain in the primary repository because this document defines
its lasting manual role. It must remain absent from CTest and GitHub CI. Its
`--out` directory is a disposable temporary working directory outside
the repository and must not become a retained result tree.
Issue-specific replay, provenance, helper, and output artifacts must neither
enter the primary repository nor be retained as long-lived personal-overlay
content. A clean primary clone, CMake configuration, CTest inventory, and CI
script must not depend on personal development content.

Validation is proportional. During implementation, run scoped static checks,
affected build targets, and focused regressions. A native clean configure, full
build, or complete CTest/JUnit pass is optional and should be chosen only when
the change's risk warrants it. Local workflow-source, YAML, and shell checks are
developer preflight only; they do not emulate the hosted GitHub Actions runner.
Do not use Docker or local `linux/amd64` emulation as a routine local preflight.
Current-head GitHub Actions remains the authoritative remote integration
environment.

## CLI Option-Action Validation

`test_cli_policy_execution_config` is the CTest-registered integration binary
for the reusable `run_graph_cli` option boundary plus policy/execution
configuration. Its configuration cases enforce transactional YAML/editor
parsing, the zero-through-eight execution-worker range, exact Host values, and
startup failure on Host rejection. Its option cases use a complete
deterministic Host spy and the real ordered parser. Successful load/output and
short-traversal cases preserve the
Host-returned session target and the argument-free `-t` grammar. Failure cases
require load, output, dependency-tree print, traversal-order, and all-cache
clear failures to return recoverable exit code 2 without printing the success
footer or entering the REPL. The load case also captures the REPL banner,
proving that a failed only action wins over the normal no-action fallback.

Option replay remains ordered and may expose effects from successful actions
before or after another recoverable action failure; it does not provide a
multi-action rollback transaction. The final result is nevertheless failure
when any action or loaded-graph precondition fails, and that failure wins over
an explicit `--repl`. An invocation with no option action retains normal REPL
entry. Run the focused boundary with:

```bash
cmake --build build --target test_cli_policy_execution_config -j
./build/tests/test_cli_policy_execution_config \
  --gtest_filter='CliOptionActions.*'
```

## Graph Document Error Matrix Validation

`test_graph_document_errors` is a CTest-registered integration binary for the
long-lived Graph document ingestion and save contracts. It exercises the
public embedded Host boundary and the direct `GraphModel::replace_nodes`
transaction boundary. Load/reload cases distinguish omitted source paths from
explicit source paths, require the exact `GraphErrc` category for I/O, YAML,
schema, topology, lifecycle, and unexpected failures, and prove that
`std::bad_alloc` remains an exception. They also prove failed initial loads do
not publish sessions, failed reloads preserve the complete prior Graph state,
successful replacement advances topology generation and authoritative
`GraphRevision`, resets runtime state, and keeps retry possible.

`test_host_adapter` owns the deterministic reload-versus-close lifetime
regression. A real blocking compute and three explicit Host-operation gates
prove that a reload admitted before the close marker remains admitted before
Kernel entry and after public status translation, and that close cannot finish
first. Repeating reload after that marker must return `GraphErrc::NotFound`
without entering Kernel. The companion node-YAML and forward/backward ROI
races prove reload still runs after each required lookup-and-use work item, so
the close admission correction does not weaken graph-state ordering. These
tests use event gates and zero-duration future snapshots, not timing sleeps.

The same binary owns the public save transaction regression. Its private,
destination-scoped `BUILD_TESTING` checkpoint runs on the graph-state worker
immediately before destination open. One case requires recoverable failure to
return `GraphErrc::Io`; another requires exact `std::bad_alloc`. Both require
the existing destination bytes plus the publicly inspected session and node
state to remain unchanged, then require an uninstrumented save retry to
succeed. The const GraphIO boundary and serialized owner path provide the
broader non-mutation guarantee. Production builds compile out the checkpoint
and retain the single real writer.

## Revision-Safe Compute Publication Validation

Issue #72 uses four maintained test binaries to own the long-lived staged
publication boundary. `test_compute_run` validates the checked nonzero strong
`GraphInstanceId` and `GraphRevision` values, non-reused Graph identity,
monotonic mutation revisions, and exact descriptor/snapshot provenance.
`test_compute_service_split` proves `RealtimeProxyGraph` snapshot cloning is a
deep isolation boundary and that complete prepared-state publication uses the
documented no-throw swap path.

`test_kernel_contracts` exercises the product Kernel boundary. Deterministic
event gates hold operation execution outside graph-state while clear, same-label
reload, or same-topology cache clear advances the live revision. Parallel and
sequential stale results must return `GraphErrc::ComputeError`, preserve the
newer visible state, and write no deferred cache artifact. A focused
`PHOTOSPIDER_INTERNAL_KERNEL_COMMIT_TESTING` checkpoint pauses after predicate
validation inside the graph-state item, proving mutation cannot enter between
validation and publication. The same checkpoint proves a valid RT proxy commit
remains visible when the independently validated HP sibling later becomes
stale. Together with `PHOTOSPIDER_INTERNAL_GRAPH_CACHE_TESTING` and
`PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING`, this macro is present only
in the three-translation-unit test-product variant used by
`test_kernel_contracts` and `test_host_adapter`. The installable product uses
the matching production objects even when `BUILD_TESTING=ON`.

The same binary proves the private compute-request lane serializes execution
observation/route replacement with same-Graph compute, accepted async work
survives a dropped caller future, and close drains compute-request work before
graph-state without tearing down process-owned routes. These races use explicit
gates and bounded waits, not timing sleeps. Every discovered
`test_kernel_contracts` case also has a
30-second CTest timeout.

`test_disk_cache_diagnostic_concurrency` is the separate long-lived
multithreaded fault-isolation binary. Production record/snapshot workers run
while `GraphModel` clear, clone, and staged publication repeat; another case
invokes the two-store exchange from both argument orders through an inline,
source-tree-only bridge; a deterministic allocation failure proves a throwing
snapshot copy releases the private scoped guard. Every discovered case carries
the `kernel-concurrency` label and a 20-second CTest timeout. If a lock
regression prevents worker recovery, CTest terminates that dedicated process;
neither a `std::future` destructor nor a thread join can retain the broad
kernel-contract process or wait for the CI job timeout. The sequential
`CacheSemantics.DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics`
case remains in `test_kernel_contracts` to prove failed reload preservation and
successful clear/reload reset without duplicating the deadlock probe.
Run the focused contract with:

```bash
cmake --build build \
  --target test_compute_run test_compute_service_split test_kernel_contracts \
  test_disk_cache_diagnostic_concurrency -j
./build/tests/test_compute_run \
  --gtest_filter='GraphRevision.*:ComputeRunDescriptor.CapturesIdRevisionIntentQualityAndQosWithoutReuse'
./build/tests/test_compute_service_split \
  --gtest_filter='RealtimeProxyGraph.*'
./build/tests/test_kernel_contracts \
  --gtest_filter='ComputeContracts.ParallelStaleComputeCannotOverwriteGraphClear:ComputeContracts.SequentialStaleComputeCannotOverwriteGraphClear:ComputeContracts.ReloadedDocumentRejectsOlderSameLabelCompute:ComputeContracts.SameTopologyCacheClearRejectsStaleMemoryAndDiskPublication:ComputeContracts.CommitPredicateAndPublicationExcludeMutationToctou:ComputeContracts.RealtimeCommitSurvivesStaleHighPrecisionSibling:ComputeContracts.ExecutionObservationAndReplacementWaitForCompute:ComputeContracts.CloseWaitsForAcceptedAsyncComputeRequest:ComputeContracts.DroppedAsyncFutureRemainsOwnedUntilCloseDrain:CacheSemantics.DiskCacheDiagnosticStorePreservesClearReloadAndPublicationSemantics'
ctest --test-dir build --output-on-failure \
  -R '^DiskCacheDiagnosticConcurrency\.'
```

## Cooperative Run Cancellation Validation

Issue #73 keeps cancellation coverage in maintained behavior tests rather than
an issue-specific replay tool. `test_compute_run` owns the private Run source,
stable first reason, injected monotonic deadline, terminal-before-quiescent
state, request fan-out, and cancellation/failure/commit arbitration. The same
binary exercises `ExecutionService` cancellation before active publication,
exact queued-Run purge, the dequeue/pre-callback race, non-preemptible callback
drainage, suppressed dependent re-entry, peer isolation, and exact grant/root
release. Its legacy `A -> B` case proves cancellation after A returns retires
the callback-owned and still-plan-owned units exactly once without entering B
or publishing staged output; its companion exception branch proves a later
provider failure cannot replace the already accepted cancellation.

`test_kernel_contracts` owns the product boundary. Deterministic commit hooks
prove cancellation before claim publishes no Graph/proxy/cache state and a
request after claim cannot undo successful publication. The RT/HP case keeps a
committed proxy visible when the HP sibling later becomes stale, while the
sequential case proves provider return observes cancellation before staged
publication, and the close case proves a logically cancelled request still
drains its running provider and public `ComputeError` translation before Graph
destruction. `test_compute_service_split` cancels from connected preflight on
the private `serial_debug` route and proves that dirty HP and paired HP/RT requests enter
neither the parameter dependent nor phase-two target work.

Public non-expansion remains part of existing durable contracts:
`test_ipc_protocol` locks the exact 60-method protocol-v2 inventory, rejects
`compute.cancel`, round-trips every version-two status label, and requires
`cancellable: false`; `test_compute_request_registry` locks the daemon job
snapshot; `test_policy_registry` locks transactional ABI-v1 load rejection,
binding-held DSO lifetime, and first-fault stability; and
`StaticProductConsumerSmoke` compiles and runs the installed 58-virtual Host,
60-call Client, operation ABI v2, and pure-C policy ABI v1 consumers. These
tests must not gain a compatibility cancellation shim for this private change.

Run the focused cancellation boundary with:

```bash
cmake --build build \
  --target test_compute_run test_compute_service_split \
  test_kernel_contracts test_ipc_protocol test_compute_request_registry \
  test_policy_registry -j
./build/tests/test_compute_run \
  --gtest_filter='ComputeRunCancellation.*:ComputeRunCommitArbiter.LinearizesCancellationBeforeOrAfterCommitClaim:ExecutionServiceCancellation.*'
./build/tests/test_compute_service_split \
  --gtest_filter='ComputeServiceCancellation.ConnectedPreflightCancellationSuppressesDirtyAndSiblingPublication'
./build/tests/test_kernel_contracts \
  --gtest_filter='ComputeContracts.SequentialCancellationAfterProviderReturnSuppressesPublication:ComputeContracts.CancellationBeforeCommitClaimSuppressesPublication:ComputeContracts.CancellationAfterCommitClaimPreservesPublication:ComputeContracts.RealtimeCommitSurvivesStaleHighPrecisionSibling:ComputeContracts.CancelledComputeStillDrainsBeforeGraphClose'
./build/tests/test_ipc_protocol \
  --gtest_filter='ProtocolContract.AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods:EnumCodec.RoundTripsEveryDefinedVersionTwoLabel:HostRoutedGraphStateProtocolTest.ComputeLifecyclePreservesEveryTypedHostRequestFieldAndStableShapes'
./build/tests/test_compute_request_registry \
  --gtest_filter='ComputeRequestRegistrySubmission.PublishesQueuedCommitSnapshot'
./build/tests/test_policy_registry
```

## Latest-Wins Supersession Validation

Issue #74 keeps latest-wins and realtime-group coverage in maintained behavior
tests. `test_compute_supersession` owns canonical absent/explicit HP key
equality, checked nonzero generation overflow, exact 64-total compute-lane
admission, persistent ticket FIFO/wake behavior, concurrent same-key ticket
adoption, cross-target/intent/Graph isolation, close retirement, deterministic
18,000- and 36,000-publication storms, and `RunGroup` cancellation/aggregate
rules. The group cases distinguish a request-level accepted reason from a
reason that actually wins an open child arbiter: late Superseded or
ExplicitRequest after two child successes cannot replace aggregate success,
while a winning cancellation retains the first reason below failure priority.
CMake discovers all 15 cases through CTest with a 60-second per-case timeout.
The stress cases assert one ticket, one logical active owner, at most one
pending owner, exact displaced settlement, and only the final current
generation remaining commit-eligible; they do not create a background runner
or rely on timing sleeps.

`test_kernel_contracts` owns the product boundary. It proves missing intent and
explicit HP share one key, failed newest work does not resurrect an older
prepared commit, already committed older output remains visible, and realtime
supersession on both sides of RT publication denies the old HP sibling while
preserving a valid old proxy. A post-commit checkpoint additionally blocks the
old realtime caller after both child successes and visible publication but
before group aggregation; newer generation publication records Superseded
without changing that old caller's success. `test_compute_run` covers immutable
supersession identity and child-local versus group-wide cancellation. Existing
`test_compute_service_split`, `test_host_adapter`, and
`test_bad_alloc_boundaries` remain focused regression companions for service,
Host lifecycle, and allocation-failure boundaries.

Run the focused supersession boundary with:

```bash
cmake --build build \
  --target test_compute_supersession test_kernel_contracts test_compute_run \
  test_compute_service_split test_host_adapter test_bad_alloc_boundaries -j
./build/tests/test_compute_supersession
./build/tests/test_kernel_contracts
./build/tests/test_compute_run
./build/tests/test_compute_service_split
./build/tests/test_host_adapter
./build/tests/test_bad_alloc_boundaries
ctest --test-dir build --output-on-failure \
  -R '^(SupersessionIdentity|GraphStateExecutorContinuation|ComputeRequestCoordinator|ComputeRequestCoordinatorStorm|RunGroup)\.'
```

## Policy Generation and Private Execution Validation

Issue #75 keeps policy-generation and private-route coverage in maintained
behavior tests. `test_policy_registry` owns the exact built-ins and class
support, transactional rejection of a missing API or mismatched ABI, active
binding/DSO lifetime across registry unload, and first-fault stability for one
binding generation. `test_resource_admission` owns the exact closed
`cpu`/`gpu_pipeline`/`serial_debug` route vocabulary, worker-limit rollback,
one fixed pool per Host composition, and validation-first session route
replacement. The `ExecutionServicePolicy.*` cases in `test_compute_run`
continue to own Host-authored cost, class/frontier/fairness, aging, headroom,
three-to-one progress, dependent re-entry, saturation, and exact grant release
through reserved start.

`test_cli_policy_execution_config` locks transactional policy/execution config
parsing and exact Host application. `test_host_adapter` loads real operation
ABI-v2 and pure-C policy ABI-v1 fixtures, configures both extensions, validates
their snapshots, and computes through the private CPU route.
`GraphCliPluginComputeSmoke` repeats that vertical slice through the real REPL.
`test_ipc_protocol` and `test_ipc_daemon` own protocol-v2 routing, process-owned
policy state, generation-changing replacement, scan, and shared execution
defaults. `StaticProductConsumerSmoke` independently builds the installed C11
policy DSO and C++ operation DSO before executing the same external-consumer
path.

The installed Host, CLI, and IPC protocol-v2 surfaces still expose no
cancellation command. IPC continues to reject `compute.cancel` and publish
`cancellable: false`; supersession remains a private embedded-kernel behavior,
not a new public control surface. The worker-owning scheduler ABI has no
compatibility consumer.

Run the focused policy/execution boundary with:

```bash
cmake --build build \
  --target test_policy_registry test_resource_admission \
  test_cli_policy_execution_config test_host_adapter test_ipc_protocol \
  test_ipc_daemon graph_cli -j
./build/tests/test_policy_registry
./build/tests/test_resource_admission
./build/tests/test_cli_policy_execution_config \
  --gtest_filter='CliPolicyExecutionConfigParsing.*:CliPolicyExecutionConfigApply.*'
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.PolicyScanAndOperationPluginUseStatusValues:EmbeddedHostAdapter.ExternalOperationAndPolicyPluginsDriveParallelCompute'
./build/tests/test_ipc_protocol \
  --gtest_filter='ProtocolContract.AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods:HostRoutedGraphStateProtocolTest.PolicyAndExecution*:ClientExecutionDefaults.*'
./build/tests/test_ipc_daemon \
  --gtest_filter='IpcDaemonExecution.*:IpcDaemonPolicy.*'
ctest --test-dir build --output-on-failure \
  -R '^(GraphCliPluginComputeSmoke|StaticProductConsumerSmoke)$'
```

Focused companion regressions own the remaining boundaries:

- `test_kernel_contracts` drives the real `GraphIOService` stream through
  post-write, post-flush, and post-close failure states. Each phase must return
  `GraphErrc::Io`, and the created destination demonstrates the documented
  non-atomic post-open behavior.
- `test_resource_ledger` proves checked vector arithmetic, independent
  saturation and exact recovery for all five current dimensions, atomic
  mixed-vector and pair admission, bounded child grants, deferred parent
  release, move-only token contracts, and concurrent no-overcommit behavior.
- `test_resource_admission` proves the exact private-route vocabulary,
  worker-limit rollback, one fixed pool per Host with independent Host
  compositions, and validation-first session route replacement that preserves
  the previous copied route after an invalid candidate.
- `test_compute_run` records complete action/node/worker/epoch tuples. It proves
  two concurrent Runs that reuse local task id zero deliver only matching
  Run/node epochs to their separate Hosts; cleanup releases a blocked first Run
  on every assertion path so a serialization regression terminates as a test
  failure. Realtime Full HP and Interactive RT children share one physical Host
  and local task id zero, but distinct trace-node markers map each Host event
  to the matching epoch and callback-retained descriptor/task identity.
  This realtime case intentionally exercises `ExecutionService` directly:
  worker-loop Host/epoch selection and retained callback identity are observable
  at that boundary without adding a test-only GraphRuntime hook. Direct service
  cases also cover whole-vector rejection and recovery for retained Host
  memory, scratch, ready entries, and ready bytes; checked-overflow rejection;
  shared CPU admission across concurrent Runs; initial ready-store backpressure
  and priority ordering; dependent re-entry backpressure; and exact root
  release after success or failure.
- `test_ipc_protocol` proves exact Graph status propagation, one-call mutation
  behavior, and daemon session-name rollback after failed load.
- `test_ipc_daemon` proves the real transport returns save `NotFound` and `Io`
  exactly, leaves the remotely owned graph inspectable after destination
  failure, and accepts a subsequent successful save.

Run the focused validation with:

```bash
cmake --build build --target test_graph_document_errors test_host_adapter \
  test_kernel_contracts test_resource_ledger test_resource_admission \
  test_compute_run test_ipc_protocol test_ipc_daemon -j
./build/tests/test_graph_document_errors
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.*Reload*'
./build/tests/test_kernel_contracts \
  --gtest_filter='GraphIoContract.Save*'
./build/tests/test_resource_ledger
./build/tests/test_resource_admission \
  --gtest_filter='EmbeddedHostExecutionConfiguration.*'
./build/tests/test_compute_run \
  --gtest_filter='ExecutionService.*'
./build/tests/test_ipc_protocol \
  --gtest_filter=ProtocolGraphLoad.FailedHostLoadReleasesNameForRetry
./build/tests/test_ipc_daemon \
  --gtest_filter=IpcDaemonGraphLifecycle.PersistsAcrossClientsAndInspectsCopiedSnapshots
```

These are maintained product-behavior tests. No migration-residue scan,
issue-specific replay script, or retained result artifact belongs to this
validation surface.

The maintained scripted CLI integration check in
`ci/scripts/graph_cli_script_test.sh` owns the corresponding REPL boundary.
Its explicit-missing-source case requires a load failure, an empty `graphs`
inventory, and no current Graph. Its invalid-target case first loads the
maintained propagation fixture before requiring target rejection, so it does
not depend on a failed load publishing state. Each case uses isolated temporary
session and history storage that is removed when the script exits.

## Injected Image Artifact Codec Validation

`test_kernel_contracts` owns the long-lived fake-codec cache boundary. Its
`CacheSemantics.InjectedCodec*` cases create `GraphCacheService` or a real
`Kernel` with a shared `FakeImageArtifactCodec` and verify exact decode/encode
paths, service-retained codec lifetime, `int16` precision selection, recoverable
`GraphErrc::Io` diagnostics without HP-cache mutation, and exact
`std::bad_alloc` propagation. The Kernel-lifetime case blocks the real
`GraphStateExecutor`, admits a second cache-save work item that borrows
`Kernel::cache_service_`, releases the caller's only codec owner, and destroys
Kernel on another thread. Executor checkpoints and futures require destruction
to wait, the admitted encode to observe a live codec, and codec release to occur
only after Kernel destruction completes. The fake performs no real image-format
IO, so these tests remain independent of OpenCV codec behavior while exercising
the production runtime and cache service.

`ImageArtifactCodecDependencyDisabledBuild` configures a fresh nested build with
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF` and `PHOTOSPIDER_BUILD_IPC=OFF`,
builds the provider-independent focused `test_kernel_contracts` target, and
runs only the injected-codec cases. The target remains available without
registering the complete kernel-contract binary in that profile's CTest
inventory. This proves the Graph/cache injection contract and fake do not
depend on the optional operation provider. The separate
`DependencyDisabledInstallSmoke` covers the complete product profile that omits
OpenCV discovery and selects the unavailable production codec.

Run the focused validation with:

```bash
cmake --build build --target test_kernel_contracts -j 2
./build/tests/test_kernel_contracts \
  --gtest_filter='CacheSemantics.InjectedCodec*'
ctest --test-dir build --output-on-failure \
  -R '^ImageArtifactCodecDependencyDisabledBuild$' -j 2
```

## Optional OpenCV Operation Provider Validation

`test_optional_opencv_operation_provider` is a CTest-registered integration
binary built against both provider configurations. In the normal configuration
it seeds the repository OpenCV provider, executes its real resize callback,
proves an invalid OpenCV matrix shape is translated to host-owned
`GraphErrc::ComputeError`, loads a stdlib-only v2 provider that takes complete
ownership of the resize execution/dirty/forward slots, executes the replacement
sentinel output, unloads it, and executes the restored OpenCV predecessor.

`test_opencv_operation_provider_exceptions` runs in its own process so the
first provider initialization attempt is deterministic. A private
`BUILD_TESTING` hook injects one `cv::Exception` inside the real
`std::call_once` body before `cv::setNumThreads(1)`: the first registration must
return host-owned `GraphErrc::ComputeError` without publishing callbacks, and
the next registration must retry, set the OpenCV thread count to one, and
publish the provider. The same private, uninstalled test-access boundary drives
the actual monolithic and tiled exception wrappers directly. Two independent
`cv::Error::StsNoMem` injections must each emerge as an exact, fresh
`std::bad_alloc`, while a tiled non-exhaustion failure must emerge as
`GraphErrc::ComputeError`; no test attempts real memory exhaustion or changes
the public ABI.

`OpenCvOperationProviderDisabledBuild` configures a transient nested build with
`BUILD_TESTING=ON` and
`PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER=OFF`, while OpenCV, YAML, graph
CLI, and operation-plugin defaults remain enabled. The provider-aware broad
suite gate is therefore off. The driver validates the exact CMake cache
profile, builds the provider-independent focused provider binary, its
stdlib-only fixture, and the dedicated disk-cache diagnostic concurrency binary,
then queries the machine-readable CTest inventory. That inventory must contain
exactly `DependencyDisabledInstallSmoke`,
`OptionalOpenCvOperationProvider.ReplacementExecutesAndRestores`, and the three
`DiskCacheDiagnosticConcurrency.*` cases; every concurrency case must retain
only the `kernel-concurrency` label and its 20-second timeout. No broad
provider-dependent test may remain registered. The driver runs the optional
provider case and all three concurrency cases through CTest. The disabled
profile requires dependency-neutral
analyzer/math operations to remain seeded, OpenCV-backed operation keys to be
absent, and the replacement provider to publish, execute, and fully retire its
resize key. The transient build is a long-lived product configuration check;
it emits commands/results to CTest and retains no per-run report. This stage
disables the operation provider, not the separate OpenCV codec, normalization,
adapter, or embedded-product dependencies.

Before removing its transient tree, the nested-build driver derives an
absolute work spelling without resolving it for deletion. It rejects parent
traversal, the repository, every repository ancestor, filesystem roots, and
every symlink in the final work path or an existing parent component. Canonical
resolution is used only for protected-location comparison; the same checks are
repeated immediately before recursive removal, which always receives the
validated absolute spelling rather than a symlink target. Recursive-removal
failures propagate, and an lstat-style postcondition verifies that no directory
or dangling link remains.
`OpenCvOperationProviderBuildSmokeSafety` exercises those destructive guards,
failure propagation, and postcondition only against a synthetic repository,
ancestors, and unrelated symlink targets under a disposable temporary root.
Its final-symlink and symlinked-parent cases require each unrelated target and
marker to survive; the test never passes the real checkout or its parents to
the remover. The driver also reads the nested
`CMakeCache.txt`: a nonempty `CMAKE_CONFIGURATION_TYPES` selects
`tests/<config>/`, while a single-config cache must contain the exact requested
`CMAKE_BUILD_TYPE`. Missing or contradictory cache state fails explicitly, and
the safety regression covers both layouts independently of the host platform.
It is a fast ordinary full-CTest regression that imports and invokes the
driver's helpers in-process; only `OpenCvOperationProviderDisabledBuild`
launches the child configure/build/CTest profile and carries `build-smoke`.

## OpenCV Operation Concurrency Validation

`test_opencv_operation_concurrency` is a CTest-registered integration binary
for the long-lived operation-provider and benchmark-worker contracts. It uses
Host-boundary records and bounded callback gates rather than elapsed-time
thresholds:

- `BenchmarkAutoThreadsPublishResolvedGrantToHost` proves that automatic
  selection is resolved once before Host configuration, publishes a nonzero
  grant before Graph load, and reports that identical grant without repeating
  hardware detection in the verdict.
- `BenchmarkThreadsConfigureExactHostExecutionWorkers` runs the real
  `BenchmarkService`, Host execution configuration, Graph load, and registered
  callback path for automatic and explicit `1/2/4/8` requests. It requires the
  exact resolved number of callbacks and rejects a grant-plus-one callback.
- `BenchmarkThreadsRejectOutOfDomainValuesBeforeGraphLoad` requires signed
  negative and above-eight worker requests to fail before publishing a Graph
  session.
- `BuiltinCurveCallbacksReachRequestedWorkerConcurrency` repeats the built-in
  tiled `curve_transform` path three times at each `1/2/4/8` grant and requires
  exact callback overlap through a test-only observer.
- `BuiltinCurveOutputMatchesBetweenOneAndEightWorkers` compares packed pixel
  rows from the public Host result and requires one-worker and eight-worker
  output to be bitwise equal.

The observer exists only in `BUILD_TESTING` builds, is private to the source
tree, and is never installed. These cases prove reachable concurrency and
deterministic output; they do not claim a machine-independent speedup.

`opencv_operation_concurrency_benchmark` is the corresponding long-lived
manual measurement tool. It is intentionally absent from CTest and CI. The
tool creates and removes a disposable temporary Graph root, executes the real
Host/benchmark/private-execution/built-in-operation path, retains no result artifact,
and prints environment, raw wall-time samples, median wall time, throughput,
speedup, and maximum callback concurrency to stdout. Build and run it with:

```bash
cmake --build build --target opencv_operation_concurrency_benchmark -j
./build/tests/opencv_operation_concurrency_benchmark \
  --size 2048 --warmups 2 --samples 7 --chain-length 4
```

The native snapshot captured on 2026-07-15 used macOS `arm64`, Clang 21.0.0
(`clang-2100.1.1.101`), OpenCV 4.12.0, reported hardware concurrency 10, and
reported `opencv_internal_threads=1`. The workload was a chain of four built-in
`curve_transform` nodes over a 2048-by-2048 FP32 image, with two warmups and
seven samples per grant:

| Workers | Median wall (ms) | Throughput (Mpix/s) | Speedup | Max in flight |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 27.450 | 611.188 | 1.000 | 1 |
| 2 | 19.567 | 857.433 | 1.403 | 2 |
| 4 | 15.688 | 1069.455 | 1.750 | 4 |
| 8 | 15.008 | 1117.910 | 1.829 | 8 |

The raw wall-time samples in milliseconds were:

- 1 worker: `27.694|27.134|27.450|27.183|27.869|27.250|28.035`
- 2 workers: `19.021|19.567|19.774|19.497|19.435|20.427|20.997`
- 4 workers: `16.059|15.688|15.992|15.727|15.600|14.692|14.649`
- 8 workers: `16.436|16.610|16.512|15.008|14.859|14.064|14.760`

This snapshot establishes that the requested grants reached the real callback
path and that the tested machine benefited from removing outer serialization.
It is not a permanent performance baseline or pass/fail threshold. Rerun the
exact command when evaluating another machine, compiler, OpenCV version, or
operation-concurrency change, and interpret the newly printed raw samples.

## CTest Registration

All intended GoogleTest binaries should be registered with CTest. This includes
milestone tests and `test_propagation_contracts`, which may currently be
low-confidence.

Low-confidence tests should still be visible in validation rather than silently
excluded. If a test is not reliable enough to gate development, document that
status explicitly and create follow-up work to upgrade or replace it.

Milestone tests and `test_propagation_contracts` are registered with CTest so
they are visible, but they remain low-confidence legacy tests until a follow-up
pass rewrites them as narrower regression tests with clearer fixtures and
assertions.

`test_propagation` is different: it is a scriptable REPL/tool target, not a
GoogleTest binary. CMake keeps it buildable for manual scripts and ad hoc
validation, but CTest does not discover or run it. Do not claim that CTest
covers `test_propagation`; run the exact manual command separately when needed.

The provider-dependent default full test suite is registered only when
`BUILD_TESTING`, OpenCV, YAML, graph CLI, the repository OpenCV operation
provider, and repository OpenCV operation plugins are all enabled. It registers
`test_stdlib_image_buffer_processing` and compiles the standard-library
implementation directly even though that producer uses OpenCV. The test
verifies clone independence, stride-safe deterministic bilinear border
behavior, channel conversions, and ROI copying. The default CTest inventory
also includes `DependencyDisabledInstallSmoke`.

When only `PHOTOSPIDER_BUILD_OPENCV_OPERATION_PROVIDER` is disabled from that
otherwise default test profile, CMake does not create or discover the broad
suite. It keeps the provider-independent `test_kernel_contracts` target
buildable for the injected-codec smoke and registers exactly the focused
optional-provider GoogleTest, the three dedicated disk-cache diagnostic
concurrency cases, and `DependencyDisabledInstallSmoke`.

The default CTest inventory intentionally contains no phase-completion scan,
migration-residue check, stale-term search, Doxygen audit, or issue-specific
orchestration. The daemon help driver, static package-consumer smoke, and graph
CLI allocation-failure driver remain registered because they exercise real
installed/runtime behavior.

For IPC changes, focused local product validation is:

```bash
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol test_ipc_host \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|ExecutionTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientExecutionDefaults|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonExecution|IpcDaemonPolicy|IpcObservationFixtureDaemon|PhotospiderdCapabilityHelp|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
```

Temporary daemon processes, sockets, graph sessions, package prefixes, and
consumer trees must be absent after these tests. The mode-`0600` persistent
`${socket}.lock` inode is an intentional product synchronization artifact; a
test-owned temporary root removes it with that root, while the real default
runtime location preserves it. CTest output/JUnit and remote CI artifacts are
the evidence; do not create `tests/results` or an issue-specific replay/
provenance helper.

## Known Test Quality Caveat

Some milestone tests and propagation contract tests were written as development
checks rather than polished regression tests. They should be registered so they
are visible, then upgraded later into clearer, higher-confidence tests.

`test_propagation` remains a manual tool target until it is either converted
into a proper GoogleTest binary or replaced with narrower CTest-registered
fixtures.

## GitHub/CI Integration Status

GitHub Actions and the Linux CI container are maintained validation paths.
Pull requests targeting `main` use the base branch's protected workflow through
`pull_request_target`, while pushes to `main` and `CI/**` also run CI. Ordinary
feature branches cannot change `ci/**`, `.github/workflows/**`, or
`Dockerfile.ci`; those inputs require a `CI/**` branch in the base repository.
Only a same-repository `CI/**` pull request is deduplicated in favor of that
branch's push run. A fork with the same branch prefix is rejected before
checkout, and branch spelling alone never authorizes protected-path changes.
Both production guards write `git diff --name-only -z` to a parent-visible
artifact, read exact NUL records into Bash, match complete path values, and use
`%q` for human-readable changed/protected logs. Producer or reader failure is
fail-closed, and a newline inside a valid `ci/**` filename cannot bypass or
forge the protected-path inventory.

Every triggered run keeps a stable `healthcheck` conclusion. The integration
workflow classifies exact event revisions before configuration: changes limited
to `docs/**`, root Markdown, and the documented root text contracts skip all
build, CTest, and integration shards intentionally, while the stable
`integration` gate verifies and reports that route. Any non-documentation path
or uncertain Git state runs full integration. Type changes and uncommon Git
statuses stay in the unfiltered path inventory. Every `CI/**` push also forces
full current-head integration, including a later incremental push that changes
only documentation. The workflows deliberately avoid `paths-ignore`, which
could leave a configured required check pending. Stable gates take the same
repository-identity decision: only a same-repository `CI/**` pull request can
report intentional deduplication; fork or missing identity fails closed.

`healthcheck-published-image` is a container job, and published-image
healthcheck execution and build/test integration jobs run in
`ghcr.io/<owner>/<repo>/photospider-ci:latest`; lightweight routing and result
gates remain on `ubuntu-latest`. Immediately after checkout, the published
container's unique `Trust checked-out workspace` step binds `shell: bash`,
adds only the exact `$GITHUB_WORKSPACE` to the job-persistent global
`safe.directory`, and verifies `HEAD^{commit}` read-only. It neither configures
`safe.directory=*` nor executes a checked-out repository script. This trust
boundary precedes both conditional history fetches and `healthcheck.sh`,
including `main` push and `workflow_dispatch` routes where neither fetch runs,
instead of relying on checkout's temporary HOME-scoped configuration. The
`Fetch pull request base history` and `Fetch CI branch main history` steps each
also bind `shell: bash`, making their `set -Eeuo pipefail` prologues valid
without relying on the container default shell. If a change modifies an image
input, the workflow builds `photospider-ci:local` and runs the same repository
scripts in that image so validation does not race image publication. For pull
requests, the published-image and local-image healthcheck jobs each fetch the
target branch from the base-repository URL, verify `CI_BASE_SHA` as the event's
exact base commit inside their own job, and supply that exact SHA as `CI_BASE_REF`
independently of the fork checkout's `origin`. For every `CI/**` push, each job
instead fetches and verifies `origin/main`, then supplies it as `CI_BASE_REF`
so the static scope remains cumulative from the `main` merge base across
successive pushes. A later documentation-only push therefore cannot hide an
earlier unformatted C++ commit. An ordinary `main` push retains
`github.event.before` as its incremental `CI_BASE_REF`. Published-image
verification precedes `healthcheck.sh`; local-image verification precedes the
head Dockerfile build and mounted-workspace execution. Required fetch or parse
failure therefore stops before the script's fallback base selection.
`Dockerfile.ci` installs the C++ toolchain, CMake, OpenCV, yaml-cpp, GTest,
nlohmann-json, clang-format, Python, and cpplint required by those scripts.
The image detector uses no Git status filter. The healthcheck static-scope
inventory instead uses `--diff-filter=d` to omit deleted formatter/linter
inputs while retaining type changes and uncommon non-deletion statuses. Both
use NUL-delimited Git output and a parent-visible temporary file. A failing
`git diff` therefore terminates image detection or healthcheck static-scope
detection without emitting a false negative route.

The maintained entry points are:

- `ci/scripts/healthcheck.sh` for fail-closed changed-path inventory, diff,
  format, cpplint, the build-smoke inventory regression, and both durable shell
  regressions.
- `ci/scripts/change_classification.sh` and
  `ci/scripts/change_classification_test.sh` for fail-closed documentation-only
  routing and its durable event/path regression matrix.
- `ci/scripts/ci_routing_test.sh` for exact canonical locking of both
  `protected-ci-paths.if` expressions; execution of the real stable-gate,
  fork-rejection, and protected-path blocks; job/step-scoped locking of both
  published-image history-fetch steps' own `shell: bash` metadata;
  job/step-scoped locking of the unique published-image workspace-trust step,
  its exact non-wildcard global `safe.directory`, read-only HEAD verification,
  and checkout-before-trust-before-fetch/healthcheck order;
  published/local job-scoped pull-request exact-base and `CI/**`
  cumulative-main ordering; exact three-way `CI_BASE_REF` source routing;
  allow-empty configuration preflight, strict post-build matrix job output,
  empty-output-safe `fromJSON`, full-CTest/fallback routing; newline-path
  artifacts; and detector/reader/producer failure propagation. It
  executes the production trust block with an isolated HOME/repository and
  requires exactly that repository in the resulting global trust list. It also
  executes both production main-fetch blocks, while an isolated Git history
  proves cumulative main scope retains earlier C++ and event-before scope sees
  only the later docs increment. The local source/shell lock does not emulate
  GitHub's expression evaluator, cross-UID dubious ownership, or the hosted
  container runner.
- `ci/scripts/build_smoke_inventory.py` and its focused regression for strict
  CTest JSON parsing, deterministic strict or explicit allow-empty matrix
  generation, duplicate label values, safe artifact keys, NUL-delimited names,
  exact index-based execution, absent/disabled/commandless selections stopping
  before a second subprocess, and a real configure-placeholder-to-post-build
  discovery fixture.
- `ci/scripts/integration_plan.sh` for allow-empty exact-label configuration
  preflight without authoritative matrix output.
- `ci/scripts/build_integrity.sh` for the default producer profile, including
  required-target/full builds, strict post-build labelled CTest validation, and
  the authoritative matrix job output.
- `ci/scripts/ctest_full.sh` for the main CTest suite with the exact
  `build-smoke` label excluded.
- `ci/scripts/integration_suite.sh` for sequential integration behavior checks,
  running every post-build-discovered build smoke alongside full CTest, CLI,
  propagation, and plugin coverage. Its protected trusted-CI revision still
  references the removed scheduler suite; the base-repository CI owner must
  replace that block with current policy/execution coverage before Issue #75
  can pass the remote integration gate. An ordinary feature branch does not
  modify that protected script.

CI source inventories and exclusion lists must describe maintained tests and
current source paths. Migration-only harness names must not be retained as
permanent exclusions or treated as product behavior. GitHub job status and
downloadable artifacts report remote integration behavior. The complete
workflow and artifact download boundary are documented in
`docs/CI/github-actions.md`.

Architecture evolution goals are intentionally not maintained in this testing
document. They are recorded in `docs/roadmap/Kernel-Evolution.md`, while each
implementation change defines its own proportional validation and durable
regression coverage.
