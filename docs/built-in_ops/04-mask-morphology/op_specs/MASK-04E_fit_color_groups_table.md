---
spec_schema_version: 1
id: MASK-04E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: draft_for_review
parent_id: MASK-04
function: fit_color_groups_table
oracle_entry: fit_color_groups_table
registered_operation_keys:
  - mask.fit_color_groups_table_strict
---

# MASK-04E: Fit grouped color samples

Inherits [MASK-common](MASK_common_contract.md) and the [family contract](MASK-04_color_range_contract.md).

## Interface

Registered operation key: `mask.fit_color_groups_table_strict`.

samples: Float32/Float64[N,C], group_ids: Int64[N], weights: sample-dtype[N], epsilon: Float64[C] -> model: Result, skipped: Result.

N,C are positive; sample rows form a generic numeric table, not image storage. Required statics: `input_description` of one FMT nonperiodic complete color group in column order; its C matches the sample columns. There is no implicit projection or conversion. Each table row is one observation; repeated rows count repeatedly with their specified weights. Zero rows are not representable; exclude rows with ID zero to represent no observations. epsilon is a Float64[C] tensor of positive finite scales whose units match each native color coordinate.

Both named outputs derive from one fitting computation over all sample rows, with capacity N equal to the input sample count:

* model: count:Int64[1], ids:Int64[N], mean:Float64[N,C], cholesky:Float64[N,C,C]. Effective rows are sorted by positive ID. The upper Cholesky triangle is +0.
* skipped: count:Int64[1], ids:Int64[N], with an ascending effective prefix of IDs having zero total weight. The reason is zero_total_weight by contract.

Each count may be zero. Unused entries are zero-filled; positive-ID and positive-diagonal constraints apply only to effective model rows. No absent ID creates an effective row. Applying the model to another compatible image requires no source ObjectId match.

## Group statistics and model output

Group ID zero excludes color and weight from the fit; every other ID is positive as an unchecked premise. All positive-ID samples are finite even with zero weight; weights are finite and nonnegative in the sample dtype.

For every group use exact W=sum w, mu=sum(w*x)/W and Sigma=sum(w*(x-mu)*(x-mu)^T)/W, then Sigma_eff=Sigma+diag(epsilon^2). There is no rounded mean intermediate and no sample-count correction. A zero-W group is skipped with its ID listed on valid input. There is no automatic grouping, reassignment, clipping or outlier rejection. If all IDs are zero the model and skipped list are empty.

Compute the exact positive-diagonal Cholesky factor of Sigma_eff; emit RN64(mean) and RN64(each lower factor entry). This is the fitting output rounding boundary. Finite stored entries and a positive representable diagonal are caller premises, with no adaptive increase of epsilon. Sample order does not alter these values. The model represents the actual stored mean and L, with effective covariance L*L^T.

## Registered Result schemas

The operations SDK provides the factories `color_group_model_schema(N,C)` and `skipped_color_groups_schema(N)`. Their schema IDs are photospider.ops.mask.color_groups and photospider.ops.mask.skipped_color_groups, both version 1. Member order is exactly the directory above; no field has spatial/channel axes. mean has one atomic trailing axis and cholesky has two, so a row's coordinates and factor are never split. Each Result is published as one complete bundle covering its directory, and both named outputs belong to the same call. Capacity is positive and every field has at most 2^40 samples. Consumers trust the ordinary count and canonical padding premises.

## Behavior

For any nonempty fitting Result selection, all group IDs, positive-ID samples and weights, and all epsilon values are consumed under their unchecked premises. Both output schemas are inferred without reading group values. Both Results depend globally on the admitted samples; no tile-local models or partial tables exist.

## Acceptance

Cases: noncontiguous positive IDs, order invariance, zero weights with finite colors, ID-zero samples excluded from the formula, empty models, zero-W skipped IDs, single-sample regularization, correlated dimensions, Float32/64 input, exact LDL^T reconstruction, and matching table/image inputs.

## References

[S27](../research-sources.md#s27) provides mathematical reference context; all choices above are explicit MASK rules.
