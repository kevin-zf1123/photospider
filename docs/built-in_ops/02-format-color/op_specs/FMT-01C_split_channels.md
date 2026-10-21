---
spec_schema_version: 1
id: FMT-01C
parent_id: FMT-01
function: split_channels
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# FMT-01C: author one extraction node per channel

`format::split_channels` is a compile-time graph authoring helper, not a
registered operation. It expands the source into FMT-01A index nodes and
returns one workflow handle for each channel. Inherit the Result, axis, mapping,
interpretation and storage rules from [FMT-01](FMT-01_channel_extraction_contract.md).

## Helper contract

The helper accepts a `WorkflowDocument`, source `WorkflowInput`, inferred
`OperationMetadata` for the source Result, and `ChannelExtractOptions`. The
source Result must contain one tensor member . Metadata and physical
layout are checked against a declared workflow input when present. The helper
appends nodes transactionally; authoring failure leaves the document unchanged.
It reads no samples.

The helper resolves the cell channel axis using the explicit `options.axis` or
the `channel_axis` of `options.input_description`; if both are present they must
agree. When a description is supplied, every generated node receives it as its
`input_description` parameter. Axis values refer to the cell descriptor, not
the complete sample shape; existing batch axes remain a prefix. If the caller's
declaration is known, `split_channels` checks the complete schema and spatial-
topology assertion. Each generated A node carries a domain-separated SHA-256
digest of the complete canonical schema as a bounded 64-character hex string,
plus a separate spatial-topology assertion, for compile-time verification of
forward producer references. The schema digest covers schema identity and
batch axes, reads no samples, and is not a sample hash or validity proof. The
separate spatial-topology assertion is limited to 8,192 bytes; larger assertions
fail during authoring.

For channel count C, the helper emits A nodes in index order, all bound to the
same source edge. It returns `ChannelHandle` entries named `c0` through
`c(C-1)`, each referring to that A node's `values` output. Handles are independent
workflow references, not runtime arrays or automatically exported roots. The
caller selects which handles to connect or publish.

Expansion requires `1 <= C <= 65536` and is also subject to normal graph-node
and output limits. It uses collision-aware node IDs and stages the complete node
set before appending. No special multi-output ABI, registry key, pixel buffer or
complete-input split operation is created.

## Runtime demand and validation

Each consumed handle uses an ordinary fixed A call. Only requested extraction calls produce payload tasks; each task reads the mapped source tile closure and writes an independent output tile. A source tile or producer may cover additional channels. Every result retains batch axes and follows A's keepdims and layout policies; no shared mutable runtime split state is introduced.

For temporary public acceptance, use the UInt8 B/A/R/G tensor and expected red
values from [channel extraction](FMT-01A_extract_channel_index.md#example-and-validation).
Call `split_channels`, request only `c2`, and check its second row `[12,13]` and
that no sibling extraction node runs. Also check that changing the producer's
schema identity or batch axes after authoring fails the producer assertion at
compilation.
Validate a later cross-family assembly composition separately.
