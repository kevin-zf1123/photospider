---
spec_schema_version: 1
id: GEN-06A
parent_id: GEN-06
function: spread_coordinate
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - generation.spread_coordinate_v1_strict
  - generation.spread_coordinate_v1_accelerated_apple_silicon
  - generation.spread_coordinate_v1_accelerated_x86_64
---

# GEN-06A: spread coordinate

Inherits [GEN-06](GEN-06_gradient_lookup_contract.md) and [GEN common](GEN_common_contract.md).

## Interface

Operation keys: `generation.spread_coordinate_v1_strict`, `generation.spread_coordinate_v1_accelerated_apple_silicon`, `generation.spread_coordinate_v1_accelerated_x86_64`.

Floating t of any legal complete sample_shape, including batch axes; values retains the logical sample shape and dtype with explicit output axes.

### Parameters and domain

method=pad/repeat/reflect; finite inputs; no canvas.

## Mathematics

pad clamps to [0,1] with NUM copy/signed-zero rules. repeat=t-floor(t); reflect=1-abs(1-(t-2*floor(t/2))). Exact boundary decisions, then normal RN. Repeat rounding to 1 stays 1; downstream lookup treats it as endpoint. Exact periodic boundary is +0.

## Behavior

Support class: Regional.

## Acceptance

For -0.25,0,1,1.25, repeat gives 0.75,0,0,0.25 and reflect gives 0.25,0,1,0.75. Pad preserves input -0.
