---
spec_schema_version: 1
id: FMT-05B
parent_id: FMT-05
function: extract_alpha
kind: composite_workflow
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-05B: extract internal alpha or explicitly generate opaque alpha

Inherit the complete [FMT-05 contract](FMT-05_alpha_editing_contract.md).
The proposed authoring helper `extract_alpha` takes a graph and described input
edge, returning one `values` edge. It expands into conforming extraction or
constant-generation nodes; it is not a separate native registry entry.

## Interface and inference

Inherit group, metadata_mode, conditional metadata_override, axis, layout and
profile. Static missing_alpha is error by default or opaque explicitly.
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
pixels. After applying explicit/default selection, an unresolvable, noninvertible
or ambiguous encoding, or the absence of a legal exactly representable
coverage-one code, fails InvalidArgument/InvalidDomain.
Do not round to a neighboring code, pass Int64 maximum through Float64 or
replace missing alpha with the dtype maximum despite an explicit encoding.
The output alpha component carries the selected decoder and coverage meaning.
Native dtype/encoding support remains an implementation dependency; there is no
pending FMT-07 contract after that ID's retirement. Existing integer samples
retain their own encoding description without normalization.

## Lowering, metadata and regional behavior

For existing alpha, resolve its static index and expand FMT-01A with matching
axis/keepdims/profile/layout and effective source metadata. For opaque, expand
the exact same-dtype opaque-code constant into the output shape using conforming
image storage. Preserve static shape/group dependencies without reading input
pixels. A generic zero-stride constant does not automatically satisfy planar
image storage. Force view cannot generate absent opaque pixels.

Output is an independent alpha component with projected axis/coordinate and
encoding descriptions. It has no complete color-group claim or persistent
relationship to the input image. A view can retain the normal backing owner;
this is storage ownership, not a semantic alpha attachment. Existing sample
validity is not certified merely by extraction.

For requested output q, insert the original alpha index at the removed axis
(or replace its singleton coordinate when keepdims=true) to obtain the sole
source coordinate. Dirty input alpha maps inversely; other color channels have
no sample effect. Opaque has no source pixel dependency. Both paths retain
static Descriptor dependencies and the inherited upstream failure rules.

Use transactional graph expansion, inherited resource/work accounting, exact
coverage, context-independent view lifetime, cancellation and errors from the
family. An absent alpha with error fails preflight. Opaque fallback only handles
an absent alpha relation, not an existing alpha channel with missing coverage.

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
For an invalid encoding fixture, let integer code k in {0,1} decode to 2*k:
coverage one would require k=0.5, so opaque generation must fail. For a legal-domain
fixture, let code k in [0,9] decode to k/10: the exact opaque code 10 is excluded,
so fail without clamping to 9. Check both errors before source sample reads.

Conceptual workflow: described image -> B -> alpha component processing.
Implementation must supply runnable public helper/compile/execute coverage,
including transactional failures and all layout modes. No runtime result is
claimed by this Proposed specification.
