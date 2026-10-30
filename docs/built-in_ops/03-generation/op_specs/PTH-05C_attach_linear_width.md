---
spec_schema_version: 1
id: PTH-05C
parent_id: PTH-05
function: attach_linear_width
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-05C: pair a path with linear width

Inherits [PTH-05](PTH-05_width_profiles_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

PathSet, stops[K], widths[K] Float64 and explicit domain/binding fields; return the unchanged complete PathSet value paired with a photospider.ops.path_width v1 Result.

### Parameters and domain

Three domains as PTH-05A; replace_existing default false, the absence of a conflicting width key is an unchecked caller premise; the standard key is width and the shared geometry contract specifies its schema. Normalized stops0..1; px profile covers associated length.

Public SDK helpers, declared in `photospider/ops/generation.hpp`:

```cpp
struct PathWithWidth final {
  WorkflowInput path;
  WorkflowNodeOutput width;
};
Result<PathWithWidth> ps::generation::attach_linear_width(
    WorkflowDocument* document, WorkflowInput path, WorkflowInput profiles,
    WorkflowInput bindings, WorkflowInput stops, WorkflowInput widths,
    Parameters parameters = {}, bool replace_existing = false,
    const WorkflowNodeOutput* existing_width = nullptr);

Result<WorkflowNodeOutput> ps::generation::evaluate_linear_width_binding(
    WorkflowDocument* document, WorkflowInput target, WorkflowInput binding,
    std::vector<WorkflowInput> sources, Parameters parameters = {},
    const WorkflowInput* source_map = nullptr);
```

`attach_linear_width` returns the original geometry `WorkflowInput` and a separate width Result produced by `path.linear_width_binding_v1_strict`. Its ordered binding inputs are the source geometry, Int64 `profiles[P,3]` (domain, stop_begin, stop_end), Int64 `bindings[B,3]` (target_subpath, profile_row, source_subpath), `stops` and `widths` (Float64 `[K]`); the output records source slot 0 and the source identity. The geometry is never copied into a replacement Result. Replacing an explicitly supplied existing width requires `replace_existing=true`.

`evaluate_linear_width_binding` consumes the source Results in the order of the binding's source slots and an optional `path_source_map`, and uses `path.bound_linear_width_values_v1_strict` (or `path.bound_linear_width_mapped_values_v1_strict` with a map) to publish RN64 per-vertex widths, a Float64 value of the target control capacity, for the variable-round stroke, outline and stroke-SDF operations. Interior vertices use the following positive-length segment and final endpoints use the preceding segment's end; a closed seam reuses its single published M width. Without a map, segment and subpath identity are a caller premise. ObjectIds are never used to discover inputs.

## Mathematics

Return the original PathSet value with the same ObjectId and geometry bits, and construct a separate path_width Result containing scalar Linear profiles, explicit source domains and source associations. The workflow pairs these two outputs. The public `generation::attach_linear_width` helper denotes that explicit pairing; it adds no field or implicit attribute to the geometry Result. The v1 path_width schema represents Linear width only. Independent PCHIP consumers use an explicit profile or separately specified bounded conversion.

## Behavior

Support class: complete input. The width metadata is built from explicit inputs, and source IDs are recorded without validation. The original geometry value is returned directly, preserving its identity.

## Acceptance

Three domains, valid source snapshot pairing and unchanged geometry bits.
