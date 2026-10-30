---
spec_schema_version: 1
id: PTH-02E
parent_id: PTH-02
function: evaluate_pathset
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-02E: evaluate pathset

Inherits [PTH-02](PTH-02_evaluation_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

PathSet,segment_indices[N],t[N]; four evaluation outputs.

### Parameters and domain

Supported authorities/degrees inherit PTH-02A-D; Core IDs are verb rows (M invalid,Z closing edge), primitive IDs are reference rows.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
struct PathEvaluation final {
  WorkflowNodeOutput position, derivative, tangent, tangent_valid;
};
Result<PathEvaluation> ps::generation::evaluate_pathset(
    WorkflowDocument* document, const SchemaTemplate& schema,
    WorkflowInput path, WorkflowInput indices, WorkflowInput t,
    Parameters parameters = {}, Profile profile = Profile::Strict);
```

The helper dispatches statically on `schema` (`photospider.ops.path_core` or `photospider.ops.path_primitives`) to the matching PTH-02 member and returns its four outputs. Core segment IDs are evaluable verb rows; primitive IDs are reference rows. No global-parameter segment selection or intermediate endpoint rounding is introduced.

## Mathematics

Dispatch by the checked static schema, preserving original segment IDs and explicit rounding. Z runs from current point to M. A Primitive Close reference evaluates the line between its specified mathematical endpoints: P(t)=(1-t)*end+t*start, derivative=start-end. Evaluate those endpoints under their primitive mathematics and apply only the declared final rounding; no intermediate Float64 endpoint is invented. No implicit global-parameter segment selection.

## Behavior

Support class: Regional. A requested position alone need not compute the tangent.

## Acceptance

Mixed L/Q/C/Z, queries selecting evaluable verb rows, separate/joint outputs, empty path with query NoSolution.
