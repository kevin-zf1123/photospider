---
spec_schema_version: 1
id: NUM-03B
parent_id: NUM-03
function: broadcast
operation_family: numeric.broadcast
proposed_operation_keys:
  - numeric.broadcast_strict
  - numeric.broadcast_accelerated_apple_silicon
  - numeric.broadcast_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-03B: broadcast

Broadcast expands a single-tensor Result through an explicit injective mapping from every complete source axis to a distinct target axis. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

The complete source `sample_shape()` includes batch axes and has rank 1..8. Required static String `shape` lists positive target extents with rank 1..8 and product at most 2^40. Required static String `axis_map` lists one canonical nonnegative target-axis index per source axis. Output rank is not smaller than source rank. Each source extent is one or equals its mapped target extent. Duplicate or out-of-range mapped axes, malformed shapes/maps and incompatible mapped extents are rejected statically. The SDK helper takes the CPU profile after `axis_map`.

All seven numeric dtypes are supported with unchanged dtype. Output `values` uses `photospider.tensor`/`samples` with ordinary target axes.

## Mathematics

For source shape I, target shape O, map a and global output coordinate o:

```text
s[j] = 0        when I[j] == 1
s[j] = o[a[j]]  otherwise
values[o] = bitcopy(input[s])
```

Unmapped target axes replicate source values. Permutation and singleton expansion may occur together. No periodic tiling, axis merging, implicit alignment or numeric conversion is provided. Raw bits are preserved, including signaling NaNs and integer extrema.

## Behavior

Each output sample depends on exactly one source sample, `input[s]` above. Empty selection reads nothing. Unsupported profiles fail before invocation.

## Acceptance

Source `[10,20,30]`, target `[2,3,4]`, map `[1]`: output `[i,j,k]` equals source[j]. Source `[2,3]`, target `[3,4,2]`, map `[2,0]`: output `[j,k,i]` equals source `[i,j]`. Cases: seven dtypes, singleton expansion, rank eight, sparse/empty requests, a sparse huge target whose source remains small.
