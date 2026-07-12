# Development and Validation Notes

This document records repository-level validation expectations that affect
kernel trustworthiness.

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
`StaticProductConsumerSmoke`, `GraphCliOptionBadAlloc`, GoogleTest discovery,
and `PublicHeaderSelfContainment` satisfy that rule because they execute or
compile the maintained product.
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
the change's risk warrants it. Do not use Docker or local `linux/amd64`
emulation as a routine local preflight. Current-head GitHub Actions remains the
authoritative remote integration environment.

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
orchestration. The static package-consumer smoke and graph CLI
allocation-failure driver remain registered because they exercise real
installed/runtime behavior.

For IPC changes, focused local product validation is:

```bash
cmake --build build --target photospider_ipc_client \
  photospider_ipc_server_internal photospiderd test_ipc_protocol test_ipc_host \
  test_compute_request_registry test_collection_snapshot_registry \
  test_output_store test_event_stream_boundaries test_ipc_daemon \
  public_header_self_containment -j
ctest --test-dir build --output-on-failure \
  -R '^(FrameCodec|ProtocolEnvelope|IntegerCodec|ProtocolErrors|ProtocolParams|ProtocolGraphLoad|ProtocolGraphClose|ProtocolOperationPlugins|HostRoutedGraphStateProtocolTest|StableInspectionPagingProtocolTest|InspectionJson|SessionRegistry|ComputeRequestRegistry|CollectionSnapshotRegistry|OutputStore|ComputeEventRing|SchedulerTraceRing|UnixSocketConnect|ClientLifecycle|ClientSurface|ClientCollectionAggregation|ClientJobValidation|ClientRetryPolicy|ClientResultValidation|IpcHost|IpcDaemon|IpcDaemonOperationPlugins|IpcDaemonSchedulers|IpcObservationFixtureDaemon|StaticProductConsumerSmoke|IpcDisabledInstallSmoke|PublicHeaderSelfContainment)'
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
`Dockerfile.ci`; those inputs require a `CI/**` branch.

Normal healthcheck and integration jobs run in the published
`ghcr.io/<owner>/<repo>/photospider-ci:latest` image. If a change modifies an
image input, the workflow builds `photospider-ci:local` and runs the same
repository scripts in that image so validation does not race image publication.
`Dockerfile.ci` installs the C++ toolchain, CMake, OpenCV, yaml-cpp, GTest,
nlohmann-json, clang-format, Python, and cpplint required by those scripts.

The maintained entry points are:

- `ci/scripts/healthcheck.sh` for diff, format, and cpplint checks.
- `ci/scripts/build_integrity.sh` for configure, required-target and full builds,
  plus CTest discovery.
- `ci/scripts/ctest_full.sh` for the main CTest suite.
- `ci/scripts/integration_suite.sh` for sequential integration behavior checks,
  including CLI, propagation, plugin, and scheduler coverage.

The obsolete `SplitComputeServiceRuntimeTrace` source/provenance harness has
been removed from the product test inventory. The protected
`ci/scripts/ctest_full.sh` still contains a now-inert exclusion token, and
`.github/workflows/ci_scheduler_log.yml` still inventories old source paths.
Repository policy forbids changing those files on this feature branch; a
follow-up `CI/**` branch created from main must remove the no-op exclusion and
update the workflow inventory after this layout reaches main. GitHub job status
and downloadable artifacts report remote integration behavior. The complete
workflow and artifact download boundary are documented in
`docs/CI/github-actions.md`.

## Refactor Boundaries

The following are recognized follow-up refactors, not part of the current
kernel-contract cleanup:

- Add richer dirty snapshot visualization APIs after the frontend display
  contract is defined.
- Global HP dirty ROI now routes through HP dirty planning instead of the former
  unconditional full-recompute fallback. Non-forced requests should prove local
  dirty work selection; forced HP dirty requests should prove full-frame HP
  planning and complete authoritative HP output before claiming correctness or
  performance improvements beyond the covered path.

The `ComputeService` split now has a dedicated `split-compute-service`
OpenSpec change and a maintained plan in `Compute-Service-Split.md`. The first
split is implemented behind internal modules under
`src/lib/compute/`. Boundary coverage lives in
`tests/integration/test_compute_service_split.cpp`, with retained regression
coverage from
`test_kernel_contracts`, `test_propagation_contracts`, `test_scheduler`,
`test_milestone34`, and `test_gpu_pipeline_scheduler`.

The `GraphTraversalService` topology/ROI split has landed. Boundary coverage
lives in `tests/unit/test_graph_topology_boundaries.cpp`,
`tests/unit/test_propagation_contracts.cpp`, and the maintained runtime behavior
tests that consume those boundaries.
