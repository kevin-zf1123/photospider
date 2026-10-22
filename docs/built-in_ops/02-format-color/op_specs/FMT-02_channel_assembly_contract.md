---
spec_schema_version: 1
id: FMT-02
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
---

# FMT-02: channel assembly and concatenation family

The default C++ `OperationRegistry` registers twelve CPU Result-operation keys for `assemble`, `concatenate`, `assemble_mapped` and `scalar_literal`, each with `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` profiles. These built-in operations use fixed invocation ABI 8. Public authoring helpers are installed in `photospider/ops/format/channel_assembly.hpp` and `channel_editing.hpp`. The FMT specification remains Proposed independently of the registered implementation.

This is the operator-local clarification for the Proposed FMT-02 family. Inherit
[FMT-common](FMT_common_contract.md), the NUM numerical/resource conventions
referenced there, and the accepted
[kernel storage contract](../../../kernel-specs/Tensor-Storage-and-Region-Access.md).
The specification remains Proposed while the current runtime implementation is
described separately. Its independently specified members are
[FMT-02A assembly](FMT-02A_assemble_channels.md),
[FMT-02B concatenation](FMT-02B_concatenate_channels.md) and
[FMT-02C mapped assembly](FMT-02C_assemble_mapped_channels.md).

## Purpose and inherited boundaries

FMT-02 assembles separately processed components or concatenates channel groups.
Its inputs may come from [FMT-01](FMT-01_channel_extraction_contract.md), generic
numeric processing, or explicitly imported image planes. One requirement is a
split/reassemble identity with explicit channel correspondence. FMT-02C selects
source components into explicitly mapped result slots; single-tensor swizzle,
replacement and generated constant channels retain their FMT-03 home. Association and
alpha editing belong to FMT-04/05. Merely joining alpha with color planes does
not request premultiplication or color conversion.

The family uses tensors with explicit per-invocation interpretation (FMT-common
C13). Spatial topology uses explicit H/W/channel axes and the common tile geometry of its Plan. Physical output storage uses the fixed Plan's dense tile contract. An
optional per-input description supplies channel axes and named selectors; it
cannot change physical addresses. Ordinary numerical policy is inherited rather
than reopened in this interview.

## Confirmed family behavior

FMT-02A stacks single-component inputs by introducing a channel axis. FMT-02B
concatenates existing channel axes. Input-list order defines the order of the
assembled components or concatenated channel blocks; B preserves the order
within each block. Neither member silently reorders its inputs.

FMT-02C supplies an explicit source-to-destination channel mapping. A source
RGBA channel 0 may supply destination channel 2, which a later consumer's
description may call Lab b*. Sample bytes are copied, not transformed from RGB to
Lab or rescaled. A/B retain their confirmed ordered behavior.

Every destination slot must have exactly one source; all slots 0..M-1 must be
covered, where M is the nonzero mapping-entry count. Source channels may be
omitted or reused. Duplicate destinations, holes and out-of-range destinations
fail static validation. There is no implicit fill, blend or accumulation;
constants are explicit preceding producers connected as sources.

Corresponding nonchannel extents must match exactly. An extent of one does not
request broadcasting. Spatial resampling or explicit broadcast, when needed,
precedes assembly. Axis correspondence follows the coordinate rules below.

The output carries no description. A later consumer that treats the assembled
channels as a color group receives that group in its own explicit description;
three input channels do not imply RGB. A Lab color group and an independent alpha
plane may coexist in one output tensor when the consumer's description says so.
Assembly does not consume pixel-domain color or alpha constraints and grants no
sample-validity certificate. The caller satisfies each later semantic consumer's numerical premises;
that consumer does not validate the samples it uses.

## Current Result interface and bounds

The twelve registered keys use single-tensor Result inputs and outputs. Every connected input contributes Descriptor support, including those unused by the selected mapping. Payload dependencies contain the source-grid closure of all mappings in complete selected output tiles. Each input descriptor contains one tensor.

The implementation supports seven native dtypes by exact bit copy, preserves the first input's Result schema id, tensor key and publication policy, sets `atomic_trailing_axes` to zero and attaches no description. Batch axes form a cell-independent prefix and must match across nonscalar sources; a scalar literal is an explicit unbatched shape-[1] source. Full input and output sample counts are bounded by 2^40, and the combined batch/cell rank is at most eight. `axis`, `input_axes` and `output_axis` address cell axes, excluding the Result batch prefix. Connected ports and static maps follow the implementation's admission limits.

### Fixed tile execution

Each selected output tile is one fixed CPU Host task. Lowering intersects its channel interval with the statically resolved destination spans, maps each intersection to the corresponding source coordinates, and unions source-grid tiles separately for each input port. Repeated sources deduplicate input tiles. Disjoint source spans remain separate before tile closure; no bounding rectangle bridges unused gaps. A source tile can cover extra channels or cells because of its batch, spatial or atomic geometry.

Every complete retained output tile must be initialized, including all channels that its geometry covers. A channel-local request avoids other sources only when its normalized output tiles do not also contain destinations backed by those sources. Unused ports contribute schema-only references, so an otherwise unused producer performs no sample work. Static metadata validation still covers all connected inputs. DenseHost bindings use explicit Plan COPYs; DirectTiles use their compiled views.

Preparation fixes destination spans, coordinate maps and exact read slots, and verifies the output geometry, kernel contract, empty task parameters and zero scratch. Runtime chooses a span by destination channel, reads only the mapped source coordinate and copies the original bytes to a dense, independently owned output tile. Legal input views support signed/zero strides and unaligned offsets. Runtime state consists of fixed-size coordinate arrays and one cached read view; it allocates no payload, mapping table or workspace. Cancellation is polled between at most 256 copied samples.

Auto and materialize both copy. Every nonempty view task returns InvalidArgument/ViewUnavailable because fixed produced tiles own distinct storage. A view task has only schema dependencies and requests no source payload. Empty still validates static inputs and parameters but emits no task. Operational admission, cancellation and mandatory upstream effects retain the executor's error ordering.

The zero-input scalar_literal primitive validates explicit dtype and lowercase native-order hex bytes during static preparation. A nonempty selection produces one shape-[1] tile with zero scratch; it copies the prepared bytes without conversion or NaN quieting. Empty emits no task. FMT-03 can reuse one literal call for repeated identical typed values.

Successful tile completion enables internal consumers. Public call Results become available after all required tasks succeed and the call retires in order; a failed call exports no partial output. Literal-provider and assembly calls have separate retirement boundaries. Retained results survive source and context teardown. The fixed path has no dynamic view publication, incremental dirty propagation or sample cache.

See the [architecture reference](../../../kernel-architecture/Channel-and-Color-Operations.md#structural-channel-operations).

## Clarification decisions

| Topic | Selected behavior | State |
| --- | --- | --- |
| Family members | A inserts a channel axis; B concatenates existing channel axes; C assembles explicitly selected sources into destination slots. A/B retain input-list order. | Confirmed |
| Nonchannel dimensions | Corresponding extents must match; broadcast and resampling are explicit preceding operations. | Confirmed |
| Output interpretation | The output carries no description; the next consumer supplies its own. No sample-domain color validation. | Confirmed; C13 replaces description remapping |
| Axes and shape | A: explicit inserted axis, no automatic squeeze. B: separately resolved input axes and explicit output axis; corresponding remaining axes match in order. | Confirmed |
| Arity and static structure | Allow one input, reject zero inputs; input list, axes and channel counts are static. | Confirmed |
| Interpretation conflicts | Inputs carry no descriptions, so there is nothing to conflict. Components from Gray/Black-White or differently encoded sources are combined by index; the consumer's description defines the result. | Confirmed; C13 |
| Regions | Complete selected output tiles determine mapped source-grid closure; unused ports are schema-only. | Confirmed |
| Coordinate interpretation | Nonchannel axes correspond by index. No coordinate-label comparison or spatial alignment is performed. | Confirmed; C13 |
| Output storage | Auto/materialize copy to independent dense tiles; nonempty view fails, Empty has no task. | Confirmed |
| Explicit destination mapping | Support explicit source component to output slot mapping. | Confirmed |
| Mapping member placement | C provides mapped assembly; A/B remain ordered forms. | Confirmed |
| Mapping completeness and reuse | Every output slot has one source, with no holes or duplicate writes; source omissions/reuse are allowed. | Confirmed |
| C input structure | Permit explicit component and channel-tensor sources with equal nonchannel shapes. | Confirmed |
| C source selectors | Static index/name/role, exact unique resolution; name/role require that input's `input_descriptions` entry, index needs none. | Confirmed |

## Confirmed axes and coordinate rules

A receives equal-shape single-component tensors; an input whose
`input_descriptions` entry declares a channel axis is rejected. Its required static axis a inserts a new dimension at any position
0..r in a rank-r input. It does not silently remove a singleton channel axis.
An input such as [H,W,1] with a declared channel axis must first undergo explicit
axis removal, or use B. The cell-rank bounds imply that A accepts cell ranks 1..7 and produces cell
ranks 2..8; batch axes also count toward the full sample-rank limit of 8.
Scalar/rank-zero cell input is not added.

For n inputs of shape S, A's output shape is insert(S,a,n). If j is an output
coordinate and erase(j,a) removes its channel coordinate, the exact copy rule is:

```
Y[j] = X[j[a]][erase(j,a)]
```

B resolves each input channel axis ai separately from its `input_axes` entry or
from the `channel_axis` of its `input_descriptions` entry; when both are present
they must agree, and when neither is present the node fails. It never guesses an
axis from shape. Removing ai from every input shape must yield the same ordered
shape S; there is no implicit permutation of the remaining dimensions. Required
static output_axis a inserts the combined channel extent into S. Inputs and
output have the same cell rank, within the cell-rank bound 1..8; batch plus cell
rank is at most 8.

Let ci be the channel count of input i, p0=0 and p(i+1)=pi+ci using checked
arithmetic. The output shape is insert(S,a,pn). For output channel k=j[a], find
the unique input i for which pi<=k<p(i+1). Then:

```
q = insert(erase(j,a), ai, k-pi)
Y[j] = Xi[q]
```

The equations describe element-byte copies, with no rescaling, dtype promotion,
rounding or alpha/color arithmetic. All inputs must share a dtype; dtype changes
are explicit preceding operations. Implemented dtypes are UInt8, Int8, UInt16,
Int16, Int64, Float32 and Float64. Float signed zeros,
Inf and NaN payloads are preserved exactly when successfully supplied.

For A, [H,W] planes may produce [C,H,W] or [H,W,C]. For B, [3,H,W] and
[H,W,1] may produce [H,W,4]. Physical input addressing is separate from this
logical axis choice, and no local tile geometry is introduced.

All three members permit a single input and reject an empty input list. A single
input to A still inserts an axis of extent one. A single input to B preserves
channel order and may change the logical position of that axis; it is a full
shape identity only when the output axis agrees. Input arity, extents, axes and
channel order are fixed at compile time, not runtime scalar controls. Host graph,
port and resource limits still apply to the requested static invocation.

## Interpretation handling

`input_descriptions` maps input ordinals to descriptions for this invocation.
An entry is validated against that input's descriptor; it supplies the input's
channel axis (A rejects one) and, for C, the channel or component table used by
name/role selectors. Duplicate channel names or roles in a description are
allowed; a selector that matches more than one entry fails.

Assembly attaches no description to its output. A Gray or Black/White plane may
supply what a later consumer treats as an RGB, alpha, Lab or other component.
Three Gray planes assembled by A become an RGB image only for a consumer whose
explicit description declares an RGB group on those indices; this is
interpretation, not a numerical color transform. Combining R/G/B samples from
sources that the caller considers differently encoded is the caller's
responsibility; the kernel and this family do not compare or reject source
interpretations.

Every successful result copies original element bits. In particular, UInt8 255
does not become Float32 1.0 and binary values are not automatically expanded to
another range. Cast, interval decoding, transfer or actual gray/XYZ/Lab
conversion are explicit preceding or subsequent operators as appropriate.
Packed one-bit file data belongs to the codec boundary, not a new tensor dtype.

Nonchannel axes correspond by index only. There is no comparison of coordinate
roles, units, origins or sampling grids and no automatic spatial alignment.
Spatial inputs must agree on their physical height/width axes; a description
cannot relabel physical image axes.

## Interface and support matrix

A/B/C target the default registry as separate primitives, with 1..1024 ordered
input Results in `inputs[0..n)`, each carrying one tensor, and one tensor output
`values`. They do not consume a tensor collection as an opaque input; the
authoring layer expands selected collection entries into this static ordered
input list. Port and graph capacity still apply. No
FMT-specific legacy two-to-four-input bound is inherited. All logical counts and
channel prefix sums use checked arithmetic and the inherited 2^40-element bound.
C specializes the static mapping/selection interface in its member specification.

All parameters below are static. The types specify logical authoring values;
ordered optional axes and indexed descriptions use the canonical `v1` String
records written by the installed helpers (`v1;<ordinal>:<description hex>;...`
for `input_descriptions`, `v1;<axis or _>;...` for `input_axes`).

| Parameter | Type / range | Default and conditions |
| --- | --- | --- |
| `input_descriptions` | Optional sorted, unique input-ordinal-to-`TensorDescription` records | Absent means no input is described. Each entry is validated against its input and used only for axis resolution and C name/role selectors in this invocation. |
| `layout` | String: auto, view, materialize | auto; direct nodes provide it explicitly. |
| A: `axis` | Int64 cell-axis index in [0,input_cell_rank] | Required; insertion position, no negative shorthand; excludes batch prefix. |
| B: `input_axes` | Optional ordered list of n optional Int64 cell-axis indices, each in [0,input_cell_rank) | An omitted list or `_` entry requires that input's description to supply `channel_axis`. A supplied entry must agree with a described axis. |
| B: `output_axis` | Int64 cell-axis index in [0,input_cell_rank) | Required; no implicit channel-last default. |

For A, a described channel axis is an error, including extent one. A
description does not convert a physical image backing into another layout. Image inputs and outputs must satisfy the kernel storage contract at
their actual address maps; new interpretations may require explicit conversion
or materialization. Output image axes/groups must be structurally established,
not inferred from rank or channel count alone. Generic tensor output remains
possible without a complete image/color description.

| Dimension | Strict / CPU accelerated target | Not promised |
| --- | --- | --- |
| Dtype | UInt8, UInt16, Int8, Int16, Int64, Float32, Float64, same across all inputs/output | Missing native widths require implementation; no implicit cast or Float16 extension. |
| Cell rank | A: 1..7 to 2..8; B: 1..8 to the same cell rank; C: output cell rank r+1 in 1..8 | Full sample rank includes batch and cell axes and is at most 8. Cell rank zero/empty dimensions and dynamic arity are excluded; C with r=0 has channel-vector inputs only. |
| Values | Exact element bytes, including HDR/signed and IEEE special patterns | No ULP relaxation for copying; no color/alpha-domain scan. |
| Input layout | Plan-validated affine views for generic or spatial topology; DenseHost uses explicit imports | Topology and descriptions cannot override physical view validation. |
| Output | Independent dense selected tiles; auto/materialize copy | No output alias or implicit packed ROI export. |
| Backend | Portable CPU and named Apple Silicon/x86-64 CPU profiles with identical copy semantics | GPU is unpromised; unavailable named backends fail explicitly. |
| Execution | Exact mapped components/regions with no halo | Existing complete-input implementation is not a conforming replacement. |

## Coordinates, storage and resource bounds

For A, intersect each complete selected output tile with channel i and remove the output channel axis. For B, intersect with prefix interval [pi,p(i+1)), subtract pi and insert the coordinate at source axis ai. C uses its destination-indexed source selection. Explicit internal scalar records map to source coordinate [0]. These are logical sample mappings; physical reads are their unions closed through each source grid.

The output uses the first source's schema id, member key, publication policy and batch prefix, with the inferred channel dimension, atomic_trailing_axes=0 and projected spatial topology. Topology describes axes independently of the Plan's concrete strides. A description cannot relabel spatial H/W axes or change addresses. Input count, shape, axes, descriptions and mapping changes require new compilation; bindings can change compatible immutable payloads between Runs.

Selected output storage is proportional to complete selected tiles. Input view and preparation metadata scale with source tile closure, participating ports and static mapping spans; neither runtime storage nor mapping expansion allocates one entry per unrequested logical sample. A/B have at most one span per input; C has one span per mapping row. Per sample the implementation uses a binary span lookup, O(rank) coordinate projection and a cached view or binary tile-slot lookup. There is no all-input search per sample, full-image gather or private worker pool.

The executor admits output allocations and task records before issue and retains source owners through their last read. Runs share immutable preparation and own their output allocations independently. Copy work is exact byte movement, including sNaNs, signed zeros and integer extrema. Metadata/slot validation happens before unchecked numerical execution; cancellation remains operational.

## Error mapping

| Failure | Phase | Status / scope |
| --- | --- | --- |
| Zero inputs, malformed or unsorted `input_descriptions`, missing or out-of-range axis, axis that conflicts with a description, illegal rank insertion, absent or ambiguous C selector | Compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Dtype mismatch, incompatible nonchannel extents, unsupported tensor rank/type or channel-table length | Compile/direct preflight | TypeMismatch / None. |
| Checked shape/prefix overflow, host port/graph capacity or resource/work admission failure | Inherited validation/admission phase | Preserve the shared NUM/kernel status rather than truncate or wrap. |
| Nonempty forced view | Fixed task execution | InvalidArgument / ViewUnavailable; schema-only source dependencies. |
| Missing source coverage, required upstream failure, cancellation, stale plan, unsupported backend or allocation failure | Respective inherited phase | Preserve inherited code/reason/origin and observation scope. |

Diagnostics identify the input ordinal, relevant axis/group/field and operation.
Do not invent a pixel coordinate for static descriptor errors. NaN/Inf or an
out-of-range alpha alone is not an assembly failure; this operator does not
consume those sample invariants.

## Acceptance

Use an independent coordinate interpreter and byte-address evaluator for all seven dtypes. Compare strict and available accelerated profiles bit-for-bit. Cover every legal source/output axis, rank-one channel vectors, component insertion, batch prefixes, spatial/atomic grids, prefix boundaries, mixed C structures, omitted and repeated sources, static names/roles and malformed maps.

Exercise sparse source unions, large sparse logical domains, output tile closure, signed/zero-stride inputs, DenseHost COPYs, Empty and nonempty view failures. Verify actual public split/reassemble, swizzle/replace and alpha-removal helpers, including scalar/literal expansion and compatible rebinding. Check prepared-signature rejection, cancellation, source failure, concurrent Runs and retained ownership. A no-base-read fixture must select output tiles whose complete maps use only external/scalar sources.

Temporary builtin checks are removed after validation; independent mathematical/byte oracles remain. Report actual CPU profiles and execution evidence separately from source inspection. Performance measurements require the same schema, requests, bindings, worker count and numerical workload; no throughput guarantee follows from registration.
