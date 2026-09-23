---
spec_schema_version: 1
id: FMT-02C
parent_id: FMT-02
function: assemble_mapped_channels
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - channel.assemble_mapped_strict
  - channel.assemble_mapped_accelerated_apple_silicon
  - channel.assemble_mapped_accelerated_x86_64
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-02C: assemble explicitly mapped components

Inherit the [FMT-02 family contract](FMT-02_channel_assembly_contract.md) for
exact copying, same dtype, spatial agreement, metadata modes, planar storage,
auto/view/materialize, CPU support, resource accounting and failures. This
member independently selects sources and destination slots, including deliberate
output-component reinterpretation. Its proposed default-registry keys are not
implemented operations or aliases of the legacy `channel.merge`.

## Inputs and static interface

Ordered repeated tensor inputs `inputs[0..n)` produce one tensor `values`.
n>=1; sources may be selected repeatedly or not at all. Every connected input
still participates in static structural validation, even when no mapping row
selects it. All inputs have one common dtype; no implicit cast is performed.

Each input explicitly declares one of two source structures:

- `component`: no effective channel axis. Its entire shape is the nonchannel
  shape S; its only selectable component has conceptual index 0.
- `channels`: resolve a channel axis from effective metadata or an explicit
  axis, as for B. Removing that axis from its shape yields S. In raw mode its
  axis must be supplied explicitly.

Every S must match in rank, dimension order and extent; apply the family's
coordinate-metadata checks as well. Do not infer source structure from rank,
axis extent or a missing label, and do not squeeze, broadcast, transpose spatial
axes or resample. An explicit output axis a is inserted into S. If S has rank r,
output rank is r+1<=8. Component inputs have rank r in 1..7; channel inputs have
rank r+1 in 1..8. For r=0, only channel-vector inputs are legal; this adds no
rank-zero tensor support.

All parameters are static logical authoring values. The family requires a
canonical shared encoding before implementation; these tables do not claim
that new structured parameter kinds already exist in the current ABI.

| Parameter | Meaning / constraints |
| --- | --- |
| `metadata_mode`, `input_overrides`, `output_description`, `layout` | Family parameters and defaults. Complete color groups are explicit. |
| `input_structure` | Required ordered list of n component/channels declarations, each with its optional channel-axis assertion. No axis on component entries. |
| `output_axis` | Required Int64 in [0,r]; no negative shorthand or default. |
| `mapping` | Required nonempty list of M static source/destination records as below. M obeys checked output extent/count and metadata/resource limits. |

Each mapping record contains:

| Field | Meaning / constraints |
| --- | --- |
| `input` | Int64 input ordinal in [0,n). |
| `selector` | Exactly one index, name or role selector for that input; index is nonnegative and in range. |
| `destination` | Int64 in [0,M); every destination occurs exactly once. |
| `destination_component` | Optional explicit target component fields using the shared metadata schema; may explicitly reference a group in output_description. |

Names and roles use separate namespaces, case-sensitive exact matches and no
alias/fuzzy lookup. Resolve them to unique indices at compile/direct preflight;
missing or ambiguous matches fail as in FMT-01B. A component input permits index
0, or a uniquely matching name/role in its single-component description. Raw
permits index selectors only; it cannot silently use ignored semantic labels.
Selector metadata belongs to Descriptor dependency and compiled identity.

Reject destination holes, duplicates, negative or out-of-range slots. M is the
mapping-entry count, not max(destination)+1 with implicit filling. Zero mappings
are illegal. Sort validated records by destination for canonical evaluation;
declaration order introduces no write ordering or last-writer behavior. There
is no blending, accumulation or implicit constant. An explicit constant source
must already have the required S and dtype; scalar broadcasting is not added.

## Coordinate semantics and metadata

Let the validated record for destination k resolve to source input i and
component s. For output coordinate j, let k=j[a] and u=erase(j,a). Then:

```
component source: Y[j] = Xi[u]                    (s=0)
channel source:   Y[j] = Xi[insert(u, source_axis_i, s)]
```

Both equations copy exactly one element's bytes. A source may supply several
destinations with independent output descriptions. Values, signed-zero and
NaN payloads do not change because a role, unit or model changes.

The family's standard output_description channel/group assignment applies to C
as well as A/B. `destination_component` is an additional per-record way to
specify those target fields; it is not required when output_description already
defines them. An explicit target definition authorizes reinterpretation of its
specified fields. It does not modify the effective source description,
change name/role selector resolution, affect another mapping row or require
global raw mode. A target group reference explicitly adopts that group's model,
coordinate interpretation and applicable white/profile fields for this entry.
An arbitrary display-name change alone is not a model or unit override.

Apply explicit target fields to the projected source component, then retain
only still-applicable descriptions. Model-dependent source fields that cease
to apply are removed; for example, RGB primaries/transfer do not remain active
on a component explicitly defined as Lab b*. Unredefined applicable fields
still undergo respect-mode consistency checks. The result must be structurally
coherent with the explicit output group; incomplete Lab white/model information
is not guessed. Conflicting definitions of the same destination field between
the record and output_description fail rather than using an undocumented
precedence. Referenced resources retain their owners.

Destination-component reinterpretation does not authorize changing unrelated
nonchannel coordinate grids or physical layout. Those remain governed by the
family's consistency and raw/source-override rules. No reinterpretation grants
sample validity. There is no color conversion, range scaling, premultiplication,
zero-alpha cleanup or validation scan; later consumers validate the semantics
they actually use.

## Exact demand and dirty mapping

For each requested output channel k, map its nonchannel footprint through its
one record. Per-input Data is the exact union of those source footprints. When
several requested destinations share a source component, deduplicate source
support without discarding any output contribution. Unselected components and
unused inputs receive no payload request. Preserve offset/disjoint regions;
there is no halo, Control payload or extra sample Validation domain.

Dirty mapping is a relation, not a one-to-one inverse: for each changed source
sample, enumerate every mapping row that selects that component, insert its
destination slot and intersect the observed output domain. Changes to omitted
source channels dirty nothing. Descriptor changes invalidate affected selector,
mapping, shape or interpretation evidence. Static validation still covers all
connected inputs; necessary upstream computation/failures retain their scope.

No mapped result is assumed to be a view. Reorder, duplicate or independently
owned source planes may require materialization under the canonical output
image layout. `auto` follows the family representability proof/fallback, `view`
fails if unavailable, and `materialize` prepares canonical result pages. Shared
provenance or equal bytes alone is not a legal physical alias proof.

## Reference algorithm, resources and errors

Use the family's validate/map/acquire/prepare/copy/publish algorithm, adding
static source-selector resolution and a destination-indexed table. Build and
validate per-input name/role indices once; matching uses exact strings even if
an implementation accelerates lookup with hashes. A sorted-table reference
costs O(T log T + M log T) comparisons for T total descriptor entries, plus
compared string bytes. Positional selectors need only checked O(M) resolution.
Charge table and string-index capacities; no per-pixel name lookup is allowed.

With n inputs, rank r, M mappings, Nq requested samples and F windows, post-
resolution mapping/copy work is O(n*r+M+Nq*r+F*r), plus exact footprint unions,
metadata propagation and page-set preparation. State is O(n+r+M) plus admitted
lookup/footprint/window capacity. A reverse source-to-destination index supports
dirty fan-out and has O(M) entries. Report Nq*d copied bytes separately from
deduplicated source bytes and retained/provided backing; repeated destinations
are real output samples even when one input read can serve them.

Use host scheduling only for disjoint destination observations. Duplicate
source selection cannot cause a write race because destination slots are unique.
Inherit the 1024-entry/element cancellation polling bound, exact publication,
rollback, source lifetime, optional-cache policy and page/metadata admission.

Malformed mapping records, incomplete destinations, invalid selectors, name/role
ambiguity and conflicting target fields fail compile/direct preflight with
InvalidArgument / InvalidDomain, schema origin. Rank/dtype/shape mismatches use
the family TypeMismatch rule; source/resource/cancellation/backend/view failures
retain their inherited categories. Diagnostics include mapping row, source input
and destination slot. Out-of-domain pixel values do not fail this copy operation.

## Independent fixtures and acceptance

Use straight RGB+alpha source X0, shape [1,2,4], channel axis 2, logical samples
`[[[0.25,0.5,0.75,0.8],[0.125,0.25,0.5,1.0]]]`. X1 is a component source of
shape [1,2] with samples `[[0.2,0.6]]`, same Float32 dtype and nonchannel grid.
Choose output_axis=2 and explicit Lab D50 group on slots 0..2 plus independent
alpha on slot 3. Supply these records, each with its explicit target component:

| Destination | Source | Target interpretation |
| --- | --- | --- |
| 0 | X0 index 1 (G) | Lab group, normalized l=L*/100 coordinate |
| 1 | X0 index 2 (B) | Lab group, a* coordinate |
| 2 | X0 index 0 (R) | Lab group, b* coordinate |
| 3 | X1 index 0 | Independent alpha |

Expected shape is [1,2,4]. Expected logical samples are
`[[[0.5,0.75,0.25,0.2],[0.25,0.5,0.125,0.6]]]`, with each element copied from
the represented Float32 input bits. X0 alpha is never requested. Requesting only
output (0,1,2) reads X0(0,1,0)=0.125, and no X1 payload. RGB-to-Lab reference
values are deliberately irrelevant to this byte-copy oracle.

A separate reuse fixture selects X0 R into destinations 0 and 2 and X0 alpha
into destination 1, without declaring a complete color group. Its first pixel
is [0.25,0.8,0.25]. A change to that R sample dirties destinations 0 and 2;
changes to G/B dirty nothing. Requesting only destination 2 reads R once and
does not publish destination 0. Reject a duplicate-destination or missing-slot
version of this mapping before requesting any input payload.

The independent oracle scatters each selected source coordinate into all of its
destinations and compares exact bytes to a separate gather evaluation. Also
cover identity mapping, input/mapping declaration ordering, selector equivalence
and ambiguity, per-row interpretation isolation, incomplete/conflicting group
descriptions, every legal source/output axis, mixed component/channel sources,
rank-one channel vectors, and source reuse across disjoint ROI/tile boundaries.
Inherit all family memory, cancellation, failure and special-value cases.

The conceptual public workflow is `RGBA + independent plane -> mapped assembly
-> named result`. Implementation must supply a real public compile/execute
example with a partial channel request and checked bytes. Benchmark the family
[4096,4096] workload with identity, reorder and repeated-source maps, full and
one-channel requests, recording mapping size and descriptor-resolution time
separately. No runtime or performance result is claimed by this Proposed spec.
