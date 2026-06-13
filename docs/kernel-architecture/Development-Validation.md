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

## Known Test Quality Caveat

Some milestone tests and propagation contract tests were written as development
checks rather than polished regression tests. They should be registered so they
are visible, then upgraded later into clearer, higher-confidence tests.

`test_propagation` remains a manual tool target until it is either converted
into a proper GoogleTest binary or replaced with narrower CTest-registered
fixtures.

## GitHub/CI Integration Status

GitHub Actions and the CI container image are not part of the currently
maintained validation path. The workflows are manual-only `workflow_dispatch`
scaffolding, and `Dockerfile.ci` is TODO integration scaffolding unless a future
change explicitly re-enables and validates them.

Current evidence should come from local build, focused test binaries, CTest, and
saved `tests/results/...` artifacts. Do not claim that GitHub CI or the
containerized CI image is a supported or passing test chain until the Dockerfile
dependencies, workflow triggers, checkout/submodule behavior, build commands,
and CTest invocation are updated and proven with fresh CI evidence.

## Refactor Boundaries

The following are recognized follow-up refactors, not part of the current
kernel-contract cleanup:

- Add richer dirty snapshot visualization APIs after the frontend display
  contract is defined.
- Global HP dirty ROI now routes through HP dirty planning instead of the former
  full-recompute fallback. Evidence should include the coordinator stage,
  planned work, and dirty snapshot artifacts before claiming performance
  improvements beyond the covered path.

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
