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

Inherits the [FMT-18 family](FMT-18_softproof_contract.md) and the [FMT-12 foundation](FMT-12_icc_transform_contract.md).

## Interface

One input Float32/Float64 tensor, one selected complete group (or explicit raw ordered slots). The source profile and endpoint interpretation are explicit static parameters. The proof profile is mandatory. source_intent defaults to relative; there is no display profile, BPC, alarm color, execution switch or user threshold. Output `mask` uses the family keepdims rules and exactly +0/1 diagnostic samples. All settings and resources are static. The described and finite-only raw paths retain the family's explicit parameter, unit and structural requirements. No profile or physical display condition is inferred from names or the current monitor.

## Behavior

The mandatory source-dimensional CLUT correction is used and the gamut pipeline is read directly; its dimensions are validated. Float32 normalized input passes through the pinned u16 table evaluation; the output is 1 exactly when score>0, with score finiteness an unchecked premise. Alarm-color comparison is forbidden.

Any requested mask sample consumes all source colors at that position. There is no halo, alpha dependence or hidden-color suppression, and no alpha/AOV outputs. Static construction is required even for empty requests. The complete static alarm grid and helper transforms are built; construction failure invalidates the build and never produces a successful all-zero mask. Only auto/materialize layouts; forced view fails. The external engine configuration determines numerical behavior; no NUM precision label or commercial CMM equivalence is claimed.

## Acceptance

The family direct-CMM fixtures, model/unit cases and exact regions. RGB-to-CMYK and CMYK-to-RGB source grid dimensions, K-only variation, q=0/q=1, same-count stock parity and alarm-color independence.

## Conceptual DAG

The working input branches into proof colors (FMT-18A) and an independent warning mask (B), then optionally composes an overlay.
