---
spec_schema_version: 1
id: FMT-10D
parent_id: FMT-10
function: convert_linear_rgb
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-10D: compose linear RGB basis conversion

Inherit the complete [FMT-10 family](FMT-10_rgb_basis_contract.md) and
[exact matrix definitions](FMT-10_basis_math.md). This is a Proposed specification,
not a registered implementation or an alias of a retired operation.

## Authoring interface and expansion

The proposed helper `convert_linear_rgb` receives a graph, an `input` edge,
known descriptor/effective metadata, explicit target basis, family fields and
one NUM CPU profile (strict by default). It returns one `values` edge handle.
It creates no native D registry key or automatic converter. Resolve and verify
source geometry against real inference; raw requires explicit source geometry.

The static white policy determines the expansion:

- require_match (default): unequal whites fail; otherwise A->B.
- preserve_xyz: A->B, with B's semantic preserve_xyz option.
- adapt: A->C->B, method required, with C output white matching B's target.

Each generated node uses the chosen profile and layout. Source override applies
to A's effective input once; later semantic nodes consume the actual inferred
intermediate descriptions in respect mode. Do not reapply the original RGB
source override to intermediate XYZ. Raw expands all stages in raw mode with
explicit frozen geometry, the same ordered slots and no source-color assertions;
raw B receives no semantic white_handling field.

Reserve collision-free node IDs, preserve existing graph nodes/exports, stage
the complete expansion and publish it only after static validation succeeds.
An authoring error leaves the original graph unchanged. The final output handle
names B's actual `values` port; no hidden reference-white relabel substitutes C.
Each intermediate keeps dtype, shape and slots, changes only applicable model
metadata, and retains bypass alpha/AOV data and ordinary owners.

## Observable computation and storage

D retains each constituent rounding and required validation/failure. It is not
RN(inverse(Mt)*K*Ms*input) with one final rounding. Equal bases do not remove A/B,
and an overall product I does not enable the native identity-copy exception.
A transformation/optimization must prove identical bits, metadata, input support
and failure outcomes before deleting stages. No such generic fusion is selected.

Requests and dirty sets compose the actual nodes. A B row of a nonidentity target
inverse requires all three intermediate XYZ components, even if it has zeros.
Those A/C outputs must exist and pass their semantic checks. AOV-only bypass
requests avoid color arithmetic, but do not waive any static parameter checks.
Resource accounting includes intermediate reservations/backing/read windows and
exact matrix preparation. Errors retain constituent attribution and ordinary
observation scope; D does not manufacture success after an intermediate failure.

Auto/materialize/view apply per generated node. A forced view requires every
constituent to be its own exact-I/legal-backing case. Normal equal-source/target
RGB conversion still traverses nonidentity A/B and therefore fails forced view.
The no-conversion path is an explicitly retained input edge, not a mathematical
simplification silently performed by D.

## Independent fixtures and future public workflow

1. Same sRGB/Rec.709 basis, Float32 RGB [1,0,0], no C: exact matrices from the
   RN64 preset with rounding after A and B yield strict output bit patterns
   [0x3f800000,0xb1356c32,0xaf63dc5c]. The small negative G/B values demonstrate
   why A->B is not an identity-copy contract. Reproduce with an independent
   rational solver and independently certified RN32 at both stage boundaries.
2. For the custom-axis basis white=(0.25,0.25), M=diag(1,1,2). Semantic input
   [1,0,max_finite], source=target, requested final R: A's required Z overflows
   because B retains all three input terms, so D fails. A requested alone at X
   succeeds. A fused identity would hide this required error and is forbidden.
3. Use source custom-axis white Ws=(1,1,2), target Wt=(2,1,1), input RGB [1,1,1].
   preserve_xyz yields [0.5,1,2]; adapt via any of the four admitted methods
   yields [1,1,1]. Omitted policy rejects unequal whites. Use exact dyadic whites
   from the native member fixtures so these are valid exact sample assertions.
4. Full and R-only queries through expanded graphs must match manual A/C/B
   composition bit-for-bit and in required input/output support, including raw
   NaN order and zero-coefficient peers. Do not use the helper itself as oracle.
5. Compare sparse/cross-tile, alpha-only, invalid unrequested external AOV and
   hidden-color cases. Forced view of ordinary same-basis conversion fails.
   Authoring invalid geometry/policy leaves graph exports and node IDs unchanged.
6. Changing only a geometry/method/policy descriptor requires reinference;
   ordinary numeric input changes propagate through the generated tuple support.
   Source/context lifetime, low budgets, intermediate failures and cancellation
   are checked on the actual expanded graph.

A future public demonstration explicitly builds A->C->B and the D helper for the
same source/target/profile, requests color and alpha separately and checks both
against independent staged arithmetic. For encoded images, FMT-09 stages remain
visible outside D. The proposal adds no runtime implementation or timing claim.
