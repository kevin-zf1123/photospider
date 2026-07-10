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
Compatibility is maintained through the command/provider/context audit against
the 3.16 command and policy inventory, the current GitHub integration package
consumer, and a targeted real old-version run when a compatibility-sensitive
change or release check warrants one.

When a targeted minimum-version run is performed, it starts from a fresh producer
build tree, configures the top-level project with CMake 3.16 and
`BUILD_TESTING=OFF`, builds the real `photospider` target, installs to a fresh
prefix, and only then configures, builds, and runs an external
`find_package(Photospider)` consumer. It must not reuse a producer tree
configured by a newer CMake or substitute an internal helper target. If no
natively compatible old CMake runtime is available locally, the review records
that the targeted run was not performed and makes no CMake 3.16 runtime PASS
claim; architecture emulation is not required.

Freshness verification is fail-closed. The producer build tree, installation
prefix, consumer source tree, and consumer build tree must be removed without
suppressing errors; evidence must record whether each path existed before
cleanup, prove that it and any pre-configure `CMakeCache.txt` were absent after
cleanup, and derive `freshness_verified` from those filesystem observations.
Copying a command-line "fresh" flag is not evidence.

Every producer/consumer evidence run must generate `environment.md` from that
same run's `actual` observations and process invocation. It records ordered UTC
start/finish times, the source HEAD plus staged and unstaged patch hashes, an
ordinary-untracked source path/content-hash inventory, the requested and
resolved CMake executable/version, producer/install/consumer paths, and the
exact in-environment replay command. The comparison must verify
those fields against `actual.json`, command-log headers, producer freshness, and
the producer `CMakeCache.txt`; a manually retained environment description from
an older run is not valid evidence. The maintained-command/provider audit is a
separate run and keeps its own command and time window rather than being folded
into the producer/consumer timestamps.

A formal validation root must freeze one content-addressed final source
identity before any consumed producer, consumer, test, structural, or quality
run begins. Every child artifact records the same base commit/tree, final
snapshot tree, final patch hash, and ordinary-untracked source inventory from
its own invocation. Checks that consume personal-overlay inputs additionally
record a separate overlay commit/tree, snapshot, and patch identity, excluding
generated evidence outputs from the identity to avoid self-reference. The root
`compare.log` recomputes both applicable identities after all runs and fails
unless every child identity and recomputed identity are equal. Source edits
after the freeze invalidate the run; artifact timestamps, directory names, and
hashes copied from an earlier review are not substitutes for this equality
proof.

When the selected CMake generator exposes multiple configurations, package
evidence must select that same generator for both producer and consumer,
record each `CMAKE_GENERATOR` and `CMAKE_CONFIGURATION_TYPES` cache value, and
build/install/run `RelWithDebInfo` plus another available configuration. The
consumer executable must still be resolved from the configuration-specific
`$<TARGET_FILE:...>` manifest; a synthetic path-layout check alone is not
multi-configuration runtime evidence.

The CMake 3.16 command/provider/context audit treats the root producer and package-config template
as separate execution contexts. A project command is available only after its
local declaration or after an `include()` whose real CMake 3.16 module source
provides it; `@PACKAGE_INIT@` and `CMakeFindDependencyMacro` are explicit,
position-sensitive providers. Every maintained
`cmake_policy(SET CMPxxxx value)` call is compared with the real 3.16 policy
list, and every unknown policy use requires an exact enclosing
`if(POLICY <same-id>)`. The real `--help-command-list` proves command-name
existence, while subcommand/keyword compatibility is claimed only for the
finite sensitive-token inventory recorded in evidence, not for complete CMake
grammar. The audit and the current GitHub CI package consumer together guard
the maintained path without pinning every PR to one Ubuntu or CMake release.
Targeted old-version evidence adds a strict-fresh active-path run when needed.
Apple and Windows branches have command/policy/context and recorded sensitive-
token static coverage only until real runners execute them.

Issue-specific phase and residue guards protect the landed public-boundary
semantics only as personal-overlay evidence. Their review-15 copies live under
`tests/results/codebase-refactor/phase-4-static-product/review15/migration_gates/`
and are not primary-repository CTest or CI inputs. They may inspect the
authoritative OpenSpec and Chinese mirrors, architecture documents, utility
diagrams, Kernel/embedded-adapter sources, and CLI sources to prove one
migration snapshot. That evidence keeps frontend flow behind `ps::Host`,
classifies Kernel and InteractionService as internal, confines
`Kernel::ComputeRequest` construction to the adapter, and rejects an obsolete
general worker queue in GraphRuntime. Long-lived runtime tests, public-header
compilation, and package-consumer tests own the durable product boundary after
the issue-specific scans are archived.

## Validation and Evidence Ownership

Primary-repository CTest and CI entries are reserved for long-lived software
behavior: correctness, performance, stability, multithreaded execution, error
handling, compile boundaries, package consumption, and runtime API boundaries.
`StaticProductConsumerSmoke`, `GraphCliOptionBadAlloc`, GoogleTest discovery,
`PublicHeaderSelfContainment`, and `SplitComputeServiceRuntimeTrace` satisfy
that rule because they execute or compile the maintained product. A phase name,
migration-residue search, stale-term detector, source-layout completion check,
issue replay, or evidence/provenance report is not a software behavior test and
must not be registered with CTest or invoked by CI.

The CLI/Host and scheduler Doxygen AST tools are long-lived manual developer tools,
not tests. Run them explicitly when the corresponding declarations,
definitions, exception contracts, or target source closures change:

```bash
python3 tests/verification/codebase_structure/cli_host_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json --out <out>
python3 tests/verification/codebase_structure/scheduler_doxygen_ast.py \
  --repo . --compile-commands build/compile_commands.json --out <out>
```

Their files may remain in the primary repository because this document defines
their lasting manual role. They must remain absent from CTest and GitHub CI.
Issue-specific replay, migration, formal-report, source-identity, provenance,
and evidence helpers belong under `tests/results/<change-or-feature>/...` in
the personal overlay. A clean primary clone, CMake configuration, CTest
inventory, and CI script must not read or import that overlay content.

Validation is proportional. During implementation, run scoped static checks,
affected build targets, and focused regressions. After source and documentation
are frozen, perform at most one native clean configure, one full build, and one
complete CTest run with JUnit output. Formal local evidence reuses that same
build tree for focused stress and manual inspection; it does not create a build
tree per gate. Do not use Docker or local `linux/amd64` emulation for this final
local pass. Current-head GitHub Actions remains the authoritative remote
integration environment.

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
validation, but CTest does not discover or run it. Do not cite `ctest` evidence
as covering `test_propagation`; cite the specific manual command transcript if
that target is used.

The default CTest inventory intentionally contains no phase-completion scan,
migration-residue check, stale-term search, Doxygen audit, or personal-overlay
helper. The static package-consumer smoke and graph CLI allocation-failure
driver remain registered because they exercise real installed/runtime behavior.

## Known Test Quality Caveat

Some milestone tests and propagation contract tests were written as development
checks rather than polished regression tests. They should be registered so they
are visible, then upgraded later into clearer, higher-confidence tests.

`test_propagation` remains a manual tool target until it is either converted
into a proper GoogleTest binary or replaced with narrower CTest-registered
fixtures.

## Issue #34 Review-15 Local Validation

Review 15 evidence lives under
`tests/results/codebase-refactor/phase-4-static-product/review15/`. Its formal
driver freezes separate primary and personal-overlay content identities, creates
one native clean `BUILD_TESTING=ON` tree, performs one full build and one full
CTest/JUnit run, and reuses that tree for focused scheduler/plugin/CLI/runtime
stress plus manual AST and migration evidence. It intentionally omits the prior
second `BUILD_TESTING=OFF` producer, multi-configuration rebuilds, Docker, and
local `linux/amd64` emulation.

The durable CTest inventory contains real package consumption, CLI process
error handling, public-header compilation, runtime trace behavior, and all
GoogleTest binaries. Review-specific phase/residue/replay tools live only under
`review15/migration_gates/`; the formal driver invokes selected tools directly,
while clean CMake, CTest, and CI contain no personal-overlay dependency. The
machine-readable classification and rationale are in
`review15/script_ownership_inventory.json` and its Markdown companion.

Focused coverage adds exact CPU/GPU exception-state rollback, cross-epoch
publication gating, two-stage scheduler running publication, complete scheduler
plugin owner forwarding, GraphRuntime shutdown sweeping, and reusable CLI
startup/option exception handling. Formal local PASS still leaves intentional
dual-repository commit/push, current-head GitHub Actions and artifact review,
fresh Codex/Copilot review with zero unresolved threads, and any genuinely
required real Windows/MSVC run as external gates. Issue #34 and the active CLI
layout change remain incomplete locally.

## Issue #34 Review-14 Local Validation

Review 14 evidence lives under
`tests/results/codebase-refactor/phase-4-static-product/review14/` and is
generated by one fail-closed orchestration over a frozen, content-addressed
source snapshot. Its root comparison consumes child identities rather than
trusting directory names or an earlier review's hashes.

The focused behavior gates cover allocation-independent exact-key operation-
plugin unload, unload-all, destruction, relative-path normalization failure,
reverse successful-load restoration, scheduler-plugin raw
instance ownership during host owner allocation/type-name failure, CPU/GPU
batch exception fencing, immediate clean reuse, and the GPU HP-CPU idle-wait
handshake. They also cover the active benchmark/CLI `std::bad_alloc` path.
Phase-4 differential fixtures follow reference, pointer/dereference, chained,
conditional, and mutator-call aliases and require the same unpolluted
`exception_ptr` to be transported and rethrown; the CLI Clang AST audit checks
every maintained Host-facing declaration/definition and every inner broad
catch.

Formal product gates use `current_consumer/`, one Ninja Multi-Config producer
for `multiconfig/relwithdebinfo/` and `multiconfig/debug/`, a clean
`BUILD_TESTING=OFF` product/ABI inspection, and a clean `BUILD_TESTING=ON`
all-target build plus CTest/JUnit. Structural and quality gates include the
phase-4/phase-7 scans, Host/member/dirty/GraphRuntime guards, OpenSpec/task
audits, formatting/lint, Python static checks, Doxygen AST, whole-authorized-
tree `__pycache__`/artifact inspection, both Git repositories' structured
diffs, a flat/pair/invalid structure-argument schema differential, and a native
real/missing CMake provider-resolution differential. A missing optional provider
is reported as not applicable without importing definitions; a missing required
provider is a structured failure rather than an exception traceback.

Every child result must carry the applicable primary and overlay identities
frozen in `source_identity/`. `provenance/` and the root formal report
re-evaluate all identities after the run; one missing or unequal identity
invalidates the complete local result.

At the user's direction, a local `linux/amd64` replay of the same CI container
is not a Review-14 completion gate. Native local gates run first; current-head
GitHub Actions is the authoritative integration environment, and any failing
artifact must be inspected and repaired there. This does not replace earlier
CMake 3.16 minimum-version evidence or claim a new local CMake 3.16 run.
Local PASS still cannot close Issue #34 or archive the active change:
intentional commit/push, authoritative GitHub CI and artifact review,
review-bot/thread resolution, and a real Windows runner remain external
completion gates.

## Issue #34 Review-13 Local Validation

Review 13 closes the locally reproducible findings against the content-addressed
pre-fix source recorded under
`tests/results/codebase-refactor/phase-4-static-product/review13/pre_fix/`.
The saved failing binaries, runtime transcripts, parser fixtures, patch hash,
and derived comparison reproduce all six reviewed defects without rebuilding
the historical source in the current worktree.

The corresponding post-fix evidence is split by behavior rather than by a
single pass/fail claim:

- `post_fix/` repeats the operation-plugin transaction regressions ten times
  and the CPU/GPU exception-publication regressions fifty times. It proves that
  every post-registrar allocation failure leaves the live registry, source map,
  result map, and retained handle set unchanged; failed callbacks are destroyed
  before library unload; ordinary registrar errors remain result records;
  `std::bad_alloc` is rethrown unchanged; the first scheduler publisher makes
  its exact `exception_ptr` visible before the flag; and both schedulers remain
  reusable after worker and concurrent-publisher stress.
- `phase4_static_product_scan/`, `phase7_plugin_registration/`, and
  `cli_host_doxygen_ast/` preserve the `std::bad_alloc` control-flow boundary,
  the versioned operation-plugin ABI plus transaction language, and complete
  Clang-AST-verified Host-facing CLI Doxygen contracts.
- `consumer_smoke_current/`, both `multiconfig_*` directories,
  `binary_abi_checks/`, and `product_off/` cover a clean `BUILD_TESTING=OFF`
  static product, public-header consumption, runtime package use, two
  configurations from one multi-config producer, and the absence of test seams
  from the installed product.
- `final_clean/` records a clean `BUILD_TESTING=ON` all-target build and the
  complete CTest JUnit result. `quality/` records formatting, lint, Python
  compilation/static analysis, Doxygen AST, diff-integrity, and structured
  evidence checks. `tasks_audit/` proves that English and Chinese task
  checkboxes remain synchronized and that Issue #34 remains open locally.

These artifacts establish local source and runtime closure only. They do not
authorize checking the Issue #34 tracking item or archiving its active OpenSpec
change. The same-CI-container run, commit/push, remote CI and review-bot gates,
review-thread resolution, and a real Windows runner remain external completion
gates.

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
- `ci/scripts/integration_suite.sh` for sequential local-container reproduction,
  including CLI, propagation, plugin, and scheduler checks.

`SplitComputeServiceRuntimeTrace` writes its transient output below the CMake
build tree, never below personal-overlay `tests/results`. The current
`ctest_full.sh` exclusion is a CI runtime-policy choice rather than an overlay
dependency; focused or complete native CTest evidence may run it directly.
GitHub job status and
downloaded artifacts supplement, rather than replace, focused tests and
task-specific `tests/results/...` expected/actual/compare evidence. The complete
workflow, Docker reproduction commands, and artifact download boundary are
documented in `docs/CI/github-actions.md`.

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
`src/kernel/services/compute-service/`. Boundary coverage lives in
`tests/test_compute_service_split.cpp`, with retained regression coverage from
`test_kernel_contracts`, `test_propagation_contracts`, `test_scheduler`,
`test_milestone34`, and `test_gpu_pipeline_scheduler`.

The `GraphTraversalService` topology/ROI split has landed. Boundary coverage
lives in `tests/test_graph_topology_boundaries.cpp`,
`tests/test_propagation_contracts.cpp`, and the reproducible evidence under
`tests/results/split-graph-traversal-service/`.
