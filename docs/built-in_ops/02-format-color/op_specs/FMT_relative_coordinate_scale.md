---
spec_schema_version: 1
id: FMT-relative-coordinate-scale
kind: shared_data_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# Relative color-coordinate scales

Confirmed on 2026-09-23 during FMT-11 clarification. This target revises the
CIELAB/CIELCh lightness coordinate used by NUM/CRV/FMT descriptions. It supersedes
earlier native L*=0..100 storage statements, including earlier FMT-11 decisions.
Ordinary NUM rounding, arithmetic, demand and precision are unchanged. This
documentation change does not implement a metadata/runtime migration.

## Canonical relative coordinates

Relative lightness/luminance components use reference scale one. The nominal
0..1 interval is not a validity clamp: finite negative and HDR values remain
permitted where the member formula admits them. A formula's explicit singular
or transfer domain rules still apply.

| Model/coordinate | Canonical storage |
| --- | --- |
| CIELAB | l,a*,b*, with l=L*/100 |
| CIELCh(ab) | l,C*,h, with the same l=L*/100 |
| CIELAB-lightness Gray | l=L*/100 |
| OKLab/OKLCh lightness and OKLab-lightness Gray | Existing L scale; no rescaling or forced white residual correction |
| HSL lightness, HSV value | Existing L/V scale |
| Relative XYZ/xyY luminance and linear-Y Gray | Existing Y scale, reference-white Y=1 |
| Encoded luma Gray | Existing native Y' scale, nominally 0..1 |

Keep CIELAB a*,b*,C* in their existing scales. Keep OKLab opponent/chroma,
YCbCr chroma, hue units/winding and CMYK ink coordinates unchanged. XYZ X/Z
share Y's tristimulus scale; do not independently divide them by their white
components. A white may therefore have X/Z greater than one. Color components
remain semantically distinct despite similar magnitudes.

For CIELAB formulas, the mathematical standard quantity remains L*=100*l.
Substitute that exact relation into the whole formula, then follow the member's
rounding boundary. Do not first round a temporary L* and thereby add an
unrequested numeric conversion. Polar conversion and interpolation use stored l
directly. The normalized coordinate is native under this revised target, not
an extra non-native FMT-06 code encoding that every model operator must undo.

Example: normalized CIELAB l=0.5 means L*=50 and relative Y=(66/116)^3, about
0.1841865. Explicitly reinterpreting it as linear RGB value 0.5 is permitted by
the override/assignment rules but changes color meaning. Equal scales neither
establish equivalent colors nor replace a CMYK profile.

## Absolute units and numeric encoding

Absolute cd/m² values, including FMT-09 PQ/BT.1886 and FMT-10 absolute RGB/XYZ,
retain their explicit units. Converting them to relative values requires an
explicit reference luminance and numerical normalization; no display peak,
white brightness or exposure is inferred. Native relative coordinates can still
be explicitly encoded by FMT-06 into integer or other numeric intervals.

## Migration and compatibility boundary

Current ColorArray v1 assigns implicit model coordinate units, including old
CIELAB L*. Do not reinterpret those existing bytes as l or infer the scale from
sample magnitudes. The new generic semantic schema must identify the revised
coordinate convention; complete-group validation, authoring, caches, LUT axes,
tables, sample fixtures and consuming operations must migrate together.
Old payloads require explicit import conversion or retirement, not a silent
compatibility alias. No new persisted schema/version number is selected here.
Closing the [shared representation implementation gate](FMT_common_contract.md#shared-representation-implementation-gate)
is mandatory before registering consumers or persisting the revised descriptions.
The document front matter's `spec_schema_version: 1` versions the specification
format only; it cannot identify either old or revised runtime coordinate units.

The existing interpolation algorithms are scale-linear, but that fact alone
does not migrate their public metadata or runtime evidence. Affected specs mark
the revised semantic portion pending. Historical implementation sections and
their test counts describe the old representation, not conformance to this
target. Future acceptance includes reference white l=1, extended values,
same-scale metadata matching and rejection of silently mixed old/new meanings.

Sources: [CIE CIELAB definition](https://cie.co.at/eilvterm/17-23-076) defines
standard L*. Division by 100 and the scope of this storage change are explicit
project decisions, not a claim that CIE redefines L* or that every color channel
has the same physical meaning.
