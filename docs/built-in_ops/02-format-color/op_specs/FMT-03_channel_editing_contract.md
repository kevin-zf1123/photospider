---
spec_schema_version: 1
id: FMT-03
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu
clarification_status: complete
---

# FMT-03: channel reordering and replacement

FMT-03A/B are public authoring helpers for channel editing. Scalar sources carry exact native bits. The helpers have no independent native operation keys and are not aliases of `channel.swizzle`.

Inherits [FMT-common](FMT_common_contract.md) and its NUM numerical baseline. The purpose is to edit a base tensor's channel arrangement or selected channels. The members are [A swizzle](FMT-03A_swizzle_channels.md) and [B replacement](FMT-03B_replace_channels.md).

## Relationship to earlier families

[FMT-01](FMT-01_channel_extraction_contract.md) extracts single components. [FMT-02](FMT-02_channel_assembly_contract.md) assembles ordered single components, concatenates channel blocks or creates a result from explicit multi-source maps. FMT-03 specializes single-base channel editing and reuses FMT-02's exact coordinate and explicit-selector principles. Association arithmetic and alpha-specific policies remain FMT-04/05 responsibilities; editing a channel never runs those operations.

Scalar bit-copy semantics follow the numeric contract. Connected scalar Results have shape `[1]`; authored literals are encoded as native-order bytes with an explicit dtype.

## Family behavior

| Topic | Selected behavior |
| --- | --- |
| Members | A lists every output slot for single-base swizzle/subset/repeat/constants; B changes selected slots and preserves all others and the base channel count. |
| A interpretation | Output carries no description. Callers configure subsequent consumers explicitly. |
| Constant values | General typed scalar constants, rather than only 0/1. |
| Constant source form | Connected same-dtype shape-[1] scalars, plus authoring sugar for typed literals; scalar values may change between Runs while structure stays static. |
| B interpretation | Replacement copies data into destination indices; it does not assign destination labels. |
| B sources | Original base components, external component/channel tensors and explicit scalar constants; all replacements read original inputs simultaneously. |
| Interface kind | Compile-time swizzle/replace authoring helpers returning a `values` edge. |

A takes a base tensor and a static ordered output-slot list. Each slot selects a base component or an explicit constant. It may reorder, omit or repeat source channels. B preserves the base channel count and replaces selected destination slots, leaving unlisted slots unchanged. Both publish new results; neither mutates the input. A nonempty result channel axis is required.

Selections use explicit axis/index arguments or caller-supplied input descriptions, without a fixed HWC assumption. Source bytes are preserved, including nonfinite IEEE patterns. Outputs carry no descriptions or color/alpha validity claims.

The mathematical source of each output coordinate is its effective expression: constants use the scalar, unchanged B slots use the original base, and replaced slots use their replacement. Static validation covers every connected input, including unused sources. Auto/materialize produce independent dense outputs; a nonempty forced view fails `ViewUnavailable`. Empty validates static configuration and emits no payload.

`expected_inputs` is emitted as `result-inputs-v1:` followed by 64 lowercase hexadecimal SHA-256 characters (81 characters total). The digest uses domain separation and covers the complete ordered canonical schema and spatial topology for every connected input, including repeated and unused inputs. It hashes no sample payload and is not a validity proof.

## Interface

Both authoring interfaces return one tensor handle `values`. Input 0 is a Result containing one tensor, `base`, with a positive cell rank and an explicitly resolved cell-axis a. Every other nonscalar input Result also contains one tensor. Axis parameters index cell axes and exclude the Result batch prefix. The full sample rank, including batch and cell axes, is at most 8. Let the channel extent be C and let S be the ordered shape with axis a removed.

Axis resolution follows FMT-01/02: an explicit `input_descriptions` entry or a static axis; no HWC/CHW guess; assertions must match; raw requires an explicit axis. Output retains the base channel-axis position; moving that axis is a separate explicit operation, for example FMT-02B with one input.

For A, the required nonempty static slot list has length M and output shape is insert(S,a,M). Every slot is either a base-channel selector or a constant-source reference; omissions and duplicate source selections are allowed. A one-slot result retains the channel dimension.

For B, the static replacement list maps distinct base destination channels to source expressions. Output shape equals base shape. An empty replacement list is a valid identity edit; duplicate targets fail even if their source bytes agree. Source expressions are original base channels, external component tensors, selected external channel-tensor components, or explicit constants. All spatial sources yield exactly S after removing their declared channel axis, if any. External channel axes may differ from a. A component source cannot have an implicit singleton channel axis. With rank-one base, S is empty and external rank-zero component tensors are unsupported; scalar constants and rank-one channel sources still work. No spatial source is broadcast.

Names and roles are separate selector namespaces with case-sensitive exact unique matches, as in FMT-01B. Source and destination selectors resolve against their original effective input descriptions before any replacement. Source reuse and swaps always read original inputs; the list is not a sequence of in-place assignments. Raw uses index selectors only.

Every nonscalar input has exactly the base batch prefix; scalar inputs are unbatched shape `[1]` values. All inputs, including scalars, have the base dtype: UInt8, UInt16, Int8, Int16, Int64, Float32 or Float64. The logical element bound is 2^40. No implicit cast, rounding, range conversion or NaN canonicalization occurs.

| Field | Meaning / default |
| --- | --- |
| `axis` | Optional static Int64 cell-axis index in [0,base_cell_rank); required when no explicit input description supplies a channel axis. Batch axes are excluded. |
| `input_descriptions` | Optional map of input ordinals to explicit TensorDescription values. Used for axes and name/role resolution. |
| `layout` | auto by default; view/materialize have the FMT-02 meanings and failures. |
| `profile` | strict by default, or the named Apple Silicon/x86-64 CPU profile; selects the corresponding primitive keys. |
| A: `slots` | Static ordered list of base selectors or constant references; defines every output slot and M. |
| B: `replacements` | Static list of unique target selectors and source expressions. External sources have explicit component/channels/scalar structure and axis information. |
| Scalar source | Connected single-tensor Result of unbatched shape [1], read at index zero. Its value may change between executions; its dtype/shape and map structure do not. |
| Typed literal | Authoring shortcut for a scalar source with an explicit dtype and exact represented bits. |

A scalar that supplies a channel is constant over that channel's spatial domain, not necessarily constant across Runs. An ordinary shape-[1] spatial tensor does not become a constant unless the source expression says so. A literal never silently casts a different scalar dtype to the base dtype. Int64 values and all floating special bits are preserved, not routed through an untyped Float64 parameter.

## Mathematics

For requested output coordinate j, write k=j[a] and u=erase(j,a). Resolve the effective source expression e(k): A uses slots[k]; B uses its replacement if present, otherwise original base channel k.

```
base channel s:        Y[j] = base[insert(u,a,s)]
external component X:  Y[j] = X[u]
external channel s:    Y[j] = X[insert(u,source_axis,s)]
constant scalar V:     Y[j] = V[0]
```

Every equality is a byte copy at one output coordinate. The relation includes simultaneous swaps and excludes overwritten base samples that no effective output expression selects. A scalar is read if any destination in the retained output selects it.

## Explicit interpretation

A and B copy or fill bytes at selected indices. They neither inherit source labels nor assign destination labels. The next operation receives its own explicit description. Supplied input descriptions are validated against their structural descriptors and used only to resolve axes and selectors. Dropping alpha does not flatten against a background. Replacing an alpha slot does not multiply or divide color samples or scan a coverage interval. FMT-04 owns association arithmetic and FMT-05 owns alpha-specific editing.

## Authoring behavior

Descriptor validation covers the original base and every connected source, including unused sources and all-constant outputs. Supplied descriptors must agree with inference. Channel counts and output structure are determined from static descriptors without executing input producers.

Authoring is transactional as in [FMT-01C](FMT-01C_split_channels.md): failure leaves the caller's graph unchanged. Added node IDs do not collide with existing IDs, and the resulting graph must satisfy the host graph, port and root limits.

## Errors

| Failure | Phase | Outcome |
| --- | --- | --- |
| Empty A slots, duplicate B targets, invalid axis/index/name/role, malformed literal/source kind or invalid explicit input description | Authoring/compile preflight | InvalidArgument / InvalidDomain, schema origin. |
| Full sample rank above 8, batch-prefix mismatch, cell-rank/dtype/nonchannel-shape mismatch, or scalar batch/shape other than unbatched [1] | Preflight | TypeMismatch / None. |
| Checked count/graph/map capacity, unavailable forced view | Inherited phase | Preserve FMT-02/NUM/kernel status and diagnostic. |

Nonfinite sample bytes and alpha outside [0,1] do not fail a copy/fill operation.

## Acceptance

Verify against a byte oracle covering identity, partial/disjoint ROI, source reuse, channel-axis positions, constant-only requests, simultaneous swaps, overwritten-value nonreads, explicit selector descriptions, view/materialization and special-value bits. Member files specify analytic fixtures.
