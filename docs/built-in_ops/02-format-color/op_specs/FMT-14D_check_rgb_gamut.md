---
spec_schema_version: 1
id: FMT-14D
parent_id: FMT-14
function: check_rgb_gamut
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.check_rgb_gamut_strict
---

# FMT-14D: RGB gamut diagnostic mask

Inherits the [FMT-14 family](FMT-14_gamut_mapping_contract.md), including its normative D algorithm, interface, finite-domain raw form and errors. The strict CPU operation is the only key.

## Interface

One `input` Float32/Float64 Tensor. Output `mask` has the family non-channel shape/keepdims policy and binary diagnostic samples without attached description. Select one native relative linear RGB group, or explicit raw ordered slots. Interpretation is explicit static configuration; layout defaults to auto; materialize is supported and view is rejected. All description/geometry/member parameters are static. No basis, transfer, range encoding or tone conversion is implied.

## Mathematics

One requested mask sample consumes the complete source RGB triple. There is no alpha/AOV output. The family section gives every branch and rounding boundary. There is no spatial halo or data-dependent demand elision. Static admission runs also for empty or bypass-only requests. Hidden colors are mapped without reading alpha. Finite-domain conditions are unchecked caller premises. No sample-dependent identity branch makes the node eligible for view.

## Acceptance

(-0,1,.5) gives +0, (0,1+2^-23,.5) Float32 gives 1. A consumed NaN violates the finite caller premise; no mask value or diagnostic is specified for that input. Rank-one RGB requires keepdims=true.

Conceptual workflow: explicit source decoding/basis conversion -> this member -> diagnostic mask consumer.
