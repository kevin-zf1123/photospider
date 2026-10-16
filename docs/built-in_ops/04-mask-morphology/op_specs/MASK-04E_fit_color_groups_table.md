---
spec_schema_version: 1
id: MASK-04E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-04
function: fit_color_groups_table
oracle_entry: fit_color_groups_table
proposed_operation_keys:
  - mask.fit_color_groups_table_strict
---

# MASK-04E: Fit grouped color samples

Inherit [MASK-common](MASK_common_contract.md) and the
[family contract](MASK-04_color_range_contract.md). Numerical rules follow
01-numeric. This spec does not claim native registration or production execution.

## Ports, parameters and output inference

samples: Float32/Float64[N,C], group_ids:Int64[N], weights: sample-dtype[N],
epsilon:Float64[C] -> model:Result, skipped:Result. N,C are positive; sample
rows form a generic numeric table, not image storage. Required statics:
source_description of one FMT nonperiodic complete color group in column order.
No implicit projection or conversion. Description C must match sample columns.
Each table row is one observation; repeated rows count repeatedly with their
specified weights. Zero rows use the existing positive-extent host rule; exclude
rows with ID zero to represent no observations. All sample rows participate in
control selection; this is complete-input over the sample axis, with typed payload
consumption restricted by group IDs as specified below.

## Group statistics and model output

Group ID zero excludes color and weight reads; every other ID must be positive.
All positive-ID samples are validated even with weight zero. Weights are finite
and nonnegative in the sample dtype. For every group use exact W=sum w,
mu=sum(w*x)/W and Sigma=sum(w*(x-mu)*(x-mu)^T)/W, then
Sigma_eff=Sigma+diag(epsilon^2). No rounded mean intermediate or sample correction.
A zero-W group is skipped with diagnostic ID; validation failure is not a skip.
No automatic grouping, reassignment, clipping or outlier rejection.

Two named static Result outputs derive from one prepared shared fitting stage.
Use capacity N equal to the input sample count (H*W for the image member),
checked during preparation:

* model: count:Int64[1], ids:Int64[N], mean:Float64[N,C],
  cholesky:Float64[N,C,C]. Effective rows are sorted by positive ID. The upper
  Cholesky triangle is +0. Input color interpretation is explicit configuration.
* skipped: count:Int64[1], ids:Int64[N], with an ascending effective prefix of
  IDs having zero total weight. The reason is zero_total_weight by contract.

Each count may be zero. Unused entries are zero-filled; positive-ID and positive-
diagonal constraints apply only to effective model rows. Both counts are ordinary
payload requiring Needs. No absent ID creates an effective row. The complete
schema/codec and fixed publication scopes remain registration gates for these
Proposed members. The global fitting computation is not repeated per output tile.
Source ownership follows the host contract; applying the model to another
compatible image requires no source ObjectId match.

Compute the exact positive-diagonal Cholesky factor of Sigma_eff; emit RN64(mean)
and RN64(each lower factor entry). This is the fitting output rounding boundary.
An exact rational LDL^T plus correctly rounded square roots is a possible
reference. Finite overflow or loss of a positive representable diagonal fails;
no adaptive increase of epsilon. Sample orders and tile partitions cannot alter
these numeric values. epsilon is a required Float64[C] Tensor of positive finite
scales; its units match each native color coordinate. The model represents the
actual stored mean and L, with effective covariance L*L^T.

## Ownership, validation and acceptance

For any nonempty fitting Result request, validate all group IDs and consume only
positive-ID sample/weight payloads, plus all epsilon values. Both output schemas
can be inferred without reading group values. Selected semantic groups are
complete; applicable descriptions are checked even when all IDs are zero.
Both Results depend globally on the admitted samples; no tile-local models or
partial tables are published. All-ID-zero gives an empty model and empty skipped
list. Capacity/work limits and cancellation fail without partial publication.
Account for group maps, exact accumulators, factor/refinement state and both
outputs; grouping complexity is not capped by maximum ID. No hidden spill.

The numerical oracle omits actual color descriptors and native Result codecs.
Acceptance includes noncontiguous IDs, order invariance, zero weights with
nonfinite colors (error), ID-zero nonfinite payloads (unconsumed), empty models,
zero-W diagnostics, single-sample regularization, correlated dimensions,
Float32/64 input, exact LDL^T reconstruction, and matching table/image inputs.
Native public workflows additionally test metadata, resource bounds, immutable
lifetimes, atomic failure and actual source reads. [S27](../research-sources.md#s27)
provides mathematical reference context; all choices above are explicit MASK rules.

Oracle entry: `fit_color_groups_table` in [reference.py](../../../../oracle/ops/mask_morphology/mask04/reference.py).
