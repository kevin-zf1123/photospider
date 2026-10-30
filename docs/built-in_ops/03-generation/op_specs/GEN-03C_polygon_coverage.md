---
spec_schema_version: 1
id: GEN-03C
parent_id: GEN-03
function: polygon_coverage
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# GEN-03C: polygon coverage

Inherits [GEN-03](GEN-03_basic_shapes_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

CoreVerbs M/L/Z polygon PathSet; coverage[H,W].

### Parameters and domain

canvas, dtype default Float64, fill_rule=nonzero/evenodd.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::polygon_coverage(
    WorkflowDocument* document, WorkflowInput path, Parameters parameters,
    Profile profile = Profile::Strict);
```

The helper appends one `path.fill_polygon_area_v1_<profile>` node (PTH-09A) and returns its `coverage` output. `parameters` carries height, width, origin, dtype and fill_rule; there is no separate `generation.polygon_coverage` operation key.

## Mathematics

Compose PTH-09A exact polygon-area filling; self-intersections and holes use the selected fill rule. Open contours close only for this fill consumption.

## Behavior

Support class: Regional.

Each output pixel depends on the complete path.

## Acceptance

Repeated same-oriented contours differ under nonzero/evenodd; bow-tie, shared edges and orientation reversal.
