---
spec_schema_version: 1
id: FMT-07
kind: retired_catalog_entry
category: 02-format-color
status: Retired
document_maturity: D1_draft
implementation_status: not_applicable
clarification_status: complete
---

# FMT-07: retired catalog entry

FMT-07 is retired. No FMT-07 primitive, helper, operation key or alias exists. The ID is preserved as a retired catalog reference; FMT-08..18 are not renumbered and FMT-07 is not reused for another capability.

## Reason and successors

[FMT-06](FMT-06_numeric_conversion_contract.md) defines dtype conversion plus default/explicit per-channel interval mapping, with pure-cast mode. A second basic interval encoder would duplicate that behavior. The remaining requirements are assigned as follows.

| Requirement | Successor |
| --- | --- |
| Basic dtype/range conversion, optional pure cast | [FMT-06A](FMT-06A_convert_numeric_format.md) |
| Effective bit depth, legal code sets, encoding/decoding validation | [FMT-06 family](FMT-06_numeric_conversion_contract.md), future members |
| Dither | [GRD-29](../../07-grade/adjustments.md) |
| Halftone | [GRD-30](../../07-grade/adjustments.md); not equivalent to threshold, posterize or dither |
| Reusable random/blue-noise fields | [NOI-01 / NOI-07](../../03-generation/generators.md) |
| Tensor layout / file bit packing | [Input/output codec boundary](FMT_codec_boundary.md) |
| Opaque alpha generation | [FMT-05B](FMT-05B_extract_alpha.md): with an explicit alpha_encoding or FMT-06's dtype-default encoding of coverage [0,1], the code that decodes to coverage one is generated |

The GRD-13 posterize and GRD-14 / MASK-03 threshold boundaries remain separate.
