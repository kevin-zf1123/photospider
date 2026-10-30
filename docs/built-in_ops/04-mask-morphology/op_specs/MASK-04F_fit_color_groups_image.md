---
spec_schema_version: 1
id: MASK-04F
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: fit_color_groups_image
oracle_entry: fit_color_groups_image
registered_operation_keys:
  - mask.fit_color_groups_image_strict
---

# MASK-04F: Fit image regions by group ID

Inherits [MASK-common](MASK_common_contract.md) and the [family contract](MASK-04_color_range_contract.md).

## Interface

Registered operation key: `mask.fit_color_groups_image_strict`.

image: Float32/Float64 planar[C_image,H,W], group_ids: Int64[H,W], weights: image-dtype[H,W], epsilon: Float64[C_selected] -> model: Result, skipped: Result.

Required statics: `channels` (one complete nonperiodic color group) and `input_description`. There is no raw fitting or hidden conversion. Controls match the image's spatial basis. Unrelated alpha/AOV planes are not mathematical inputs. The output schemas, capacity (N=H*W) and fields are those of [MASK-04E](MASK-04E_fit_color_groups_table.md#registered-result-schemas) and its interface.

## Mathematics

The admitted sample multiset is exactly the positive-ID pixels in global row-major order, using selected coordinates and weights. The definition is equivalent to [MASK-04E](MASK-04E_fit_color_groups_table.md#group-statistics-and-model-output) on this table, without requiring the table to be materialized: group statistics, zero-weight skipping, regularization and the RN64 Cholesky output boundary are identical. Tile order is not a rounding rule.

## Behavior

Group IDs are Control for the whole image; image and weights at positive IDs are Data. Both Results depend globally on the admitted samples. All-ID-zero gives an empty model and empty skipped list.

## Acceptance

As [MASK-04E](MASK-04E_fit_color_groups_table.md#acceptance), with identical results for the equivalent table and image inputs.
