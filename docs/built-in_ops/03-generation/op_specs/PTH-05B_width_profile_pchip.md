---
spec_schema_version: 1
id: PTH-05B
parent_id: PTH-05
function: width_profile_pchip
kind: composite_workflow
category: 03-generation
status: Accepted
implementation_status: implemented_public_authoring_helper
clarification_status: resolved
---

# PTH-05B: width profile pchip

Inherits [PTH-05](PTH-05_width_profiles_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Ports and width domains as PTH-05A; values[N].

### Parameters and domain

Same unchecked domain premises; PCHIP slopes and short-table behavior inherit CRV-01B.

Public SDK helper, declared in `photospider/ops/generation.hpp`:

```cpp
Result<WorkflowNodeOutput> ps::generation::width_profile_pchip(
    WorkflowDocument* document, WorkflowInput s, WorkflowInput source_length,
    WorkflowInput stops, WorkflowInput widths,
    std::string domain = "subpath_normalized_arclength",
    ElementType output_type = ElementType::Float64);
```

`s` is Float64 `[N]`; `source_length` is Float64 `[1]` or `[N]`. Both already refer to the selected source domain (after any caller-owned source mapping). The helper publishes query operands through `path.width_query_operands_v1_strict` (a zero length yields operands 0/1), performs an explicit NUM division that publishes RN64 u, then invokes the CRV-01B PCHIP workflow. `stops` and `widths` may use either floating dtype. There is no PTH-05B operation key.

## Mathematics

Explicitly publish NUM-computed u=s/L or s (zero length0), then evaluate CRV-01B. Preserve this intermediate RN difference from A's direct expression. Shape preservation needs no post-hoc max(0,w).

## Behavior

Support class: Regional. Each query depends on the complete profile controls.

## Acceptance

Nonmonotone nonnegative widths, endpoint hits, short-table linear reduction and per-node rounding agreement.
