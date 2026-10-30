---
spec_schema_version: 1
id: NUM-10-scatter
parent_id: NUM-10
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-10: scatter into a base tensor

## Interface

The four operations `scatter_replace`, `scatter_sum`, `scatter_minimum` and `scatter_maximum` have strict and two CPU accelerated profiles. Inputs are base, Int64 indices `[M]`, and updates. Every Result contains one tensor under any schema/member key; complete sample shapes include batch axes and have rank 1..8, positive extents and at most 2^40 elements. Base and updates share UInt8, Int64, Float32 or Float64 dtype. The required static Int64 `axis` is in range. Updates match base except that the axis length is M. Output `values` uses `photospider.tensor`/`samples` with ordinary base axes and unchanged dtype. Base is not modified.

Static validation rejects incompatible types, ranks, shapes, axis and logical counts. This contract overrides the [NUM baseline](NUM_common_contract.md).

## Indices and contributors

Every index lies in the base axis extent, including indices unrelated to the selected output, as an unchecked caller premise. A coordinate is a hit when at least one index equals its position along `axis`; its contributors are the updates with that target index, ordered by increasing original position in the index vector.

A no-hit coordinate copies base bits exactly, including signaling NaNs. Replace selects the contributor with the greatest original position and copies that update without arithmetic. Aggregate variants start with base and then process matching updates in original position order. The first source NaN wins, with preserved sign/payload and a set quiet bit, before generated exceptional results. No-hit and Replace results never quiet NaNs.

## Aggregation

Sum: mixed positive and negative infinities produce the fixed positive quiet NaN; otherwise infinity sets the result sign. Finite contributors sum exactly with one final floating conversion; intermediate exact totals may exceed the destination range. Integer final representability is an unchecked caller precondition. Exact zero is negative only when every contributor is negative zero; underflow preserves the exact sign. Strict conversion is correctly rounded; accelerated profiles follow the [shared floating bound](NUM_accelerated_contract.md).

Minimum/maximum use NUM-05 numerical ordering with exact integer comparisons. Mixed zero signs yield -0 for minimum and +0 for maximum; equal zero signs are unchanged. These aggregate rules apply only to hit coordinates.

## Individual specifications

| Spec | Duplicate target behavior |
| --- | --- |
| [scatter_replace](NUM-10C_scatter_replace.md) | Last original position wins |
| [scatter_sum](NUM-10D_scatter_sum.md) | Exact sum including base |
| [scatter_minimum](NUM-10E_scatter_minimum.md) | Minimum including base, first NaN wins |
| [scatter_maximum](NUM-10F_scatter_maximum.md) | Maximum including base, first NaN wins |
