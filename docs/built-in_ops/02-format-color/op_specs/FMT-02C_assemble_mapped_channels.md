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

The default C++ `OperationRegistry` registers this member's `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` CPU variants as Result operations. These built-ins execute through fixed invocation ABI 8. Public authoring helpers are installed in `photospider/ops/format/channel_assembly.hpp` and `channel_editing.hpp`. The FMT specification remains Proposed independently of the registered implementation.

Inherit the [FMT-02 family contract](FMT-02_channel_assembly_contract.md) for
exact copying, same dtype, spatial agreement, explicit input descriptions, fixed tile storage,
auto/view/materialize, CPU support, resource accounting and failures. This
member independently selects sources and destination slots, without assigning output interpretation. Its default-registry keys are independent of the retired `channel.merge`.

## Inputs and static interface

Ordered inputs `inputs[0..n)` are single-tensor Results and produce one tensor
`values`. The family allows 1..1024 inputs; sources may be selected repeatedly
or not at all. Every connected input
still participates in static structural validation, even when no mapping row
selects it. All inputs have one common dtype; no implicit cast is performed.

Each input explicitly declares one of two source structures:

- `component`: no effective channel axis. Its entire shape is the nonchannel
  shape S; its only selectable component has conceptual index 0.
- `channels`: resolve a channel axis from an explicit input description or axis parameter, as for B. Removing that axis from its shape yields S. Without a description, its axis is required.

Every S must match in rank, dimension order and extent;  Do not infer source structure from rank,
axis extent or a missing label, and do not squeeze, broadcast, transpose spatial
axes or resample. An explicit output cell-axis a, excluding the Result batch prefix, is inserted
into S. If S has cell rank r, output cell rank is r+1. Component inputs have
cell rank r in 1..7; channel inputs have cell rank r+1 in 1..8. In every case,
batch plus cell rank is at most 8. For r=0, only channel-vector inputs are legal;
this adds no rank-zero tensor support.

All parameters are static logical authoring values serialized through the
current String parameter ABI. Public helpers validate and encode these records
without reading sample payloads.

| Parameter | Meaning / constraints |
| --- | --- |
| `input_descriptions`, `layout` | Optional per-input explicit descriptions and the family layout policy. |
| `input_structure` | Required ordered list of n component/channels declarations, each with its optional channel-axis assertion. No axis on component entries. |
| `output_axis` | Required Int64 in [0,r]; no negative shorthand or default. |
| `mapping` | Required nonempty list of M static source/destination records as below. M obeys checked output extent/count and metadata/resource limits. |

Each mapping record contains:

| Field | Meaning / constraints |
| --- | --- |
| `input` | Int64 input ordinal in [0,n). |
| `match` | `index`, `name` or `role`. |
| `selector` | Canonical unsigned decimal index or exact name/role text. |
| `destination` | Int64 in [0,M); every destination occurs exactly once. |

Names and roles use separate namespaces, case-sensitive exact matches and no
alias/fuzzy lookup. Resolve them to unique indices at compile/direct preflight;
missing or ambiguous matches fail as in FMT-01B. A component input permits index
0, or a uniquely matching name/role in its single-component description. Raw
permits index selectors only; it cannot silently use ignored semantic labels.
Explicit selector descriptions and mapping parameters enter compiled identity; input structure contributes Descriptor dependency.

Reject destination holes, duplicates, negative or out-of-range slots. M is the
mapping-entry count, not max(destination)+1 with implicit filling. Zero mappings
are illegal. Sort validated records by destination for canonical evaluation;
declaration order introduces no write ordering or last-writer behavior. There
is no blending, accumulation or implicit constant. An explicit constant source
must already have the required S and dtype; scalar broadcasting is not added.

## Coordinate mapping and explicit interpretation

Let the validated record for destination k resolve to source input i and
component s. For output coordinate j, let k=j[a] and u=erase(j,a). Then:

```
component source: Y[j] = Xi[u]                    (s=0)
channel source:   Y[j] = Xi[insert(u, source_axis_i, s)]
```

Both equations copy exactly one element's bytes. A source may supply several
output destinations. Values, signed zero and NaN payloads remain unchanged.

Mapping records contain only input, match, selector and destination. There is no
`destination_component` or `output_description` parameter. Callers construct the
configuration for each downstream consumer explicitly; the output has no domain
description. This operation performs no color conversion, range scaling,
premultiplication, zero-alpha cleanup or sample-domain validation.

## Fixed dependencies, resources and errors

For every complete selected output tile, intersect its channel interval with each destination span and map the participating regions to their source coordinates. Deduplicate source tiles per port, preserving disjoint mapped spans before source-grid closure. Omitted ports have only schema dependencies. Atomic/spatial output geometry can include extra destinations; source grids can include extra samples in authorized tiles. The kernel reads only effective mapped coordinates.

Preparation resolves selectors once and checks exact read slots, output geometry and zero scratch. Runtime performs a binary destination-span lookup and fixed coordinate projection, then copies bytes into an independent dense output tile. Auto/materialize copy; every nonempty view task fails InvalidArgument/ViewUnavailable without source payload reads. Empty validates static metadata and emits no task. Cancellation polling, call retirement and result ownership follow the family.

Malformed mappings, incomplete destinations, invalid or ambiguous selectors and invalid descriptions fail static preparation. Shape/dtype mismatches and operational failures retain their documented categories. No sample values are numerically validated; incremental dirty mapping and dynamic view publication are outside this protocol.

## Independent fixtures and acceptance

Use straight RGB+alpha source X0, shape [1,2,4], channel axis 2, logical samples
`[[[0.25,0.5,0.75,0.8],[0.125,0.25,0.5,1.0]]]`. X1 is a component source of
shape [1,2] with samples `[[0.2,0.6]]`, same Float32 dtype and nonchannel grid.
Choose output_axis=2. The next consumer may be configured with a Lab D50 group on slots 0..2 and independent alpha on slot 3. Supply these data mapping records:

| Destination | Source | Downstream interpretation supplied by caller |
| --- | --- | --- |
| 0 | X0 index 1 (G) | Lab group, normalized l=L*/100 coordinate |
| 1 | X0 index 2 (B) | Lab group, a* coordinate |
| 2 | X0 index 0 (R) | Lab group, b* coordinate |
| 3 | X1 index 0 | Independent alpha |

Expected shape is [1,2,4]. Expected logical samples are
`[[[0.5,0.75,0.25,0.2],[0.25,0.5,0.125,0.6]]]`, with each element copied from
the represented Float32 input bits. X0 alpha is not a mapped source sample, though its containing source tile may be retained. Output (0,1,2) copies X0(0,1,0)=0.125; omitting X1 payload requires output tiles with no X1-backed destinations. RGB-to-Lab reference
values are deliberately irrelevant to this byte-copy oracle.

A separate reuse fixture selects X0 R into destinations 0 and 2 and X0 alpha
into destination 1, without declaring a complete color group. Its first pixel
is [0.25,0.8,0.25]. R contributes to destinations 0 and 2, while G/B contribute to neither. Runtime selection operates on complete output tiles. Reject a duplicate-destination or missing-slot
version of this mapping before requesting any input payload.

The independent oracle scatters each selected source coordinate into all of its
destinations and compares exact bytes to a separate gather evaluation. Also
cover identity mapping, input/mapping declaration ordering, selector equivalence
and ambiguity, explicit selector descriptions and invalid description/shape combinations, every legal source/output axis, mixed component/channel sources,
rank-one channel vectors, and source reuse across disjoint ROI/tile boundaries.
Inherit all family memory, cancellation, failure and special-value cases.

The conceptual public workflow is `RGBA + independent plane -> mapped assembly
-> named result`. Implementation must supply a real public compile/execute
example with a partial channel request and checked bytes. Benchmark the family
[4096,4096] workload with identity, reorder and repeated-source maps, full and
one-channel requests, recording mapping size and descriptor-resolution time
separately.
