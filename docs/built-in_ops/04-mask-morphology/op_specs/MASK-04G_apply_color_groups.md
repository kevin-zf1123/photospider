---
spec_schema_version: 1
id: MASK-04G
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-04
function: apply_color_groups
oracle_entry: apply_color_groups
proposed_operation_keys:
  - mask.apply_color_groups_strict
---

# MASK-04G: Apply grouped color selection

Inherit [MASK-common](MASK_common_contract.md) and the
[family contract](MASK-04_color_range_contract.md). Numerical rules follow
01-numeric. This spec does not claim native registration or production execution.

## Ports, parameters and output inference

image: Float32/Float64 planar[C_image,H,W], model:complete grouped-color Result
-> values: Coverage[H,W], image dtype. Required statics: channels;
input_description; shared
inner,outer finite Float64, 0<=inner<=outer; curve linear|smoothstep.
No per-group overrides, priors or hidden color conversions. The workflow supplies the same input_description used to interpret the model
coordinates and pairs it with a compatible selected complete image group;
this semantic pairing is a caller premise.

## Exact model consumption and response

Preparation checks model schema and dimensions and the explicit description.
Model count, IDs, numerical entries and canonical padding are unchecked caller
premises. Effective IDs are positive, unique and sorted; means/lower factors are
finite Float64, upper triangle is +0 and every effective diagonal is positive.
An empty model is legal. No fitting-source ObjectId match is required.

For each effective group solve L*z=x-mean in exact arithmetic on the stored
binary values. Let d=sqrt(sum z^2). No rounded inverse matrix or sequential
Float64 solve defines the reference. Form the shared inner/outer response as in
the family and return the maximum response, with empty maximum +0. Equivalently,
use the exact minimum squared distance, then apply the shared monotone response
once and RN_T once. Equal distances give equal coverage; no winning-group output
is defined. Group population and fitting weight are not blend weights.

## Demand, errors and acceptance

For nonempty output selection, consume the model's effective prefix and selected
image samples for a nonempty model, including all groups needed by the minimum.
An empty model produces +0 without evaluating image samples; schema and explicit
description checks still apply. Compile a fixed input-tile superset covering the
complete model capacity and the image support for all selected output tiles;
runtime count never extends the Plan. No input/model scan or publication
validator is added. Account for model retention and exact triangular-solve
scratch, and poll across pixels/groups.

Production acceptance uses valid empty/nonempty models, covariance correlation,
groups with identical responses, shared threshold equality, both image dtypes,
unrelated-channel exclusion and another image with compatible interpretation.
Duplicate/nonpositive IDs, invalid triangles and nonpositive pivots are oracle
input diagnostics only. Numeric oracle inputs carry ids/mean/cholesky; actual SDK
schemas, public execution, fixed reads, budgets and ownership require native checks.

Oracle entry: `apply_color_groups` in [reference.py](../../../../oracle/ops/mask_morphology/mask04/reference.py).

The consumer reads count from its prepared fixed input bindings and evaluates only
the effective prefix. Zero padding does not participate in response evaluation
and is not scanned for canonicality. MASK-04E/F define capacity and zero-fill.
