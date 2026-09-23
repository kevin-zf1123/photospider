---
spec_schema_version: 1
id: FMT-10A
parent_id: FMT-10
function: rgb_to_xyz
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.rgb_to_xyz_strict
  - color.rgb_to_xyz_accelerated_apple_silicon
  - color.rgb_to_xyz_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-10A: linear RGB to XYZ

Inherit the complete [FMT-10 family](FMT-10_rgb_basis_contract.md) and
[exact matrix definitions](FMT-10_basis_math.md). This is a Proposed specification,
not a registered implementation or an alias of a retired operation.

## Interface and interpretation

One `input` tensor Value produces same-dtype/same-shape `values`. Semantic input
selects one complete linear RGB group, resolves its primary/white geometry from
effective metadata and checks any explicit assertions. Raw provides its source
basis and ordered component triple explicitly. Source transfer decoding and
non-native semantic code scaling must be done before A, never implicitly here.

Construct M from the exact resolved xy coordinates. For requested transformed
row i, output RN_dtype(sum_j M[i,j]*RGB[j]+0). Read the full source RGB triple
at that spatial coordinate for every nonidentity transformed request, even if
some coefficients are zero. Semantic mode requires those inputs and the
requested rounded output finite. Unrequested matrix rows are not evaluated.

Write X/Y/Z into the original R/G/B slots and publish XYZ with the source white,
units, observer and reference. Drop inapplicable RGB-only fields from that group;
free-form names, coordinates, internal alpha and unrelated groups survive.
Raw preserves applicable original interpretation without claiming XYZ conversion.
A has no destination white, adaptation method or arbitrary matrix parameter.

## Mapping, storage and errors

Inherit complete-triple point support for nonidentity, pointwise native identity
copy, exact bypass support, dirty mapping and auto/view/materialize. Native exact
I may copy bits while publishing new model roles; approximate I may not.
All geometry/selector/representation checks apply before pixels, including Empty
or alpha-only observations. Failure of a consumed peer is not suppressed by a
zero coefficient or by source alpha=0. No whole-image scan is introduced.

Semantic overflow occurs only for a requested row's rounded result; raw keeps
NUM special values. Required upstream failures, resource/exact-coefficient
budgets, cancellation, prepared planar pages and normal owner lifetime inherit
the family. No source invalidation or relabel mutates another consumer.

## Independent fixtures and future public workflow

1. Custom primaries R=(1,0), G=(0,1), B=(0,0), white=(0.25,0.25) are valid
   homogeneous axes with y=0 primaries. They give M=diag(1,1,2). Input [1,-2,3]
   maps exactly to XYZ [1,-2,6] in both dtypes, preserving negative samples.
2. For any admitted preset, M*[1,1,1] equals exact W before final rounding.
   Verify actual stored output against RN_dtype(W), not a separately rounded
   published matrix. Include AP0's virtual blue primary and both dtypes.
3. Duplicate primary columns, a zero normalization scale or malformed white
   fail statically. Valid exactly invertible near-singular cases use the exact
   oracle; lack of numeric work capacity reports resource failure, not an
   invented epsilon-singular classification.
4. For the diagonal fixture, request only X with finite R=1,G=0,B=max_finite.
   X succeeds with 1 although the unused Z would overflow. Put NaN in G instead:
   semantic X fails, and raw X returns that quieted source NaN despite M[X,G]=0.
5. Nonidentity input [-0,-0,-0] yields +0 for each exact-zero output under the
   three-term plus +0 NUM rule. This is different from the native exact-I copy
   rule; a row alone looking like identity does not select a whole-node shortcut.
6. Noncanonical R/G/B slots, sparse cross-tile requests and alpha-only output
   preserve positions and exact support. Alpha NaN does not affect requested
   color; hidden nonfinite color still fails at alpha=0.

When implemented, a public graph should bind planar linear RGB plus alpha,
select a source preset/custom geometry, request X and alpha separately and
compare to independent rational results. A second graph explicitly puts FMT-09A
before A for an encoded source. This document supplies no executable example
using nonexistent APIs and makes no runtime claim.
