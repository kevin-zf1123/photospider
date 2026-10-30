---
spec_schema_version: 1
id: GEN-05F
parent_id: GEN-05
function: path_distance_coordinate
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# GEN-05F: path distance coordinate

Inherits [GEN-05](GEN-05_gradient_coordinates_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

PathSet and positive scale[1] in px; t[H,W].

### Parameters and domain

canvas/dtype; explicit distance_source=centerline_unsigned/fill_signed/stroke_signed; provide required fill/stroke parameters.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::path_distance_coordinate(
    WorkflowDocument* document, WorkflowInput path, WorkflowInput scale,
    std::uint64_t height, std::uint64_t width,
    std::string distance_source = "centerline_unsigned",
    Parameters parameters = {},
    std::vector<WorkflowInput> distance_inputs = {},
    Profile profile = Profile::Strict);
```

`distance_source` selects the PTH-11 member: `centerline_unsigned`, `fill_signed` or `stroke_signed`. `distance_inputs` follow the geometry port (for example the stroke width input required by `stroke_signed`), and `parameters` holds the complete PTH-11 canvas and fill/stroke settings. `scale` is Float32/64 `[1]` with the dtype of the published distance; it is broadcast onto the output and divides the distance through NUM division. The helper returns the division output. No monolithic GEN-05F operation key exists.

## Mathematics

Compose the corresponding PTH-11 member followed by NUM division. Preserve the published-distance rounding stage; do not replace the workflow with one rounded true-distance/scale expression.

## Behavior

Support class: Regional.

Each output pixel may depend on the complete path.

## Acceptance

Distance 2 with scale 4 gives 0.5; open-path signed fill requires explicit fill interpretation.
