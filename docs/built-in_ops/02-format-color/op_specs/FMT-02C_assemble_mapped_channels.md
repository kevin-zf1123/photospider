---
spec_schema_version: 1
id: FMT-02C
parent_id: FMT-02
function: assemble_mapped_channels
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
implemented_operation_keys:
  - channel.assemble_mapped_strict
  - channel.assemble_mapped_accelerated_apple_silicon
  - channel.assemble_mapped_accelerated_x86_64
---

# FMT-02C: assemble explicitly mapped components

Inherits the [FMT-02 family contract](FMT-02_channel_assembly_contract.md) for exact copying, same dtype, spatial agreement, explicit input descriptions, auto/view/materialize and failures. This member selects sources and destination slots, without assigning output interpretation.

## Interface

Ordered inputs `inputs[0..n)` (1..1024) are single-tensor Results and produce one tensor `values`. Sources may be selected repeatedly or not at all. Every connected input participates in static structural validation, even when no mapping row selects it. All inputs share one dtype; no implicit cast is performed.

Each input explicitly declares one of two source structures:

- `component`: no channel axis. Its entire shape is the nonchannel shape S; its only selectable component has index 0.
- `channels`: a channel axis is resolved from an explicit input description or axis parameter, as for B. Removing that axis from its shape yields S. Without a description, its axis is required.

Every S matches in rank, dimension order and extent. Source structure is never inferred from rank, axis extent or a missing label, and nothing is squeezed, broadcast, transposed or resampled. An explicit output cell-axis a, excluding the Result batch prefix, is inserted into S. If S has cell rank r, output cell rank is r+1. Component inputs have cell rank r in 1..7; channel inputs have cell rank r+1 in 1..8. Batch plus cell rank is at most 8. For r=0, only channel-vector inputs are legal.

All parameters are static.

| Parameter | Meaning / constraints |
| --- | --- |
| `input_descriptions`, `layout` | Optional per-input explicit descriptions and the family layout policy. |
| `input_structure` | Required ordered list of n component/channels declarations, each with its optional channel-axis assertion. No axis on component entries. |
| `output_axis` | Required Int64 in [0,r]; no negative shorthand or default. |
| `mapping` | Required nonempty list of M static source/destination records as below. |

Each mapping record contains:

| Field | Meaning / constraints |
| --- | --- |
| `input` | Int64 input ordinal in [0,n). |
| `match` | `index`, `name` or `role`. |
| `selector` | Canonical unsigned decimal index or exact name/role text. |
| `destination` | Int64 in [0,M); every destination occurs exactly once. |

Names and roles use separate namespaces, case-sensitive exact matches and no alias/fuzzy lookup. They resolve to unique indices at preparation; missing or ambiguous matches fail as in FMT-01B. A component input permits index 0, or a uniquely matching name/role in its single-component description. Without descriptions only index selectors are permitted.

Destination holes, duplicates, negative or out-of-range slots are rejected. M is the mapping-entry count, not max(destination)+1 with implicit filling. Zero mappings are illegal. Declaration order introduces no write ordering or last-writer behavior. There is no blending, accumulation or implicit constant. An explicit constant source must already have the required S and dtype; scalar broadcasting is not added.

## Mathematics

Let the validated record for destination k resolve to source input i and component s. For output coordinate j, let k=j[a] and u=erase(j,a):

```
component source: Y[j] = Xi[u]                    (s=0)
channel source:   Y[j] = Xi[insert(u, source_axis_i, s)]
```

Both equations copy exactly one element's bytes. A source may supply several output destinations. Values, signed zero and NaN payloads remain unchanged.

Mapping records contain only input, match, selector and destination. The output has no description. The operation performs no color conversion, range scaling, premultiplication, zero-alpha cleanup or sample-domain validation.

## Behavior

Output sample j depends on one source sample. Omitted ports have only schema dependencies. Auto/materialize copy; a nonempty view fails with `InvalidArgument` / `ViewUnavailable`. Malformed mappings, incomplete destinations, invalid or ambiguous selectors and invalid descriptions fail static preparation; shape/dtype mismatches retain their documented categories.

## Acceptance

Straight RGB+alpha source X0, shape [1,2,4], channel axis 2, logical samples `[[[0.25,0.5,0.75,0.8],[0.125,0.25,0.5,1.0]]]`. X1 is a component source of shape [1,2] with samples `[[0.2,0.6]]`, same Float32 dtype and nonchannel grid. Choose output_axis=2. The next consumer may be configured with a Lab D50 group on slots 0..2 and independent alpha on slot 3:

| Destination | Source | Downstream interpretation supplied by caller |
| --- | --- | --- |
| 0 | X0 index 1 (G) | Lab group, normalized l=L*/100 coordinate |
| 1 | X0 index 2 (B) | Lab group, a* coordinate |
| 2 | X0 index 0 (R) | Lab group, b* coordinate |
| 3 | X1 index 0 | Independent alpha |

Expected shape is [1,2,4] and samples are `[[[0.5,0.75,0.25,0.2],[0.25,0.5,0.125,0.6]]]`, each element copied from the Float32 input bits. X0 alpha is not a mapped source sample. Output (0,1,2) copies X0(0,1,0)=0.125. RGB-to-Lab reference values are irrelevant to this byte-copy case.

Reuse fixture: select X0 R into destinations 0 and 2 and X0 alpha into destination 1, without declaring a complete color group. The first pixel is [0.25,0.8,0.25]. A duplicate-destination or missing-slot version of this mapping is rejected statically.

Also cover identity mapping, selector equivalence and ambiguity, every legal source/output axis, mixed component/channel sources, rank-one channel vectors, and source reuse across disjoint ROI/tile boundaries.

## Conceptual DAG

`RGBA + independent plane -> mapped assembly -> named result`
