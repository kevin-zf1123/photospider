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

Inherit [MASK-common](MASK_common_contract.md) and the
[family contract](MASK-04_color_range_contract.md). Numerical rules follow
01-numeric. This Proposed member is registered in the bundled strict CPU set
through fixed invocation ABI 8.

## Ports, parameters and output inference

samples: Float32/Float64[N,C], group_ids:Int64[N], weights: sample-dtype[N],
epsilon:Float64[C] -> model:Result, skipped:Result. N,C are positive; sample
rows form a generic numeric table, not image storage. Required statics:
input_description of one FMT nonperiodic complete color group in column order.
No implicit projection or conversion. Description C must match sample columns.
Each table row is one observation; repeated rows count repeatedly with their
specified weights. Zero rows use the existing positive-extent host rule; exclude
rows with ID zero to represent no observations. All sample rows participate in
control selection; this is complete-input over the sample axis, with typed payload
consumption restricted by group IDs as specified below.

## Group statistics and model output

Group ID zero excludes color and weight from mathematical support; every other
ID is positive as an unchecked premise. All positive-ID samples are finite even
with zero weight; weights are finite and nonnegative in the sample dtype. No
production pass validates these premises. For every group use exact W=sum w,
mu=sum(w*x)/W and Sigma=sum(w*(x-mu)*(x-mu)^T)/W, then
Sigma_eff=Sigma+diag(epsilon^2). No rounded mean intermediate or sample correction.
A zero-W group is skipped with diagnostic ID on valid input; violated numerical
premises have no diagnostic or skipped-group guarantee.
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
payload included in the compiled fixed reads. No absent ID creates an effective row. The SDK schemas below define the
registered fixed publication scopes. The global fitting computation runs once
per call, including a request for only one output, and is not repeated per tile.
Source ownership follows the host contract; applying the model to another
compatible image requires no source ObjectId match.

Compute the exact positive-diagonal Cholesky factor of Sigma_eff; emit RN64(mean)
and RN64(each lower factor entry). This is the fitting output rounding boundary.
An exact rational LDL^T plus correctly rounded square roots is a possible
reference. Finite stored entries and a positive representable diagonal are
caller premises, with no output scan or adaptive increase of epsilon. Sample orders and tile partitions cannot alter
these numeric values. epsilon is a required Float64[C] Tensor of positive finite
scales; its units match each native color coordinate. The model represents the
actual stored mean and L, with effective covariance L*L^T.

## Registered Result schemas

The operations SDK header [photospider/ops/mask.hpp](../../../../plugins/ops/include/photospider/ops/mask.hpp)
provides color_group_model_schema(N,C) and skipped_color_groups_schema(N).
Their schema IDs are photospider.ops.mask.color_groups and
photospider.ops.mask.skipped_color_groups, both version 1. Member order is
exactly the directory above; all fields have no spatial/channel axes. mean has
one atomic trailing axis and cholesky has two, so a row's coordinates/factor
cannot be split across tiles. Each Result has one CompleteBundle scope covering
its complete directory. Both named outputs belong to the same call retirement.

Factories allocate owned metadata, with no tensor backing or attached color
description. Capacity is positive and every field has at most 2^40 samples.
A user-built model publishes its buffers with an explicit StridedLayout matching
that storage; field directory order does not imply physical byte strides.
The fitting producer initializes the whole capacity and writes each output
exactly once. Consumers trust the ordinary count and canonical padding premises.

## Ownership, fixed support and acceptance

For any nonempty fitting Result selection, consume all group IDs, positive-ID
samples/weights, and all epsilon values under their unchecked premises. Lower
this runtime selection to fixed reads covering the static input capacities. Both output schemas
can be inferred without reading group values. Selected semantic groups are
complete; applicable descriptions are checked even when all IDs are zero.
Both Results depend globally on the admitted samples; no tile-local models or
partial tables are published. All-ID-zero gives an empty model and empty skipped
list. Capacity/work limits and cancellation fail without partial publication.
Account for group maps, exact accumulators, factor/refinement state and both
outputs; grouping complexity is not capped by maximum ID. No hidden spill.

The numerical oracle omits actual color descriptors and native Result codecs.
Acceptance includes noncontiguous positive IDs, order invariance, zero weights
with finite colors, ID-zero samples excluded from the formula, empty models,
zero-W diagnostics, single-sample regularization, correlated dimensions,
Float32/64 input, exact LDL^T reconstruction, and matching table/image inputs.
Native public workflows additionally test metadata, resource bounds, immutable
lifetimes, atomic failure and actual source reads. [S27](../research-sources.md#s27)
provides mathematical reference context; all choices above are explicit MASK rules.
Malformed ID/weight/color fixtures exercise oracle diagnostics only.

Oracle entry: `fit_color_groups_table` in [reference.py](../../../../oracle/ops/mask_morphology/mask04/reference.py).
