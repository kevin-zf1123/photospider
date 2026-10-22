---
spec_schema_version: 1
id: FMT-10A
parent_id: FMT-10
function: rgb_to_xyz
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - color.rgb_to_xyz_strict
  - color.rgb_to_xyz_accelerated_apple_silicon
  - color.rgb_to_xyz_accelerated_x86_64
---

# FMT-10A: linear RGB to XYZ

Inherit the complete [FMT-10 family](FMT-10_rgb_basis_contract.md) and
[exact matrix definitions](FMT-10_basis_math.md). This specification remains Proposed; the keys are registered fixed invocation ABI 8
operations under the strict, accelerated Apple Silicon and accelerated x86-64
CPU profiles.

## Interface and interpretation

One input Result with one Float32 or Float64 tensor produces one
`values` output Result containing one tensor. The output preserves Result schema
identity, tensor key, logical shape and batch axes; it carries no description and sets `atomic_trailing_axes` to zero. Complete sample rank,
including batch and cell axes, is at most 8; sample count is at most 2^40. Semantic input
selects one complete linear RGB group, resolves its primary/white geometry from
explicit input_description and checks any explicit assertions. Raw provides its source
basis and ordered component triple explicitly. Source transfer decoding and
non-native semantic code scaling must be done before A, never implicitly here.

Construct M from the exact resolved xy coordinates. For each transformed
row i in a retained output tile, output RN_dtype(sum_j M[i,j]*RGB[j]+0). Read the full source RGB triple
at that spatial coordinate for every nonidentity transformed request, even if
some coefficients are zero. Semantic callers guarantee finite consumed inputs and finite rounded outputs throughout the retained output tile. Rows outside retained tiles are not evaluated.

Write X/Y/Z into the original R/G/B slots on the source white/unit/reference
scale. No description is attached; callers supply the resulting XYZ configuration
to later consumers. Alpha and unrelated sample values are copied.
A has no destination white, adaptation method or arbitrary matrix parameter.

## Mapping, storage and errors

Inherit complete-triple point support for nonidentity, pointwise native identity
copy, source-grid tile closure and auto/view/materialize. Native exact
I may copy bits; destination interpretation is supplied explicitly by the caller.
Approximate I may not select this path. Static selector/representation checks
apply before pixels, including Empty or alpha-only output. Geometry validity is
an unchecked numerical caller premise. Failure of a consumed peer is not suppressed by a
zero coefficient or by source alpha=0. No whole-image scan is introduced.

Semantic sample validity is an unchecked caller precondition; raw keeps NUM special values. Required upstream failures, resource/exact-coefficient
budgets, cancellation, prepared Result coverage and normal owner lifetime inherit
the family. No source invalidation or relabel mutates another consumer.

## Independent fixtures

1. Custom primaries R=(1,0), G=(0,1), B=(0,0), white=(0.25,0.25) are valid
   homogeneous axes with y=0 primaries. They give M=diag(1,1,2). Input [1,-2,3]
   maps exactly to XYZ [1,-2,6] in both dtypes, preserving negative samples.
2. For any admitted preset, M*[1,1,1] equals exact W before final rounding.
   Verify actual stored output against RN_dtype(W), not a separately rounded
   published matrix. Include AP0's virtual blue primary and both dtypes.
3. Duplicate primary columns, a zero normalization scale or numerically invalid
   white violate caller premises and have no required failure behavior. Malformed
   parameter syntax/type remains a static error. Valid exactly invertible
   near-singular cases use the exact oracle; exhausted arithmetic capacity
   reports resource failure without an epsilon-singular classification.
4. For the diagonal fixture, request only X with finite R=1,G=0,B=max_finite.
   Use a tile grid retaining X without Z. X succeeds with 1 although Z would overflow. Raw X with NaN in G returns that quieted source NaN despite M[X,G]=0; this NaN input is outside semantic caller preconditions.
5. Nonidentity input [-0,-0,-0] yields +0 for each exact-zero output under the
   three-term plus +0 NUM rule. This is different from the native exact-I copy
   rule; a row alone looking like identity does not select a whole-node shortcut.
6. Noncanonical R/G/B slots, sparse cross-tile requests and alpha-only output
   preserve positions and exact support. Alpha NaN does not affect requested
   color. Alpha=0 does not waive the finite semantic color precondition.

Use the installed `format::rgb_to_xyz` helper to append A to a workflow, then
compile and execute the resulting Result graph. Keep transfer decoding and any
other required stage explicit before A. Fixed invocation behavior is described in
the linked implementation documentation; the mathematical fixtures above
remain the independent numerical acceptance targets.
