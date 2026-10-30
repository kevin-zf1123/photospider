---
spec_schema_version: 1
id: FMT-11S
parent_id: FMT-11
function: gray_to_black_white
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_public_helper
clarification_status: complete
proposed_template_names:
  - color.gray_to_black_white
---

# FMT-11S: Gray to binary Black/White

Inherits the [family contract](FMT-11_model_conversion_contract.md), [normative mathematics](FMT-11_model_math.md) and [relative-coordinate scale](FMT_relative_coordinate_scale.md). S is a public helper that appends the registered `mask.threshold_channel_<profile>` operation; there is no native color-model key for S.

## Interface

One Result containing a single Float32 or Float64 tensor produces one `values` Result containing a single tensor. Source: one explicitly described native Gray component. Target: binary Black/White; the caller retains the Gray origin in its configuration. Shape, channel axis/count and slots are preserved. Unrelated groups, internal alpha and AOV samples pass through bit-for-bit. An explicit input_description selects the described path; otherwise the family raw selector applies. The output carries no description. `threshold` is a required finite Float64 in the native Gray unit, with no implicit 0.5.

## Mathematics

The hard comparison `x >= threshold` selects exact typed 1, otherwise +0. The finite-input precondition applies to semantic use; raw NaN compares false.

## Behavior

Only the selected Gray is consumed; all other channels bypass. The operation materializes output and rejects forced view. Empty demand emits no payload. Integer packing stays in codecs.

## Acceptance

Gray l=[0.25,0.5,0.75], threshold=0.5 yields [0,1,1]. Semantic NaN is outside the caller precondition. Also cover both dtypes, signed zeros and threshold neighbors.

## Conceptual DAG

`Q -> S -> explicit FMT-06/codec if needed`
