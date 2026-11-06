---
spec_schema_version: 1
id: KEY-07D
kind: primitive
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
parent_id: KEY-07
function: matte_guided_gray
execution: tile
proposed_operation_keys:
  - keying.matte_guided_gray
---

# KEY-07D: Gray-guided matte refinement

Inherit the [shared contract](COM_common_contract.md) and [KEY contract](KEY_keying_contract.md). The proposed basic tile operator is `keying.matte_guided_gray`.

## Interface

`matte: T[H,W]`, `guide: T[H,W]`, `epsilon: T[1]` -> `matte: T[H,W]`.

Matte in [0,1]; finite guide in explicit scalar units. Required radius:Int64 0..4096. epsilon>0 in squared guide units.

Port order, tensor members, static selectors and output representation follow COM. Shapes and dtypes are inferred from the stated signatures before payload evaluation.

### Static parameters

`radius:Int64` is required and has the declared caller domain 0..4096.

`max_work:Int64=9223372036854775807` is the nonnegative per-invocation operator-work limit. Run work admission and cancellation remain effective in addition to this limit.

## Mathematics

In each cropped square window compute means mg,mp, population variance vg and covariance cov(g,p). Set a=cov/(vg+epsilon), b=mp-a*mg. At p average a,b over its cropped square window and return clamp(mean(a)*guide_p+mean(b),0,1). Radius zero copies matte.

### Current implementation profile

The implementation evaluates the two cropped-box stages in Float64 and a fixed global order. For at least two image rows and epsilon in [1e-250,1e250], row rings stream the centered input moments and coefficient windows across the declared radius domain; other positive epsilon values use the centered fallback. Large-radius seeds and clipped borders use bounded cancellation checkpoints. The AArch64 Float64 path fuses a single incoming moment row with coefficient evaluation when the row has a horizontal interior, while retaining the original add-then-subtract rounding order. A numerically suspect coefficient abandons the whole-image stream. For radius at most 32 and sufficient declared scratch, the implementation retries fixed global 256-square output blocks, first attempting full-width 256-row bands with packed source rows when those larger patches also fit. A successful band skips its square blocks. Wide bands stream into the final output while saving and restoring the already completed leading halo; subsequent bands overwrite their following halo. Every attempt includes two radii of input halo and publishes only its center. Failed square blocks are bisected to at most 16 samples per dimension before entering the centered fallback. Attempted patch area is limited to twice the image area; exceeding it recomputes the complete result with the centered fallback. Other cases use that fallback directly. Partitioning is independent of the requested output tiles. In that fallback, flagged coefficients use compensated local centered statistics, and affected output windows use local centered predictions. Windows whose guide differences, moments, or regularized denominator overflow, or whose variance approaches underflow, additionally normalize the guide by a power of two, scale epsilon in squared guide units, and evaluate predictions with that same local scaling. Radius zero copies source bits. These evaluation choices preserve the stated guided-filter equation.

## Behavior

H(2*radius) support from the two window stages. Means and coefficients are Float64 numerical intermediates; the strict oracle evaluates the complete mathematical expression before final output rounding.

## Acceptance

Radius zero copies. A constant matte remains constant within floating tolerance. Use a nonuniform guide to check edge-aware behavior.

Use the common native-versus-strict comparison rules for finite arithmetic, and exact checks for explicit copies and discrete outputs. Compare identical coordinates across whole-image, nonzero ROI and tile requests. Mathematical sources are listed in [research sources](../research-sources.md).
