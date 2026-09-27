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
interpretation respect|override with conditional source_description; shared
inner,outer finite Float64, 0<=inner<=outer; curve linear|smoothstep.
No per-group overrides, priors or hidden color conversions. Model coordinate
roles and effective description must match the selected complete image group.

## Exact model consumption and response

Validate all model IDs, counts, dimensions and floating entries before use.
IDs are unique positive and sorted. Means and lower factors are finite Float64;
upper triangle must be +0 and every diagonal positive. An empty model is legal.
There is no requirement that the image share the fitting source ObjectId.

For each effective group solve L*z=x-mean in exact arithmetic on the stored
binary values. Let d=sqrt(sum z^2). No rounded inverse matrix or sequential
Float64 solve defines the reference. Form the shared inner/outer response as in
the family and return the maximum response, with empty maximum +0. Equivalently,
use the exact minimum squared distance, then apply the shared monotone response
once and RN_T once. Equal distances give equal coverage; no winning-group output
is defined. Group population and fitting weight are not blend weights.

## Demand, errors and acceptance

For nonempty Q consume the complete model and selected image samples at Q for
a nonempty model, including all groups needed by the minimum. No response=1
short-circuit waives required input/model validation. For an empty model, return
+0 without image sample reads; descriptors and coordinate interpretation remain
checked. There is no full-image statistics stage. Model edits may dirty all output;
image edits follow the selected image/validation support. Empty Q is descriptor
only. Account for model retention and exact triangular-solve scratch; poll across
pixels/groups. Invalid model structure/domain fails rather than repairing it.

Acceptance covers empty models, duplicate/nonpositive IDs, invalid triangles,
negative/zero pivots, covariance correlation, groups with identical responses,
shared threshold equality, both image dtypes, unrelated-channel exclusion and
application to another image with matching descriptions. Numeric oracle inputs
carry model ids/mean/cholesky only; real metadata codecs, public graph execution,
Region reads, budgets and ownership require separate native validation.

Oracle entry: `apply_color_groups` in [reference.py](../../../../oracle/ops/mask_morphology/reference.py).
