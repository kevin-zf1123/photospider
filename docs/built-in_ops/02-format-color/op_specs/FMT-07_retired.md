---
spec_schema_version: 1
id: FMT-07
kind: retired_catalog_entry
category: 02-format-color
status: Retired
document_maturity: D1_draft
implementation_status: not_applicable
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-07: retired catalog entry

The maintainer retired FMT-07 during its clarification. Do not introduce an
FMT-07 primitive, helper, new operation key or compatibility alias. Preserve the
ID as a retired catalog reference; do not renumber FMT-08..18 or silently reuse
FMT-07 for an unrelated capability. This is a specification allocation record,
not an executable operator contract or ADR.

## Reason and subsequent runtime retirement

[FMT-06](FMT-06_numeric_conversion_contract.md) already defines dtype conversion
plus default/explicit per-channel interval mapping, with pure-cast mode. A second
basic interval encoder would duplicate that behavior. The remaining requirements
are assigned to the appropriate existing or future families below instead.

The maintainer subsequently authorized removal of the legacy numeric.encode_range
implementation together with numeric.cast and the old format/color nodes.
Package 0.20.0 performs that [separate implementation retirement](FMT_legacy_retirement.md).
Neither old path is retained as a conforming FMT-06 implementation.

## Confirmed successor allocation

| Requirement | Successor | Scope of this decision |
| --- | --- | --- |
| Basic dtype/range conversion, optional pure cast | [FMT-06A](FMT-06A_convert_numeric_format.md) | Already clarified; no behavior change. |
| Effective bit depth, legal code sets, encoding/decoding validation | [FMT-06 family](FMT-06_numeric_conversion_contract.md), future members | Assigned for later clarification; do not expand A's dtype-overflow rule into a code-range rule. |
| Dither | [GRD-29](../../07-grade/adjustments.md) | Newly allocated independent family; concrete members, algorithms and regional/temporal rules remain unselected. |
| Halftone | [GRD-30](../../07-grade/adjustments.md) | Newly allocated independent family; no automatic equivalence with threshold, posterize or dither. |
| Reusable random/blue-noise fields | [NOI-01 / NOI-07](../../03-generation/generators.md) | Reuse their future explicit source contracts where appropriate; this allocation adds no RNG or noise algorithm. |
| Tensor layout / file bit packing | [Input/output codec boundary](FMT_codec_boundary.md) and kernel storage | Physical representation belongs there, including packed 1/10/12-bit payloads; no packing primitive is added by this record. |
| Opaque alpha generation | [FMT-05B](FMT-05B_extract_alpha.md) | Rule finalized below and incorporated into the member spec. |

Catalog numbers outside FMT-07 remain stable. Follow-up work must clarify each
allocated family's real formulas, ports, support and tests rather than treating
this allocation as completed operator specifications or implemented functionality.
The existing GRD-13 posterize and GRD-14/ MASK-03 threshold boundaries remain
separate. No FMT-07 helper, key or implementation gate remains to be fulfilled.

## Opaque-alpha dependency resolution

For FMT-05B missing_alpha=opaque, optional explicit alpha_encoding selects the
output component's numeric encoding and legal code domain. Generate the code
that decodes to mathematical coverage one. Without an explicit encoding, use
FMT-06's dtype-default encoding of coverage [0,1]: Float32/64 one, UInt8 255,
UInt16 65535, Int8 127, Int16 32767 and Int64 2^63-1. Do not infer an alpha
encoding from an unrelated color channel when the alpha relationship is absent.

An explicit 10-bit [0,1023] code interval yields 1023 in UInt16. An explicit
reversed encoding can yield zero. Require the coverage-one code to be legal
and exactly representable in the output dtype; otherwise fail rather than
rounding to a merely almost-opaque value. Existing alpha extraction remains a
byte copy and is not re-encoded by this fallback parameter. The codec/metadata
and any missing native dtype are still implementation dependencies, but there
is no unresolved dependency on FMT-07.

## Verification and delivery boundary

This change retires a specification ID, allocates successors and resolves an
existing specification dependency. It implements none of those operators,
removes no legacy runtime code and claims no executable workflow or benchmark.
Validate links and the exact opaque-code examples when updating the dependent
specifications; future runtime work needs its own public API acceptance evidence.
