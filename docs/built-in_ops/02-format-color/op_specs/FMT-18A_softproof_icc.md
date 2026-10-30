---
spec_schema_version: 1
id: FMT-18A
parent_id: FMT-18
function: softproof_icc
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
proposed_operation_keys:
  - color.softproof_lcms2_19_1_cpu
---

# FMT-18A: ICC proof-rendered display colors

Inherits the [FMT-18 family](FMT-18_softproof_contract.md) and the [FMT-12 foundation](FMT-12_icc_transform_contract.md).

## Interface

One input Float32/Float64 tensor, one selected complete group (or explicit raw ordered slots). The source profile and endpoint interpretation are explicit static parameters. Proof and display profiles are mandatory. rendering_intent defaults to relative, proofing_intent to absolute, bpc to off, execution to optimized. Output `values` uses the display profile and preserves/remaps alpha and AOVs according to FMT-12. All settings and resources are static. The described and finite-only raw paths retain the family's explicit parameter, unit and structural requirements. No profile or physical display condition is inferred from names or the current monitor.

## Behavior

The specified four-stage SOFTPROOFING chain is used with GAMUTCHECK disabled. Profile intent/BPC rules and explicit double unit adaptation apply; equal profiles do not imply a copy. Any requested color consumes all source colors at that position. There is no halo, alpha dependence or hidden-color suppression; bypass-only samples are independent bit copies. Static construction is required even for empty requests. Only auto/materialize layouts; forced view fails. B is not built or evaluated by this member. The external engine configuration determines numerical behavior; no NUM precision label or commercial CMM equivalence is claimed.

## Acceptance

The family direct-CMM fixtures, model/unit cases and exact regions. Paper/black patches are compared under relative/absolute proofing and requested/effective BPC; alpha bypass is verified separately.

## Conceptual DAG

The working input branches into proof colors (A) and an independent warning mask (FMT-18B), then optionally composes an overlay.
