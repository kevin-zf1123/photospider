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
milestone and propagation tests that may currently be low-confidence.

Low-confidence tests should still be visible in validation rather than silently
excluded. If a test is not reliable enough to gate development, document that
status explicitly and create follow-up work to upgrade or replace it.

Milestone and propagation tests are registered with CTest so they are visible,
but they remain low-confidence legacy tests until a follow-up pass rewrites them
as narrower regression tests with clearer fixtures and assertions.

## Known Test Quality Caveat

Some milestone and propagation tests were written as development checks rather
than polished regression tests. They should be registered so they are visible,
then upgraded later into clearer, higher-confidence tests.

## Refactor Boundaries

The following are recognized follow-up refactors, not part of the current
kernel-contract cleanup:

- Split `GraphTraversalService` topology traversal from ROI/spatial propagation.

The `ComputeService` split now has a dedicated `split-compute-service`
OpenSpec change and a maintained plan in `Compute-Service-Split.md`. The first
split is implemented behind internal modules under
`src/kernel/services/compute-service/`. Boundary coverage lives in
`tests/test_compute_service_split.cpp`, with retained regression coverage from
`test_kernel_contracts`, `test_propagation_contracts`, `test_scheduler`,
`test_milestone34`, and `test_gpu_pipeline_scheduler`.

The `GraphTraversalService` topology/ROI split should receive a separate
OpenSpec change after the compute split boundaries are stable.
