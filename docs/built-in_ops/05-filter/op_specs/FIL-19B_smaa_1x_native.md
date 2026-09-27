---
spec_schema_version: 1
id: FIL-19B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: FIL-19
function: smaa_1x_native
proposed_operation_keys:
- filter.smaa_1x_native_strict
numeric_reference: unresolved native specification
oracle_scope: native_reference_and_pinned_third_party_comparison
research_sources:
- S08
---

# FIL-19B: smaa_1x_native

Native static SMAA 1x implementation target. The implementation must be compared with a fixed third-party implementation, but no third-party engine is a runtime dependency. This member is independent from the custom `directional_post_aa_v1` member and does not define temporal antialiasing.

Inherit [FIL-19](FIL-19_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Ports and semantic domain

The proposed input is an opaque semantic image plus an optional edge-guide input; output is a static antialiased image. Any selected color group must not carry alpha association. The caller must extract straight color or composite against an explicit background before invocation. No automatic alpha scan, alpha removal, or background selection is performed. Exact port schema, color-domain profile, and guide semantics remain to be specified with the native algorithm.

## Parameters and algorithm definition

All algorithm parameters and resource identities must be explicit; there are no constructor defaults. The native specification must pin the SMAA algorithm revision, edge detection and threshold rules, search behavior, sampling positions, boundary mapping, lookup data (if any), shader or arithmetic stages, and precision profile. The native implementation must not be described as an adapter and must not require loading an external engine at runtime.

The available decision establishes the native implementation and third-party comparison route, but does not select a specific third-party implementation, version, resource bundle, license evidence, quality metric, tolerance, sample set, or exposed comparison stages. Those facts remain unresolved; none are invented here. The exact native mathematical and numerical reference is therefore unresolved and must be completed before acceptance.

## Demand and execution

Demand, image boundaries, descriptor inference, resource admission, cancellation, output ownership, and failure behavior inherit the FILTER shared contract. The final spatial dependency class and resource cost must be derived from the pinned native sampling/search definition. Do not claim Whole, bounded halo, tileability, or resource bounds until that definition is completed.

## Acceptance

Acceptance requires (1) an independent oracle for the pinned native profile, (2) fixed third-party implementation and version, inputs and settings, (3) aligned observable stages and an explicit list of unavailable internal stages, and (4) numerical and image-quality comparisons with defined metrics and tolerances. Native NUM conformance and third-party comparison are separate checks. No comparison or runtime execution has been performed by this specification.

## Source

[S08 · Jimenez et al.: SMAA](../research-sources.md#s08). The paper is background evidence only; it does not fill the unresolved implementation profile or comparison parameters.
