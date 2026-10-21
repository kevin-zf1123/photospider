---
spec_schema_version: 1
id: FMT-relative-coordinate-scale
kind: shared_data_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# Relative color-coordinate scales

NUM/CRV/FMT descriptions and consumers use native CIELAB/CIELCh lightness
`l=L*/100`. This supersedes earlier native L*=0..100 storage statements,
including earlier FMT-11 decisions. Ordinary NUM rounding, arithmetic, demand
and precision are unchanged. Consumers use the existing TensorDescription v4/v5
codecs described in the
[runtime contract](../../../kernel-architecture/Tensor-Semantic-Metadata.md).
The independent NUM/CRV migration is complete; no kernel format changed.

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

The old ColorArray v1 public types, codec, validation helpers and overloads have
been removed. NUM/CRV now use explicit TensorDescription v4/v5 tuple parameters,
the same native convention used by FMT. Old ColorArray bytes are rejected;
there is no decoder, unit alias or inference from sample magnitude. The generic
TensorDescription codec itself is unchanged by this migration.

Callers must explicitly convert old L* sample coordinates to l, reauthor LUT
input axes and table output coordinates, regenerate sample fixtures and
recompile workflows. A logical NUM tuple uses `channel_axis=0`; an FMT description
uses the actual tensor's channel axis. Both retain the same coordinate meaning.
CIELAB a*,b*,C*, OKLab coordinates, hue winding and CMYK inks are not rescaled.

CRV-09 uses configuration hash domain `photospider.bake-configuration.v2` and
owned-table/report schema v4. Old reports and tables are retired, not silently
accepted. Reauthor validation points and tolerances in the new stored units.
CRV-09 has one scalar `atol` for all three components: dividing it by 100 does
not preserve an old mixed lightness/opponent acceptance test. Choose the intended
new-coordinate tolerance and regenerate the bake and report. The relative
tolerance formula remains unchanged.

Acceptance covers white l=1, midpoint l=0.5, finite negative/HDR lightness,
unchanged large opponent/chroma values, both floating dtypes, consistent explicit
descriptions, old payload rejection and a CRV Lab ramp followed by FMT XYZ
conversion. See [executed migration evidence](../../01-numeric/NUM-CRV_color_migration.md).
The Markdown `spec_schema_version` versions the specification document only.

Sources: [CIE CIELAB definition](https://cie.co.at/eilvterm/17-23-076) defines
standard L*. Division by 100 and the scope of this storage change are explicit
project decisions, not a claim that CIE redefines L* or that every color channel
has the same physical meaning.
