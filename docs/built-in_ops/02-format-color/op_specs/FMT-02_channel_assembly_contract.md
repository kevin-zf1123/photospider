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

The default registry provides twelve CPU Result-operation keys for `assemble`, `concatenate`, `assemble_mapped` and `scalar_literal`, each with `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` profiles. Public authoring helpers are in `photospider/ops/format/channel_assembly.hpp` and `channel_editing.hpp`.

Inherits [FMT-common](FMT_common_contract.md) and the NUM numerical conventions referenced there. The members are [FMT-02A assembly](FMT-02A_assemble_channels.md), [FMT-02B concatenation](FMT-02B_concatenate_channels.md) and [FMT-02C mapped assembly](FMT-02C_assemble_mapped_channels.md).

## Purpose

FMT-02 assembles separately processed components or concatenates channel groups. Its inputs may come from [FMT-01](FMT-01_channel_extraction_contract.md), generic numeric processing, or explicitly imported image planes. Split/reassemble is an identity with explicit channel correspondence. FMT-02C selects source components into explicitly mapped result slots; single-tensor swizzle, replacement and generated constant channels belong to FMT-03. Association and alpha editing belong to FMT-04/05. Joining alpha with color planes requests no premultiplication or color conversion.

The family uses tensors with explicit per-invocation interpretation (FMT-common C13). Spatial topology uses explicit H/W/channel axes. An optional per-input description supplies channel axes and named selectors; it cannot change physical addresses.

## Family behavior

FMT-02A stacks single-component inputs by introducing a channel axis. FMT-02B concatenates existing channel axes. Input-list order defines the order of the assembled components or concatenated channel blocks; B preserves the order within each block. Neither member reorders its inputs.

FMT-02C supplies an explicit source-to-destination channel mapping. A source RGBA channel 0 may supply destination channel 2, which a later consumer's description may call Lab b*. Sample bytes are copied, not transformed from RGB to Lab or rescaled.

Every destination slot has exactly one source; all slots 0..M-1 are covered, where M is the nonzero mapping-entry count. Source channels may be omitted or reused. Duplicate destinations, holes and out-of-range destinations fail static validation. There is no implicit fill, blend or accumulation; constants are explicit preceding producers connected as sources.

Corresponding nonchannel extents match exactly. An extent of one does not request broadcasting. Spatial resampling or explicit broadcast precedes assembly.

The output carries no description. A later consumer that treats the assembled channels as a color group receives that group in its own explicit description; three input channels do not imply RGB. A Lab color group and an independent alpha plane may coexist in one output tensor when the consumer's description says so. Assembly consumes no pixel-domain color or alpha constraints and certifies no sample validity.

## Interface

The twelve keys use single-tensor Result inputs and outputs. Every connected input contributes descriptor support, including inputs unused by the selected mapping; payload dependencies are the source samples that the mappings reference. Seven native dtypes are supported by exact bit copy. The output preserves the first input's Result schema id and tensor key, sets `atomic_trailing_axes` to zero and attaches no description. Batch axes form a cell-independent prefix and match across nonscalar sources; a scalar literal is an explicit unbatched shape-[1] source. Full input and output sample counts are bounded by 2^40, and the combined batch/cell rank is at most eight. `axis`, `input_axes` and `output_axis` address cell axes, excluding the Result batch prefix.

The zero-input `scalar_literal` primitive takes an explicit dtype and lowercase native-order hex bytes, and produces one shape-[1] tile copying the bytes without conversion or NaN quieting. FMT-03 can reuse one literal call for repeated identical typed values.

A/B/C are separate primitives with 1..1024 ordered input Results in `inputs[0..n)`, each carrying one tensor, and one tensor output `values`. They do not consume a tensor collection as an opaque input; the authoring layer expands selected collection entries into the ordered input list. All logical counts and channel prefix sums use checked arithmetic and the 2^40-element bound.

All parameters are static. Ordered optional axes and indexed descriptions use the canonical `v1` String records written by the installed helpers (`v1;<ordinal>:<description hex>;...` for `input_descriptions`, `v1;<axis or _>;...` for `input_axes`).

| Parameter | Type / range | Default and conditions |
| --- | --- | --- |
| `input_descriptions` | Optional sorted, unique input-ordinal-to-`TensorDescription` records | Absent means no input is described. Each entry is validated against its input and used only for axis resolution and C name/role selectors in this invocation. |
| `layout` | String: auto, view, materialize | auto; direct nodes provide it explicitly. |
| A: `axis` | Int64 cell-axis index in [0,input_cell_rank] | Required; insertion position, no negative shorthand; excludes batch prefix. |
| B: `input_axes` | Optional ordered list of n optional Int64 cell-axis indices, each in [0,input_cell_rank) | An omitted list or `_` entry requires that input's description to supply `channel_axis`. A supplied entry must agree with a described axis. |
| B: `output_axis` | Int64 cell-axis index in [0,input_cell_rank) | Required; no implicit channel-last default. |

`input_descriptions` maps input ordinals to descriptions for this invocation. An entry is validated against that input's descriptor; it supplies the input's channel axis (A rejects one) and, for C, the channel or component table used by name/role selectors. Duplicate channel names or roles in a description are allowed; a selector that matches more than one entry is rejected.

| Dimension | Contract |
| --- | --- |
| Dtype | UInt8, UInt16, Int8, Int16, Int64, Float32, Float64, same across all inputs and output; no implicit cast. |
| Cell rank | A: 1..7 to 2..8; B: 1..8 to the same cell rank; C: output cell rank r+1 in 1..8. Full sample rank includes batch and cell axes and is at most 8. Cell rank zero, empty dimensions and dynamic arity are excluded; C with r=0 has channel-vector inputs only. |
| Values | Exact element bytes, including HDR/signed and IEEE special patterns; no color/alpha-domain scan. |
| Execution | Exact mapped components with no halo. |

All three members permit a single input and reject an empty input list. A single input to A still inserts an axis of extent one. A single input to B preserves channel order and may change the logical position of that axis; it is a full shape identity only when the output axis agrees. Input arity, extents, axes and channel order are fixed at compile time.

## Axes and coordinate rules

A receives equal-shape single-component tensors; an input whose `input_descriptions` entry declares a channel axis is rejected, including extent one. Its required static axis a inserts a new dimension at any position 0..r in a rank-r input. It does not remove a singleton channel axis: an input such as [H,W,1] with a declared channel axis must first undergo explicit axis removal, or use B. A accepts cell ranks 1..7 and produces cell ranks 2..8. Scalar/rank-zero cell input is not added.

For n inputs of shape S, A's output shape is insert(S,a,n). If j is an output coordinate and erase(j,a) removes its channel coordinate:

```
Y[j] = X[j[a]][erase(j,a)]
```

B resolves each input channel axis ai separately from its `input_axes` entry or from the `channel_axis` of its `input_descriptions` entry; when both are present they must agree, and when neither is present the node is rejected. Removing ai from every input shape yields the same ordered shape S; there is no implicit permutation of the remaining dimensions. The static output_axis a inserts the combined channel extent into S. Inputs and output have the same cell rank.

Let ci be the channel count of input i, p0=0 and p(i+1)=pi+ci using checked arithmetic. The output shape is insert(S,a,pn). For output channel k=j[a], the unique input i satisfies pi<=k<p(i+1). Then:

```
q = insert(erase(j,a), ai, k-pi)
Y[j] = Xi[q]
```

The equations describe element-byte copies, with no rescaling, dtype promotion, rounding or alpha/color arithmetic. Float signed zeros, Inf and NaN payloads are preserved exactly.

For A, [H,W] planes may produce [C,H,W] or [H,W,C]. For B, [3,H,W] and [H,W,1] may produce [H,W,4].

C uses its destination-indexed source selection (see FMT-02C). Explicit scalar records map to source coordinate [0]. Nonchannel axes correspond by index only; there is no comparison of coordinate roles, units, origins or sampling grids and no spatial alignment. Spatial inputs agree on their physical height/width axes.

The output uses the first source's schema id and batch prefix, with the inferred channel dimension, atomic_trailing_axes=0 and projected spatial topology.

## Interpretation handling

Assembly attaches no description to its output. A Gray or Black/White plane may supply what a later consumer treats as an RGB, alpha, Lab or other component. Three Gray planes assembled by A become an RGB image only for a consumer whose explicit description declares an RGB group on those indices; this is interpretation, not a numerical color transform. Combining samples from sources that the caller considers differently encoded is the caller's responsibility; the family does not compare or reject source interpretations.

Every result copies original element bits. UInt8 255 does not become Float32 1.0 and binary values are not expanded to another range. Cast, interval decoding, transfer and gray/XYZ/Lab conversion are separate operators. Packed one-bit file data belongs to the codec boundary.

## Layout policy

Auto and materialize copy into independent dense tiles. Every nonempty `view` request fails with `InvalidArgument` / `ViewUnavailable`. Empty requests validate static inputs and parameters and emit no payload.

## Errors

| Failure | Phase | Status / scope |
| --- | --- | --- |
| Zero inputs, malformed or unsorted `input_descriptions`, missing or out-of-range axis, axis that conflicts with a description, illegal rank insertion, absent or ambiguous C selector | Compile/direct preflight | InvalidArgument / InvalidDomain, schema origin. |
| Dtype mismatch, incompatible nonchannel extents, unsupported tensor rank/type or channel-table length | Compile/direct preflight | TypeMismatch / None. |
| Checked shape/prefix overflow | Validation phase | Preserve the shared NUM/kernel status rather than truncate or wrap. |
| Nonempty forced view | Execution | InvalidArgument / ViewUnavailable. |

NaN/Inf or an out-of-range alpha alone is not an assembly failure.

## Acceptance

Byte-level comparison for all seven dtypes against an independent coordinate interpreter. Compare strict and accelerated profiles bit-for-bit. Cover every legal source/output axis, rank-one channel vectors, component insertion, batch prefixes, prefix boundaries, mixed C structures, omitted and repeated sources, static names/roles and malformed maps. Cover split/reassemble, swizzle/replace and alpha-removal helpers, including scalar/literal expansion.
