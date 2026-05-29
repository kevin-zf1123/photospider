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

## CTest Registration

All intended GoogleTest binaries should be registered with CTest. This includes
milestone and propagation tests that may currently be low-confidence.

Low-confidence tests should still be visible in validation rather than silently
excluded. If a test is not reliable enough to gate development, document that
status explicitly and create follow-up work to upgrade or replace it.

## Known Test Quality Caveat

Some milestone and propagation tests were written as development checks rather
than polished regression tests. They should be registered so they are visible,
then upgraded later into clearer, higher-confidence tests.

## Refactor Boundaries

The following are recognized follow-up refactors, not part of the current
kernel-contract cleanup:

- Split `ComputeService` into planning, execution, intent update coordination,
  cache coordination, and metrics concerns.
- Split `GraphTraversalService` topology traversal from ROI/spatial propagation.

Those changes should receive separate OpenSpec changes once the contracts and
validation surface are stable.
