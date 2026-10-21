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

The four operations `scatter_replace`, `scatter_sum`, `scatter_minimum` and `scatter_maximum` have strict and two CPU accelerated profiles. Inputs are base, Int64 indices `[M]`, and updates. Every Result contains one tensor under any schema/member key; complete sample shapes include batch axes and have rank 1..8, positive extents and at most 2^40 elements. Base/updates share UInt8, Int64, Float32 or Float64 dtype. Required static Int64 `axis` is in range. Updates match base except axis length M. Output `values` uses `photospider.tensor`/`samples` with ordinary base axes and unchanged dtype. Base is immutable.

## Indices, contributors and selected outputs

Every nonempty selection creates one fixed task that validates all indices against the base axis. Invalid indices, including any unrelated to selected output, return `InvalidArgument` with `FailureReason::InvalidDomain`. Negative indices are invalid. The task stably groups `{target_index,original_position}` pairs with eight radix passes and publishes one private dense Int64 `[M,2]` tile. Original positions remain increasing within each target group.

Each selected output tile has a consumer task. It reads base tiles intersecting that output region, update tiles with the same non-axis region and the complete M axis, and the shared private table. Bounded searches find contributor ranges. Runtime checks used normalized update positions below M before addressing updates. Preparation checks complete read sets and exact dense private views; a private port alone is not producer provenance. Empty selection emits no tasks or payload reads.

A no-hit coordinate copies base bits exactly, including signaling NaNs. Replace selects the greatest matching original position and copies that update without arithmetic. Aggregate variants start with base and then process matching updates in original position order. The first source NaN wins, with preserved sign/payload and a set quiet bit, before generated exceptional results.

For Sum, mixed positive and negative infinities produce the fixed positive quiet NaN; otherwise infinity sets the result sign. Finite contributors sum exactly with one final floating conversion. Integer final representability is an unchecked caller precondition; no diagnostic is promised outside that domain. Intermediate exact totals can exceed destination range. Exact zero is negative only when every contributor is negative zero; underflow preserves the exact sign. Strict conversion is correctly rounded; accelerated profiles retain the [shared floating bound](NUM_accelerated_contract.md).

Minimum/maximum use NUM-05 numerical ordering with exact integer comparisons. Mixed zero signs yield -0 for minimum and +0 for maximum; equal zero signs remain unchanged. These aggregate rules apply only to hit coordinates. Unselected outputs are not evaluated; no-hit and Replace paths never quiet NaNs.

## Storage, preparation and lifetime

The normalized table occupies `16*M` bytes. Scatter uses that output as one radix buffer and a second `16*M`-byte scratch buffer, with the final eight-pass result in the table. Consumer aggregate tasks use private preallocated ExactAggregate scratch; Replace needs no consumer scratch. The executor reserves maximum block scratch per CPU lane, so capacity can exceed one task's scratch demand. Output storage consists only of declared independent dense tiles.

Prepared state owns immutable metadata and read-slot maps. Runtime mutable state is stack storage, the private tile or admitted lane scratch; no runtime payload allocation is used. Negative/zero input strides and unaligned elements are supported after physical bounds checks. Index work checks cancellation every 256 entries, output copying every 64 samples, and aggregate arithmetic through cancellation callbacks. Operational failures prevent successful call retirement. Successful output owners outlive context destruction. Per-sample work counters, NumericDiagnostics and completed-result caches are unavailable.

Static validation rejects incompatible types, ranks, shapes, axis and logical counts. Prepare rejects missing/extra reads, incorrect private views, task parameters or scratch. See [fixed indexing execution](../indexing-execution.md) for validation and runtime boundaries. This contract overrides the [NUM baseline](NUM_common_contract.md); specification status remains Proposed.

## Individual specifications

| Spec | Duplicate target behavior |
| --- | --- |
| [scatter_replace](NUM-10C_scatter_replace.md) | Last original position wins |
| [scatter_sum](NUM-10D_scatter_sum.md) | Exact sum including base |
| [scatter_minimum](NUM-10E_scatter_minimum.md) | Minimum including base, first NaN wins |
| [scatter_maximum](NUM-10F_scatter_maximum.md) | Maximum including base, first NaN wins |
