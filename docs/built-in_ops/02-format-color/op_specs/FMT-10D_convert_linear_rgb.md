---
spec_schema_version: 1
id: FMT-10D
parent_id: FMT-10
function: convert_linear_rgb
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_public_authoring_helper
clarification_status: complete
---

# FMT-10D: compose linear RGB basis conversion

Inherits the [FMT-10 family](FMT-10_rgb_basis_contract.md) and [exact matrix definitions](FMT-10_basis_math.md). D is a public authoring helper over the registered A/B/C Result operations; no native D key is registered.

## Interface

`format::convert_linear_rgb` receives a WorkflowDocument, an input edge, static `RgbBasisOptions`, and an optional OperationRegistry (built-ins by default). It appends registered A/B/C operations and returns B's `values` WorkflowNodeOutput. Source structure is resolved from the real input edge. The color interpretation comes from options.input_description; raw calls require explicit source geometry.

The static white policy determines the expansion:

- require_match (default): unequal whites fail; otherwise A->B.
- preserve_xyz: A->B, with B's semantic preserve_xyz option.
- adapt: A->C->B, method required, with C output white matching B's target.

Each generated node uses the chosen profile and layout. The helper derives intermediate XYZ configurations from the explicit source description and the selected geometry, and serializes each as the consuming node's `input_description`. Raw composition supplies frozen geometry and ordered slots to every stage; raw B receives no white_handling parameter.

Node IDs are collision-free, existing graph nodes/exports are preserved, and the complete expansion is published only after static validation succeeds; an authoring error leaves the original graph unchanged. The final output handle names B's `values` port. Each intermediate keeps dtype, shape and slots and retains bypass alpha/AOV data.

## Mathematics

D retains each constituent rounding, caller sample preconditions and failures. It is not RN(inverse(Mt)*K*Ms*input) with one final rounding. Equal bases do not remove A/B, and an overall product I does not enable the native identity-copy exception. Stages are not fused unless identical bits, input support and failure outcomes are proven; no such generic fusion is selected.

A B row of a nonidentity target inverse requires all three intermediate XYZ components, even if it has zeros; those A/C outputs exist and meet the caller preconditions of their consumers. AOV-only bypass requests avoid color arithmetic but do not waive static parameter checks.

Auto/materialize/view apply per generated node. A forced view requires every constituent to pass the static exact-I check; a nonempty exact-I view then fails ViewUnavailable. Ordinary equal-source/target RGB conversion still traverses nonidentity A/B and therefore fails forced view. The no-conversion path is an explicitly retained input edge.

## Acceptance

1. Same sRGB/Rec.709 basis, Float32 RGB [1,0,0], no C: exact matrices from the RN64 preset with rounding after A and B yield strict output bit patterns [0x3f800000,0xb1356c32,0xaf63dc5c]. The small negative G/B values show why A->B is not an identity copy. Independent rational solving with certified RN32 at both stage boundaries reproduces them.
2. For custom-axis white=(0.25,0.25), M=diag(1,1,2), raw input [1,0,max_finite] and source=target, A's Z becomes Inf. B retains all three terms, so final R is canonical NaN from 0*Inf. A fused identity would produce 1. The semantic version violates the intermediate finite-output precondition.
3. Source custom-axis white Ws=(1,1,2), target Wt=(2,1,1), input RGB [1,1,1]. preserve_xyz yields [0.5,1,2]; adapt via any of the four methods yields [1,1,1]. Omitted policy rejects unequal whites.
4. Full and R-only queries through expanded graphs match manual A/C/B composition bit-for-bit and in required support, including raw NaN order and zero-coefficient peers.
5. Sparse/cross-tile, alpha-only, invalid unrequested external AOV and hidden-color cases. Forced view of ordinary same-basis conversion fails. Authoring invalid geometry/policy leaves graph exports and node IDs unchanged.

D preserves the visible A/C/B stage sequence for encoded-image workflows; FMT-09 remains an explicit stage outside D.

## Conceptual DAG

`A -> [C] -> B` (C only for adapt)
