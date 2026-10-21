---
spec_schema_version: 1
id: FMT-18B
parent_id: FMT-18
function: check_icc_gamut
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.gamut_alarm_lcms2_19_1_source_dims_v1_cpu
---

# FMT-18B: Independent ICC gamut alarm

Inherit the complete [FMT-18 family](FMT-18_softproof_contract.md) and applicable
[FMT-12 foundation](FMT-12_icc_transform_contract.md). The CPU implementation is available; the decision status remains Proposed. See [implementation, validation and performance](../FMT-12-13-18_implementation.md).

## Ports, parameters and output inference

One input Float32/Float64 tensor Tensor, one selected complete group (or explicit
raw ordered slots). Source profile and endpoint interpretation are explicit static parameters. Proof profile is mandatory. source_intent defaults to relative; no display profile, BPC, alarm color, execution switch or user threshold. Output mask uses family keepdims rules and exactly +0/1 diagnostic samples.
All settings/resources are static. The described and finite-only raw paths
retain the family's explicit parameter, unit and structural requirements. No profile or
physical display condition is inferred from names or the current monitor.

## Algorithm and exact support

Use the mandatory source-dimensional CLUT correction and read the borrowed gamut pipeline directly. Validate its dimensions and ownership. Float32 normalized input passes through the pinned u16 table evaluation; output is 1 exactly when score>0, with score finiteness an unchecked premise. Alarm-color comparison is forbidden.
Any requested color/mask consumes all source colors at that position. No halo,
alpha dependence or hidden-color suppression. There are no alpha/AOV outputs or unrelated display-transform pixel requests.
Static construction is required even for empty requests; it does not scan pixels.

## Resources, precision and failures

Only auto/materialize layouts; forced view fails. Inherit planar reservations,
explicit page preparation, immutable lifetimes and all FMT-12 host-engine resource
hooks. The complete static alarm grid and helper transforms are budgeted;
construction budget or sampler failure invalidates the build and never produces
a successful all-zero mask. Construction has no cancellation checks; pixel
execution remains cancellable.
The external engine/build identity determines numerical behavior. No native NUM
precision label or commercial CMM equivalence is claimed. Failures preserve their
actual status/reason and required observation scope with no algorithm fallback.

## Independent acceptance and conceptual use

Apply the family direct-CMM/oracle fixtures, model/unit cases, exact regions,
metadata, budget/cancellation and lifetime checks. Verify RGB-to-CMYK and CMYK-to-RGB source grid dimensions, K-only variation, q=0/q=1, same-count stock parity and alarm-color independence.
A conceptual workflow branches working input into proof colors and an independent
warning mask, then optionally composes an overlay. Public API examples and
measured fixed-profile results are in [implementation, validation and performance](../FMT-12-13-18_implementation.md). Primary sources and
compatibility limits remain in the family.
