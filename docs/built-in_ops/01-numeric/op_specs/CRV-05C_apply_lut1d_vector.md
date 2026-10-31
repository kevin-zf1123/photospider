---
spec_schema_version: 1
id: CRV-05C
parent_id: CRV-05
function: apply_lut1d_vector
operation_family: curve.apply_lut1d_vector
proposed_operation_keys:
  - curve.apply_lut1d_vector_fast
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# CRV-05C: Map each scalar query through a vector-valued LUT

Map each input scalar through all columns of a uniform-axis LUT, appending an output component axis. This numeric primitive supplies the direct lookup stage of [GRD-06C](../../07-grade/op_specs/GRD-06C_gradient_map_channels.md). It accepts a precomputed table and preserves its coordinate values. Color interpretation, curve construction and output-model validation belong to the caller.

## Interface

Ordered required inputs are `input: Float32/64[S]`, `table: Float32/64[L,C]` and `axis: Float64[3]`. Input and table dtypes are independent. Each Result contains exactly one tensor under any structurally valid schema id/version/key. Complete `sample_shape()` supplies S, with rank 1..7, positive extents and no distinguished input component axis. Require 1<=L<=1048576, C>=1 and every logical input/output element product <=2^40.

Output `values` uses `photospider.tensor` v1/member `samples`, shape `S+[C]`, selected Float32/Float64 dtype and ordinary axes. Batch coordinates become ordinary output sample axes, as in CRV-05A/B. C=1 remains an explicit final axis. A rank-1 input `[N]` therefore produces `[N,C]`, and a query image `[H,W]` produces `[H,W,C]`.

Required static String parameters are `dtype` (`float32` or `float64`) and `out_of_domain` (`reject`, `clamp` or `linear_extrapolate`). The proposed authoring constructor `apply_lut1d_vector_node` defaults dtype to the query input dtype and policy to reject, and selects the explicit `curve.apply_lut1d_vector_fast` key. No strict or accelerated registration is specified for this member. The Result carries no color description; no color model, hue unit, transfer or alpha parameter is consumed.

## Mathematics

The shared-axis, endpoint, singleton and unchecked numerical premises are those of [CRV-05A](CRV-05A_apply_lut1d.md). Arithmetic follows the [CRV-05 native fast profile](CRV-05_apply_lut1d.md#native-fast-numerical-profile), applied to each table column with the same query:

```text
values[s,c] = fast_lut_lookup(input[s], table[:,c], axis)
```

For L>1, compute the binary64 normalized position `u=(q-start)*(L-1)/(end-start)`. Clamp selects the nearest numerical endpoint; reject relies on the caller's in-domain premise. Otherwise choose `i=floor(u)` bounded to `[0,L-2]`, set `t=u-i`, and evaluate `fma(t, table[i+1,c]-table[i,c], table[i,c])` in binary64 before the final dtype conversion. Exact normalized endpoint positions directly select the endpoint row. Negative axis direction follows the same formula. Linear extrapolation uses the corresponding endpoint segment. Interior positioning follows native normalized arithmetic, not the strict profile's exact reconstruction of each rounded grid coordinate.

The query, normalized position and selected row pair are shared across output components. For L=1, directly convert `table[0,c]`; query and axis payloads are inactive, while their static schemas and the inherited singleton/reject premises still apply. Direct selection preserves signed zero. Ordinary arithmetic retains the native fast profile's floating behavior. There is no nominal component clipping or periodic reduction: finite negative/HDR coordinates and multi-turn hue-like numbers are ordinary scalar table entries.

The equivalent existing-node expression copies the query into C channels and invokes `curve.apply_lut1d_channels_fast`; the direct primitive permits query sharing without that replicated input tensor. This equivalence does not require a color-ramp conversion or a change to CRV-05A/B.

## Behavior

For L>1, output `[s,c]` depends on input `[s]`, axis endpoints and column c of the table. All L rows in that column may be declared as the lookup support superset, although arithmetic selects one or two rows. Output components remain independent for table dependencies. For L=1 only the selected table columns are payload dependencies. Empty requests require no payloads. ROI and tile evaluation agree at the same sample/component coordinates.

Consumed query/table finiteness, valid axis representation, domain membership and output representability are unchecked premises inherited from CRV-05. Static metadata checks include input rank, table rank, axis shape/dtype, output rank/product and parameter domains. The implementation uses the existing operator and fixed invocation interfaces.

## Acceptance

Use the native CRV-05 formula and its explicit query-replication composition as references. A strict mathematical comparison evaluates each CRV-05A scalar column with the supplied Float64 axis; this does not create a strict runtime key. For ordinary finite analytic fixtures use abs(error)<=atol+rtol*abs(reference), with `(atol,rtol)=(2e-6,2e-5)` for Float32 and `(2e-12,2e-11)` for Float64. Direct selected-entry conversion and exact copies compare bits. These fixture tolerances do not provide universal conditioning bounds or accelerated-profile certification.

- `input=[0,0.25,1]`, `table=[[0,10,0],[2,8,4]]`, `axis=[0,1,1]` gives `[[0,10,0],[0.5,9.5,1],[2,8,4]]`.
- Reversing table rows and using `axis=[1,0,-1]` represents the same piecewise-linear function; compare within native tolerance.
- With the same ascending table, query -1 under clamp selects `(0,10,0)`; query 2 under linear_extrapolate gives `(4,6,8)`.
- `table=[[7,-0,4]]`, singleton axis `[2,2,+0]` and clamp produce the same converted row at every output sample, retaining the selected negative zero and avoiding input-query payload reads.
- Compare explicit replication plus CRV-05B with the direct vector lookup for mixed query/table/output dtypes, C=1, C=3 and C=4, image-shaped queries, component-only requests, nonzero ROI, tiled output, zero signs and native normalized-coordinate boundaries.

## References

- [CRV-05 family](CRV-05_apply_lut1d.md).
- [Scalar LUT](CRV-05A_apply_lut1d.md) and [independent per-channel LUT](CRV-05B_apply_lut1d_channels.md).
- [GRD-06C model-space image generation](../../07-grade/op_specs/GRD-06C_gradient_map_channels.md).
