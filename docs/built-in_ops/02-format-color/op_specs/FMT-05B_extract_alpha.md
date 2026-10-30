---
spec_schema_version: 1
id: FMT-05B
parent_id: FMT-05
function: extract_alpha
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# FMT-05B: extract internal alpha or explicitly generate opaque alpha

The public `format::extract_alpha` helper appends a Result composition over the registered `channel.extract_index_<profile>` key when the selected group has alpha. For `missing_alpha=opaque`, it computes the exact same-dtype code that decodes to coverage one and uses the registered `channel.literal_like_<profile>` Result operation. Neither path adds an alpha registry key. The helper accepts and publishes a single-tensor Result without fields; it supports UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64, keeps the batch prefix, and performs no sample-domain validation.

The cell-axis `axis` excludes the Result batch prefix. The complete sample rank, including batch and cell axes, is at most 8, and full sample count is at most 2^40. Inherits target alpha semantics from the [FMT-05 contract](FMT-05_alpha_editing_contract.md).

## Interface

Inherits group, required input_description, cell-axis `axis`, layout and profile. Static missing_alpha is error by default or opaque explicitly. Static Bool keepdims defaults to false. Optional static alpha_encoding describes only the missing_alpha=opaque fallback and is otherwise invalid; an explicit descriptor is validated structurally; an existing alpha is copied unchanged. The selected group's internal alpha index is resolved from the description; a coincidentally named A/Alpha channel is never searched and the final slot is never assumed to be alpha. No external alpha binding can satisfy this lookup.

Output preserves input dtype and spans the complete nonchannel shape S. For a channel-bearing input, false removes the channel axis; true retains it with extent one. A rank-one channel vector requires true to avoid rank zero. For a component Gray input with no channel axis, either keepdims value retains its original shape and invents no axis; such an input has no internal alpha, so error or the explicit opaque policy determines the result.

The helper serializes a per-source `result-v1` assertion with a digest of the complete canonical schema plus a separate spatial-topology assertion; the canonical schema includes structural identity, dtype, shape, batch/atomic and spatial axes. Compilation checks the producer descriptor, including forward references.

## Mathematics

Existing alpha is copied exactly for all dtypes, including signed zeros and NaN payloads. No scan of color or alpha range occurs. For requested output q, the original alpha index is inserted at the removed axis (or replaces its singleton coordinate when keepdims=true) to obtain the sole source coordinate. Other color sample values are not inspected.

Opaque instead generates the legal exactly representable code whose decoder gives coverage one, using explicit alpha_encoding or the FMT-06 dtype-default encoding of [0,1]. Defaults are Float32/64 one, UInt8 255, UInt16 65535, Int8 127, Int16 32767 and Int64 2^63-1. Explicit UInt16 10-bit [0,1023] generates 1023; reversed [255,0] coverage encoding in UInt8 generates 0. No encoding is guessed from a color channel or from the image's model name.

The opaque code is resolved by exact descriptor arithmetic before generating pixels. The caller guarantees an invertible selected encoding with one legal exactly representable coverage-one code in the output dtype; these numeric premises are unchecked. Neighboring codes are not substituted, Int64 maximum is never passed through Float64, and the dtype maximum never replaces missing alpha despite an explicit encoding. The output carries no description; the caller supplies the decoder and coverage interpretation to later consumers. Existing integer samples retain their bits without normalization.

## Behavior

Opaque has no source pixel dependency, only the schema dependency; an unused source producer is not required. Both existing-alpha extraction and opaque generation write independent dense output; auto/materialize are accepted. A nonempty forced view returns `InvalidArgument / InvalidDomain`. Empty validates static parameters and emits no payload. An absent alpha with error fails preflight. Opaque fallback handles an absent alpha relation only, not an existing alpha channel with missing coverage.

Output is an ordinary component tensor with no attached description or persistent alpha relationship to the input image.

## Acceptance

For Float32 RGBA [0.5,0.25,1,-0], output alpha is the exact -0 bit pattern. Alpha NaN/out-of-range values copy unchanged even when RGB peers are invalid. For UInt8 alpha byte 255, output remains UInt8 255; no conversion to one occurs.

An input shape [2,3,4] with channel axis 2 gives [2,3] by default or [2,3,1] with keepdims. A channel vector [4] requires keepdims and yields [1]. A no-alpha Float32 Gray [2,3] with opaque yields [2,3] ones for either keepdims setting. Error mode rejects the same missing relation. Default UInt8 opaque generates 255; explicit 10-bit UInt16 generates 1023. A reversed UInt8 encoding generates 0 while still decoding to coverage one. Int64 maximum is constructed exactly. Explicit alpha_encoding does not re-encode an existing alpha, including a NaN or out-of-range actual sample.

Opaque encodings that cannot represent decoded coverage one exactly within the legal stored interval violate the caller premises: integer codes {0,1} decoding to 2*k would require code 0.5, and codes [0,9] decoding to k/10 would require excluded code 10.

## Conceptual DAG

described image -> B -> alpha component processing
