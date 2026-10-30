---
spec_schema_version: 1
id: PTH-12F
status: Accepted
implementation_status: implemented
registration_status: registered
operation_keys:
  - path.boolean_grid_float64_v1_strict
---

# PTH-12F: Explicit-grid Boolean with Float64 output

Inherits [PTH-12](PTH-12_boolean_offset_contract.md), [GEN common](GEN_common_contract.md) and [PTH geometry](PTH_geometry_contract.md).

## Interface

Operation keys: `path.boolean_grid_float64_v1_strict`.

Inputs and quantized Boolean semantics match PTH-12D. Outputs are `path`, a Float64 CoreVerbs PathSet, and a report with both the quantization and the publication diagnostics. Parameters are those of PTH-12D (grid origin and spacing, operation, fill rules, attribute policy, capacities) plus a positive publication displacement budget.

## Mathematics

Execute the PTH-12D grid algorithm, map grid coordinates to exact geometric coordinates `origin + spacing * integer`, then round once to Float64 per coordinate. The result equals an explicit PTH-12D result followed by that evaluation and one RN64 per coordinate. The exact-cycle order, duplicate snapped vertices and degenerate contours are retained; no post-publication reclipping or canonicalization changes them.

The publication displacement bound is accumulated during construction with the exact squared-error and RU64 rule of PTH-12E, measured in geometric pixel coordinates after applying origin and spacing: the publication stage is RU64 of the square root of the maximum exact squared coordinate displacement. The quantization stage equals PTH-12D. Finite representability, the chosen publication budget, preserved incidence/topology and distinct required grid points remaining distinct in Float64 are unchecked caller preconditions; no output scan or publication gate checks them. The report has `topology_status=0` (unverified). Original-input exactness is not claimed.

## Behavior

Support class: complete input. A failed call exports no successful Result.

## Acceptance

Large grid indices, non-power-of-two spacing, valid topology-preserving publication, and scale/origin identity.
