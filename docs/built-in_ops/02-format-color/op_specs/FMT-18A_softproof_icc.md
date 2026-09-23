---
spec_schema_version: 1
id: FMT-18A
parent_id: FMT-18
function: softproof_icc
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.softproof_lcms2_19_1_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-18A: ICC proof-rendered display colors

Inherit the complete [FMT-18 family](FMT-18_softproof_contract.md) and applicable
[FMT-12 foundation](FMT-12_icc_transform_contract.md). Proposed and unimplemented.

## Ports, parameters and output inference

One input Float32/Float64 tensor Value, one selected complete group (or explicit
raw ordered slots). Source profile may resolve from authoritative metadata in semantic mode; raw requires it explicitly. Proof/display profiles are mandatory. rendering_intent defaults to relative, proofing_intent to absolute, bpc to off, execution to optimized. Output values uses the display profile and preserves/remaps alpha and AOVs according to FMT-12.
All settings/resources are static. Respect is default, override is call-local,
and raw retains the family's finite/unit/structural requirements. No profile or
physical display condition is inferred from names or the current monitor.

## Algorithm and exact support

Use the specified four-stage SOFTPROOFING chain with GAMUTCHECK disabled. Preserve profile intent/BPC rules and explicit double unit adaptation; equal profiles do not imply a copy.
Any requested color consumes all source colors at that position. No halo,
alpha dependence or hidden-color suppression. Bypass-only samples remain independent bit copies.
Static construction is required even for empty requests; it does not scan pixels.

## Resources, precision and failures

Only auto/materialize layouts; forced view fails. Inherit planar reservations,
explicit page preparation, immutable lifetimes and all FMT-12 host-engine resource
hooks. B is not built or evaluated by this member.
The external engine/build identity determines numerical behavior. No native NUM
precision label or commercial CMM equivalence is claimed. Failures preserve their
actual status/reason and required observation scope with no algorithm fallback.

## Independent acceptance and conceptual use

Apply the family direct-CMM/oracle fixtures, model/unit cases, exact regions,
metadata, budget/cancellation and lifetime checks. Compare paper/black patches under relative/absolute proofing and requested/effective BPC; verify alpha bypass separately.
A conceptual workflow branches working input into proof colors and an independent
warning mask, then optionally composes an overlay. An implementation must provide
actual public workflow commands and measured fixed-profile results before claiming
runtime acceptance. Primary sources and compatibility limits are in the family.
