# ADR 0002: External Libraries Are Kernel Adapters

## Status

Accepted as a target constraint. The current implementation has not completed
this migration.

## Context

The public operation and Host contracts already use Photospider values, but
private Graph, ROI, dirty propagation, planning, cache, and runtime code still
uses OpenCV geometry and image objects. YAML also serves both as the graph file
format and as an internal runtime value model. These dependencies make an
image-processing library and a serialization library part of kernel semantics.

That coupling blocks independent geometry optimization, alternate operation
providers, in-memory graph definitions, general data types, and a build of the
kernel without OpenCV or yaml-cpp.

## Decision

The kernel owns its semantic types and minimum primitives:

- checked geometry, ROI, extent, grid, scale, and tile mathematics;
- stride-aware buffer views and the minimum copy/fill/validation operations
  required by planning and execution;
- format-neutral parameter values, graph definitions, and error contracts.

OpenCV is an optional adapter/provider. OpenCV image views, algorithms,
initialization, exception translation, and codecs remain behind operation,
buffer-adapter, or codec interfaces. OpenCV types do not appear in Graph,
propagation, planning, cache, or runtime interfaces.

Graph persistence is injected through format-neutral reader/writer contracts.
YAML remains one supported filesystem adapter, but `YAML::Node` does not remain
the runtime parameter, output, cache metadata, or graph-state value model.

Ownership migrations are complete corrections: old and new semantic types do
not coexist behind permanent forwarding wrappers.

## Consequences

- A dependency-free kernel build becomes an architectural acceptance test.
- Kernel primitives remain intentionally small; Photospider does not re-create
  a general image-processing library.
- Algorithm quality and codec policy stay replaceable outside orchestration.
- Graph load/reload/save requires an explicit transaction and error matrix.
- Operation providers must declare concurrency and resource behavior instead
  of hiding it behind process-wide library locks.
- Existing behavior requires staged vertical migration and regression tests;
  this ADR does not claim that the migration is already complete.
