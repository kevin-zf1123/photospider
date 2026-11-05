---
spec_schema_version: 1
id: GRD-29
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# GRD-29: Ordered, explicit-noise and error-diffusion dither

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-29A](GRD-29A_dither_bayer.md) | Bayer ordered dither | `grade.dither_bayer` |
| [GRD-29B](GRD-29B_dither_noise.md) | Dither with explicit noise | `grade.dither_noise` |
| [GRD-29C](GRD-29C_dither_floyd_steinberg.md) | Floyd-Steinberg uniform-level dither | `grade.dither_floyd_steinberg` |
| [GRD-29D](GRD-29D_dither_ordered_map.md) | Dither with an explicit threshold map | `grade.dither_ordered_map` |
| [GRD-29E](GRD-29E_dither_palette_ordered.md) | Ordered palette dither | `grade.dither_palette_ordered` |
| [GRD-29F](GRD-29F_dither_palette_noise.md) | Palette dither with explicit noise | `grade.dither_palette_noise` |
| [GRD-29G](GRD-29G_dither_palette_floyd_steinberg.md) | Floyd-Steinberg palette dither | `grade.dither_palette_floyd_steinberg` |

## Shared mathematics and behavior

### Uniform quantization

After edge handling, u lies in [0,1]. z=u*(levels-1), k=floor(z), f=z-k. Given threshold t, q=min(levels-1,k+(1 if t<f else 0)); output q/(levels-1). Endpoints 0/1 are exact. Bayer and authored maps supply periodic thresholds; noise supplies a matched input. Threshold equality chooses the lower level.

### Floyd-Steinberg recurrence

Visit rows from the full-image top to bottom. raster visits each row left-to-right. serpentine visits even global integer y left-to-right and odd y right-to-left. Initialize errors to zero. A pixel adds its accumulated error, quantizes, computes the error against the Float64 reconstruction value and distributes 7/16 horizontally forward, 3/16 next-row backward, 5/16 directly below and 1/16 next-row forward, in that order. Reverse horizontal directions on reverse rows. Discard contributions outside the full image. Each multiplication, reconstruction, subtraction and addition to error state rounds to Float64; the oracle rounds each of these stages strictly. Final scalar output is rounded to T. Spatial requests retain the canonical predecessor state.

### Palette selection

The palette is an explicit table of RGB entries in the source linear RGB space and dtype. Transform source and palette to Float64 OKLab using the common D65 route. K=1 returns the sole palette entry. Duplicate entries retain their index order. Output colors are always original supplied RGB entries, copied without reconstruction.

For E/F consider each pair of indices i<j with distinct OKLab vectors P_i,P_j. Compute f=clamp(dot(X-P_i,P_j-P_i)/dot(P_j-P_i,P_j-P_i),0,1) and projected color Q=P_i+f*(P_j-P_i). Choose the pair with minimum squared distance to X, breaking equal distances lexicographically by (i,j). If all entries coincide use index zero. The scalar f supplies the probability/ordered threshold fraction for choosing j instead of i. This is a native nearest-segment palette model. For G, each error-adjusted OKLab value selects its nearest palette point and diffuses the OKLab residual. Float64 converted tuples are explicit boundaries before segment selection or recurrence.

Palette generation, RNG generation, storage quantization and packed file output are independent operations. The color space and error-vector definitions here specify the image effect; they do not imply a particular search data structure.


## Current parameter encoding

Color selectors use `axis:Int64` with ordered, distinct `components:String` indices, or `input_description:String` with `group:String`. The axis is relative to the cell descriptor before any batch prefix. The shared work parameter is `max_work:Int64`, default 9223372036854775807. See [static selector encoding](GRD-21_saturation_contract.md#static-parameter-encoding).

Fixed static arrays use comma-separated String values in the declared row-major order, without brackets. Float64 entries use decimal numeric text; Int64 entries use signed decimal integers. Their declared lengths are checked during static preparation. Dynamic tensors in member signatures remain required ordered input ports.

These members process one two-dimensional image. The selected component axis may vary; explicit noise remains in logical y,x,selected-component order.

| Member | Parameter | Current type and default |
| --- | --- | --- |
| All | origin_x / origin_y | Int64 / Int64, 0 / 0; global image origin |
| A/B/C/D | levels | Int64, 2; legal range 2..65536 |
| A/B/C/D | edge | String, clamp; clamp or reject |
| A | order | Int64, 2; legal range 1..8 |
| A/D/E | origin | String, 0,0; two Int64 pattern-origin coordinates |
| A/D | channel_phase | String; 2*Csel Int64 entries, omitted means all zero |
| C/G | scan | String, serpentine; serpentine or raster |

Each channel-phase pair is phase_x,phase_y in selected-component order. Pattern coordinates are global image origin plus logical pixel coordinate minus pattern origin plus channel phase. E has no channel phase.

E/F/G select complete relative linear RGB. Raw mode declares a resolvable source_basis:String; semantic mode resolves the basis and white from its description, and explicit basis assertions must agree. Palette is a dynamic matching-dtype T[K,3] tensor. Its original row order and source bits define output palette identity.

## Current native arithmetic and prototype

Uniform quantization follows the common calculation-dtype rule. Bayer and authored-map thresholds are Float64. B uses Float32 intermediates only when both source and noise are Float32; either Float64 operand promotes the arithmetic to Float64. The threshold comparison remains strict. C/G use the explicitly ordered Float64 recurrence, including reconstruction, residual, weighted contribution and error-state additions.

C evaluates one canonical full-image recurrence for each requested selected component and publishes the retained output tiles. G prepares its Float64 OKLab palette once and evaluates one canonical vector-error recurrence for the retained request. Scan state is not reset at tile boundaries.

E/F prepare the palette's Float64 OKLab tuples and ordered candidate segments once for the current evaluation and share them across retained output tiles. Their source dependence remains elementwise plus the palette and corresponding threshold/noise support. No dynamic palette state is retained for a later evaluation with different inputs. Selection preserves the specified pair/point tie order and copies original palette RGB bits.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
