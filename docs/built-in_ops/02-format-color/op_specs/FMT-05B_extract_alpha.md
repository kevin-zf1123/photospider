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

FMT-05B remains Proposed. The public `format::extract_alpha` helper is compiled
and installed through `photospider/ops/format/alpha.hpp`. It appends an executable
Result composition over the registered `channel.extract_index_<profile>` key
when the selected group has alpha. For `missing_alpha=opaque`, it computes the
exact same-dtype code that decodes to coverage one and uses the registered
`channel.literal_like_<profile>` Result operation. Neither path adds an alpha
registry key. The helper accepts and publishes a single-tensor Result without
fields; it supports UInt8, UInt16, Int8, Int16, Int64, Float32 and Float64, keeps
the batch prefix, and performs no sample-domain validation.

The cell-axis `axis` excludes the Result batch prefix. The complete sample rank,
including batch and cell axes, is at most 8, and full sample count is at most
2^40. Required source assertions bind the complete schema and physical layout.
The helper is a compile-time composition over registered Result operations; it
does not add an alpha-specific runtime key. Independent alpha math oracles cover numerical kernels, not workflow execution. Inherit target alpha semantics
from the complete [FMT-05 contract](FMT-05_alpha_editing_contract.md).

## Interface and inference

Inherit group, required input_description, cell-axis `axis`,
layout and profile. `axis` excludes the batch prefix. Static missing_alpha is error by default or opaque explicitly.
Static Bool keepdims defaults to false. Optional static alpha_encoding describes
only the missing_alpha=opaque fallback and is otherwise invalid. Validate any
explicit descriptor structurally; an existing alpha is still copied unchanged.
Resolve the selected group's internal alpha index; never search for a coincidentally named A/Alpha channel or assume
that the final slot is alpha. No external alpha binding can satisfy this lookup.

Output preserves input dtype and spans the complete nonchannel shape S.
For a channel-bearing input, false removes the channel axis; true retains it with
extent one. A rank-one channel vector requires true to avoid rank zero.
For a component Gray input with no channel axis, either keepdims value retains
its original shape and invents no axis. Such an input has no internal alpha,
so error or the explicit opaque policy determines the result.

Existing alpha is copied exactly for all family copy dtypes, including signed
zeros and NaN payloads. No scan of color or alpha range occurs. Opaque instead
generates the legal exactly representable code whose decoder gives coverage one,
using explicit alpha_encoding or the FMT-06 dtype-default encoding of [0,1].
Defaults are Float32/64 one, UInt8 255, UInt16 65535, Int8 127, Int16 32767 and
Int64 2^63-1. Explicit UInt16 10-bit [0,1023] generates 1023; reversed [255,0]
coverage encoding in UInt8 generates 0. No encoding is guessed from a color
channel or from the image's model name.

Resolve the opaque code using exact descriptor arithmetic before generating
pixels. Encoding syntax and structural ambiguity are checked during authoring.
The caller guarantees an invertible selected encoding with one legal exactly
representable coverage-one code in the output dtype; these numeric premises
are unchecked even though the descriptor is static. Exact-arithmetic capacity
exhaustion remains an operational ResourceExhausted failure.
Do not round to a neighboring code, pass Int64 maximum through Float64 or
replace missing alpha with the dtype maximum despite an explicit encoding.
The caller supplies the selected decoder and coverage interpretation to any later consumer; the output carries no description.
The seven listed copy dtypes are supported. FMT-07 remains retired. Existing
integer samples retain their bits without normalization.

## Lowering, metadata and regional behavior

For existing alpha, resolve its static index and expand FMT-01A with matching
axis/keepdims/profile/layout and explicit source description. For opaque, expand
the exact same-dtype opaque-code constant into the output shape using conforming
tile storage. Preserve static shape/group dependencies without reading input pixels. Both existing-alpha extraction and opaque generation use independent dense output tiles; nonempty forced view is unavailable.

Output is an ordinary component tensor with no attached description or persistent
alpha relationship to the input image. Copied output storage has independent ownership. Existing sample validity is not certified merely by extraction.

The helper serializes a per-source `result-v1` assertion with a digest of the
complete canonical schema plus a separate spatial-topology assertion. The
canonical schema includes structural identity, dtype, shape, batch/atomic and spatial axes; the layout
assertion encodes spatial height/width/channel axes. The physical Plan and Run binding separately validate actual strides, offsets and capacities. Compilation/direct
preparation checks the producer descriptor, including for forward references.
Static helper expansion is transactional.

For auto/materialize requested output q, insert the original alpha index at the removed axis
(or replace its singleton coordinate when keepdims=true) to obtain the sole
source coordinate. Only selected alpha coordinates contribute to the copied output; other color sample values are not inspected. This sample mapping does not define incremental invalidation. Opaque has no source pixel dependency. Both paths retain
static schema dependencies. Required payload producers retain their declared failure scope; a nonempty forced-view task reads no payload and reports ViewUnavailable.

Use transactional graph expansion, inherited resource/work accounting, exact
coverage, context-independent output lifetime, cancellation and errors from the
family. An absent alpha with error fails preflight. Opaque fallback only handles
an absent alpha relation, not an existing alpha channel with missing coverage.

## Fixed opaque generation

The opaque branch uses fixed invocation ABI 8. Each selected output tile lowers to one CPU Host task with an input schema dependency and no payload reads. Static preparation retains complete source assertions and computes the output schema, including batch, cell, atomic and spatial axes. Kernel preparation checks the symbol, output geometry, empty read/task-parameter arrays and zero scratch. An unused source producer can remain elided even when its sample implementation would fail.

Auto and materialize fill preallocated output tiles from the prepared native bytes. Every supported dtype is copied bitwise, including signaling NaNs for direct literal calls. The physical Plan proves dense positive output strides, so invocation fills the logical tile linearly with constant-size local state. It polls cancellation between at most 256 writes and allocates no arithmetic workspace. The Plan admits selected output storage; its size is independent of the unrequested logical extent. View on a nonempty selection returns InvalidArgument/InvalidDomain because absent pixels require generation; Empty still validates static parameters but emits no payload task. Independent Runs share immutable preparation and own their output storage. Retained results survive source and execution-context teardown, and failures publish no partial call result.

## Independent acceptance

For Float32 RGBA [0.5,0.25,1,-0], output alpha is the exact -0 bit pattern.
Alpha NaN/out-of-range values copy unchanged even when RGB peers are invalid.
For UInt8 alpha byte 255, output remains UInt8 255; no conversion to one occurs.
A later coverage consumer owns its own semantic validation.

An input shape [2,3,4] with channel axis 2 gives [2,3] by default or [2,3,1]
with keepdims. A channel vector [4] requires keepdims and yields [1]. A no-alpha
Float32 Gray [2,3] with opaque yields [2,3] ones for either keepdims setting. Error mode
rejects the same missing relation. Default UInt8 opaque generates 255; explicit
10-bit UInt16 generates 1023.
A reversed UInt8 encoding generates 0 while still decoding to coverage one.
Check Int64 maximum is constructed exactly. Explicit alpha_encoding must not
re-encode an existing alpha, including a NaN or out-of-range actual sample.

Verify that opaque requires no image sample reads and that an existing but
unproduced alpha is not replaced by an opaque code. Compare whole/disjoint/offset
and cross-tile requests using an independent coordinate enumeration and byte oracle.
An opaque encoding must represent decoded coverage one exactly within its
legal stored interval. Integer codes {0,1} decoding to 2*k would require code
0.5, and codes [0,9] decoding to k/10 would require excluded code 10. Both
configurations violate unchecked numerical caller premises. Independent oracle
diagnostics may identify them; production acceptance uses representable opaque
codes and confirms that no image samples are read.

The conceptual target workflow is a described image -> B -> alpha component
processing. `format::extract_alpha` now appends the FMT-05B helper composition. Existing alpha
uses `channel.extract_index_<profile>`; opaque fallback uses
`channel.literal_like_<profile>` with exact prepared bits and descriptor-only
source demand. Validate the helper expansion through the public compile/execute path; checking
the reused primitive alone does not establish composition behavior.

## Current execution evidence

[Implementation and validation](../FMT-14-15_implementation.md) records the temporary public compile/execute
checks for all seven copy dtypes, exact opaque encodings, shared-alpha behavior
and transactional failures. Mathematical oracle coverage alone does not
establish helper execution.
