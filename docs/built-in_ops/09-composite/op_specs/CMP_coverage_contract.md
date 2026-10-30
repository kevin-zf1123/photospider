---
spec_schema_version: 1
id: CMP-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# CMP: basic coverage composition

Inherit [COM common](COM_common_contract.md). `S,B` are straight source/background color components; `a,b` are their alpha values. Every arithmetic member uses its explicitly named linear or encoded working coordinates. Copy and clear primitives are domain-independent because their behavior does not depend on a color transform.

## Porter-Duff equations

For factors Fs,Fb, A=Fs*a+Fb*b and P=Fs*a*S+Fb*b*B. Output is (P/A,A) for positive A and (+0,A) for zero A. Atop alpha is copied as stated by its member. The factors are:

| Operation | Fs | Fb |
| --- | --- | --- |
| source over | 1 | 1-a |
| destination over | 1-b | 1 |
| source in | b | 0 |
| destination in | 0 | a |
| source out | 1-b | 0 |
| destination out | 0 | 1-a |
| source atop | b | 1-a |
| destination atop | 1-b | a |
| xor | 1-b | 1-a |

Source-over with supplied blended color F uses P=(1-a)*b*B+(1-b)*a*S+a*b*F and A=a+b-a*b. F is a stored input; this primitive does not evaluate a blend-mode dispatcher or a layer tree. Conversion into the compositing domain is explicit upstream.

Raw plus returns separate color_mass=a*S+b*B and mass=a+b, with mass up to 2. Bounded plus clamps A=min(1,a+b) and each P=min(1,a*S+b*B), then returns straight P/A. Raw encoded mass is a weighted sum of encoded coordinates, not a physical light quantity. Bounded plus requires bounded source colors. Neither raw mass nor a color-only add is coverage or independent emission.

## Analytic observations

For S=(1,0,0,0.5) and B=(0,0,1,0.5):

| Operation | Result in the declared coordinates |
| --- | --- |
| source over | (2/3,0,1/3,0.75) |
| destination over | (1/3,0,2/3,0.75) |
| source in / source out | (1,0,0,0.25) |
| source atop / xor | (0.5,0,0.5,0.5) |
| raw plus | color_mass=(0.5,0,0.5), mass=1 |

The same numeric tuple in different color interpretations denotes different colors. Comparing a physical linear route with an encoded-artistic route requires explicit conversion of the same input colors into their respective domains. These mathematical choices follow the [Porter-Duff/W3C references](../research-sources.md#coverage-and-color-blending); they do not prescribe browser or editor layer semantics.
