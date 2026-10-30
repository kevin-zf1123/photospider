---
spec_schema_version: 1
id: BLN-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# BLN: color functions and explicit image interpolation

Inherit [COM common](COM_common_contract.md). Most members are color-only functions with B=background and S=source. Alpha integration is separately expressed by CMP. The bounded/HDR domains and endpoint priority are specified by each member. `normal`, per-channel extrema and whole-color selection are bit-copy choices; ordinary arithmetic is native floating computation.

## Whole-color selection

BLN-17A/B receive linear RGB and compute its Float64 working-basis Y measurement. Resolve the basis Y row to Float64 coefficients r, then use w=r/sum(r), with positive sum(r). Coefficients may be signed. Publish each dot(w,RGB) measure to Float64 before comparison; the oracle normalizes the stored coefficients exactly and strictly rounds this measure. BLN-17C/D receive native OKLab and compare its stored L. BLN-17E/F receive native CIELAB and compare its stored l=L*/100. Each returns the selected complete tuple in the same input model. Explicit model-conversion nodes compose these choices into an RGB workflow; the selection node does not insert a conversion.

## Dissolve integer definition

Use unsigned 64-bit modular arithmetic. Define mix(v): z=v+0x9e3779b97f4a7c15; z=(z xor (z>>30))*0xbf58476d1ce4e5b9; z=(z xor (z>>27))*0x94d049bb133111eb; return z xor (z>>31). Truncate each arithmetic result to 64 bits. Start z=mix(seed interpreted modulo 2^64); for c in [global_x,global_y,frame], set z=mix(z xor (c modulo 2^64)). The result is U. Inputs seed/frame/coordinates are signed Int64 values. A fixed frame gives a stable spatial pattern; changing frame explicitly changes the pattern.

U/2^64 < source alpha chooses opaque source color; otherwise retain background color/alpha. This is a finite discrete random threshold, with no claim of an exactly proportional count in every tile. Seed and coordinate behavior are part of the algorithm; a random library choice cannot change them.

## Color addition and coverage interpolation

BLN-19 adds a linear RGB signal while retaining background alpha. It is distinct from the existing independently visible Layer emission representation. BLN-20 interpolates associated color and alpha, then returns straight coordinates. Its linear and encoded variants use separate identities. A same-grid t field lets an explicitly constructed soft mask control the interpolation. Other layer/mask policies remain compositions.

## Formula sources

The [research sources](../research-sources.md) distinguish W3C formula definitions from native additions. Hard Mix through vivid-light threshold and through B+S>=1 are separate definitions; at B=0,S=1 they return 0 and 1 respectively. No UI name selects an undocumented alternative formula.
