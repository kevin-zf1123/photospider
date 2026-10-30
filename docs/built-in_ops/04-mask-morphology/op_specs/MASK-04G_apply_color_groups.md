---
spec_schema_version: 1
id: MASK-04G
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: apply_color_groups
oracle_entry: apply_color_groups
registered_operation_keys:
  - mask.apply_color_groups_strict
---

# MASK-04G: Apply grouped color selection

Inherits [MASK-common](MASK_common_contract.md) and the [family contract](MASK-04_color_range_contract.md).

## Interface

Registered operation key: `mask.apply_color_groups_strict`.

image: Float32/Float64 planar[C_image,H,W], model: complete grouped-color Result -> values: Coverage[H,W], image dtype.

Required statics: `channels`; `input_description`; shared `inner`,`outer` finite Float64 with 0<=inner<=outer; `curve` linear|smoothstep. There are no per-group overrides, priors or hidden color conversions. The workflow supplies the same `input_description` used to interpret the model coordinates and pairs it with a compatible selected complete image group; this semantic pairing is a caller premise.

The model input has schema photospider.ops.mask.color_groups version 1 with the count/ids/mean/cholesky directory specified by [MASK-04E](MASK-04E_fit_color_groups_table.md#registered-result-schemas). Static capacity and selected component count determine the schema.

## Mathematics

Model count, IDs, numerical entries and canonical padding are unchecked caller premises. Effective IDs are positive, unique and sorted; means and lower factors are finite Float64, the upper triangle is +0 and every effective diagonal is positive. An empty model is legal. No fitting-source ObjectId match is required. The consumer evaluates only the effective prefix given by count; zero padding does not participate.

For each effective group solve L*z=x-mean in exact arithmetic on the stored binary values. Let d=sqrt(sum z^2). No rounded inverse matrix or sequential Float64 solve defines the reference. Form the shared inner/outer response as in the family and return the maximum response, with empty maximum +0. Equivalently, use the exact minimum squared distance, then apply the shared monotone response once and RN_T once. Equal distances give equal coverage; no winning-group output is defined. Group population and fitting weight are not blend weights.

## Behavior

For nonempty output selection, the output depends on the model's effective prefix and the selected image samples. An empty model produces +0 without evaluating image samples; schema and description checks still apply.

## Acceptance

Cases: valid empty and nonempty models, covariance correlation, groups with identical responses, shared threshold equality, both image dtypes, unrelated-channel exclusion and another image with compatible interpretation.
