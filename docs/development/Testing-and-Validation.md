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
exact unique inventories of 55 typed Client calls plus all 53 non-destructor Host virtuals. Package
inspection also requires the IPC archive and exact three-header surface,
permits only `Threads::Threads` in the exported IPC link interface, positively
allows only the current C++ standard-library and installed `photospider/`
public includes, and rejects raw JSON, socket-address/descriptor, file-identity,
file-mapping, and backend declarations. This is the gate's exact boundary, not
an exhaustive promise about every possible POSIX spelling. With backend
discovery disabled, `COMPONENTS ipc_client OPTIONAL_COMPONENTS embedded`
succeeds with only `ipc_client` found, and an unknown optional component
remains not-found without invalidating the package.
The durable
`IpcDisabledInstallSmoke` configures a separate clean producer with
`PHOTOSPIDER_BUILD_IPC=OFF` and `BUILD_TESTING=OFF`; it verifies that no IPC
build forwarder, installed header, archive, executable, or exported target is
advertised, a required `ipc_client` component fails discovery, and an external
default embedded Host consumer still links and runs. Required unknown package
components fail as well; optional disabled `ipc_client` and unknown components
remain not-found without failing discovery; omitting components or requesting
`embedded` retains the existing backend dependency resolution.

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
`IpcDisabledInstallSmoke`, focused `test_ipc_protocol`/`test_ipc_host` cases,
and real-process `test_ipc_daemon` cases follow the same rule: they exercise
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

The CLI/Host and scheduler Doxygen AST tools are long-lived manual developer tools,
not tests. Run them explicitly when the corresponding declarations,
definitions, exception contracts, or target source closures change:

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-cli-host-doxygen
python3 tests/verification/codebase_structure/scheduler_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json \
  --out /tmp/photospider-scheduler-doxygen
```

The CLI/Host audit treats
`apps/graph_cli/src/cli_config.cpp::apply_cli_scheduler_defaults` as the
canonical scheduler-default definition and validates its complete Doxygen in
that translation unit. It separately audits `run_graph_cli` and its
resource-exhaustion policy. For `ConfigEditor::SyncUiStateToModel`, both parse
chains must rethrow `std::bad_alloc`. Scheduler-worker validation handles
`std::invalid_argument` specifically, while history-size `std::stoi` ignores
its other errors through exactly one broad `catch (...)`; the catalog therefore
expects one broad catch for that function, guarded by the preceding
`std::bad_alloc` handler.

Their files may remain in the primary repository because this document defines
their lasting manual role. They must remain absent from CTest and GitHub CI.
Their `--out` directories are disposable temporary working directories outside
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

`test_cli_scheduler_config` is the CTest-registered integration binary for the
reusable `run_graph_cli` option boundary as well as scheduler configuration.
Its option cases use a complete deterministic Host spy and the real ordered
parser. Successful load/output and short-traversal cases preserve the
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
cmake --build build --target test_cli_scheduler_config -j
./build/tests/test_cli_scheduler_config \
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
successful replacement advances topology generation and resets runtime state,
and retry remains possible.

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

Focused companion regressions own the remaining boundaries:

- `test_kernel_contracts` drives the real `GraphIOService` stream through
  post-write, post-flush, and post-close failure states. Each phase must return
  `GraphErrc::Io`, and the created destination demonstrates the documented
  non-atomic post-open behavior.
- `test_scheduler_worker_budget` proves an invalid input document releases both
  already-started scheduler reservations without publishing a session.
- `test_ipc_protocol` proves exact Graph status propagation, one-call mutation
  behavior, and daemon session-name rollback after failed load.
- `test_ipc_daemon` proves the real transport returns save `NotFound` and `Io`
  exactly, leaves the remotely owned graph inspectable after destination
  failure, and accepts a subsequent successful save.

Run the focused validation with:

```bash
cmake --build build --target test_graph_document_errors test_host_adapter \
  test_kernel_contracts test_scheduler_worker_budget test_ipc_protocol \
  test_ipc_daemon -j
./build/tests/test_graph_document_errors
./build/tests/test_host_adapter \
  --gtest_filter='EmbeddedHostAdapter.*Reload*'
./build/tests/test_kernel_contracts \
  --gtest_filter='GraphIoContract.Save*'
./build/tests/test_scheduler_worker_budget \
  --gtest_filter=EmbeddedHostSchedulerBudget.InvalidYamlAfterSchedulerStartDoesNotPublishAndReturnsPairExactlyOnce
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

## OpenCV Operation Concurrency Validation

`test_opencv_operation_concurrency` is a CTest-registered integration binary
for the long-lived operation-provider and benchmark-worker contracts. It uses
Host-boundary records and bounded callback gates rather than elapsed-time
thresholds:

- `BenchmarkAutoThreadsPublishResolvedGrantToHost` proves that automatic
  selection is resolved once before Host configuration, publishes a nonzero
  grant before Graph load, and reports that identical grant without repeating
  hardware detection in the verdict.
- `BenchmarkThreadsConfigureExactHostSchedulerWorkers` runs the real
  `BenchmarkService`, Host scheduler configuration, Graph load, and registered
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
Host/benchmark/scheduler/built-in-operation path, retains no result artifact,
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
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|SchedulerTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonSchedulers|IpcObservationFixtureDaemon|PhotospiderdCapabilityHelp|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
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
  format, cpplint, and both durable shell regressions.
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
  newline-path artifacts; and detector/reader/producer failure propagation. It
  executes the production trust block with an isolated HOME/repository and
  requires exactly that repository in the resulting global trust list. It also
  executes both production main-fetch blocks, while an isolated Git history
  proves cumulative main scope retains earlier C++ and event-before scope sees
  only the later docs increment. The local source/shell lock does not emulate
  GitHub's expression evaluator, cross-UID dubious ownership, or the hosted
  container runner.
- `ci/scripts/build_integrity.sh` for configure, required-target and full builds,
  plus CTest discovery.
- `ci/scripts/ctest_full.sh` for the main CTest suite.
- `ci/scripts/integration_suite.sh` for sequential integration behavior checks,
  including CLI, propagation, plugin, and scheduler coverage.

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
