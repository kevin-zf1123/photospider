# Dependency sampling operations

## Scope and execution paths

Operations own algorithms and sample interpretation. The kernel schedules and authorizes member tiles, freezes actual Need evidence and commits immutable outputs. A continuation may read control payload first and request source tiles in a later poll. No data/control role or sample-level relation is encoded in the kernel protocol.

## STMap Result contract

`image.stmap(source, map, boundary)` accepts a single-Tensor Float32 HWC RGBA source, with optional explicit batch axes, and a single-Tensor Float64 map with cell shape `[Ho,Wo,2]`. The source requires explicit H/W/channel axes 0/1/2 and one atomic trailing axis. The map is checked for dtype, cell shape and batch compatibility; its spatial-layout and atomic-axis metadata are not required to match the source. Input schema ids, versions and member keys may be any structurally valid values. Map batch axes are empty (broadcast over source batches) or exactly match the source. Source H/W are at most 2^40. Output `value` uses `photospider.image` v1/member `pixels`, source batch axes and Float32 `[Ho,Wo,4]` cells.

UV uses absolute source pixel-center coordinates. Bilinear is the fixed filter. The required String `boundary` has no default:

| Boundary | Integer tap rule for an axis of length N |
| --- | --- |
| constant | Outside taps contribute transparent zero without source reads. |
| clamp | Clamp to [0,N-1]. |
| wrap | Nonnegative remainder modulo N. |
| reflect | Period 2N, repeated endpoints. |
| mirror | Period 2N-2, unrepeated endpoints; N=1 selects zero. |

All nonconstant modes select zero for N=1. Rules use the complete source domain, not fragment-local coordinates. Preparation checks structure, batch compatibility, axis bounds and boundary selection without reading samples.

For map coordinates u,v, let l=floor(u-0.5), t=floor(v-0.5), fx=(u-0.5)-l, fy=(v-0.5)-t. Four bilinear weights are `(dx ? fx : 1-fx)*(dy ? fy : 1-fy)`. The operation interprets source RGB as straight and computes alpha-aware resampling:

$$ A=\sum_i w_i A_i,\qquad P_c=\sum_i w_i A_i RGB_{i,c},\qquad
RGB'_c=\begin{cases}P_c/A&A\ne0\\+0&A=0.\end{cases} $$

Arithmetic uses Float64 accumulation and narrows to Float32 output. A zero weighted alpha skips the RGB multiplication, so transparent hidden RGB contributes no color. The operation does not decode transfer, perform gamma/ICC conversion or clip gamut. Explicit color conversion belongs upstream/downstream. No default finite-premultiplied or straight-compliance validator is bound. UV must be finite and within the operation's absolute 2^40 coordinate bound; source pixel interpretation remains this algorithm's responsibility.

A rounding witness with opaque alpha uses u=2^-56, v=0.5, wrap, and a 1-by-2 source with red values [1,-1]. Float64 `(u-0.5)` rounds to -0.5, both horizontal weights are 0.5, and the Float32 red output is +0 (bits 0x00000000), while exact rational weights would give 2^-55. Preserve this independent numerical reference when checking all four caller rounding modes; temporary check programs follow the test retention policy.

### Publication and parallel arithmetic

For each requested output member tile, the program requests corresponding map tiles, reads UV and discovers source taps. Every existing tap, including a zero-weight tap, contributes to tile discovery. Source sample support is rounded to member tiles. Mathematical tap precision does not imply sample-level authorization or invalidation.

The coordinator prepares bounded pixel spans and writes through Result write windows. Large admitted work can use the CPU staged tile service; work-item geometry is separate from plan payload tiles. Tile workers read retained immutable inputs and write disjoint prepared spans. Allocation, Needs, work admission and publication stay with the coordinator, which joins workers before committing. Operations establish their numerical environment inside each executing callback; the kernel only saves and restores thread state.

### Map coordinate storage

Input windows preserve actual affine/fragmented mappings. A representable map row can retain its owner and stride; required row copies are explicit and charged. Neither full input coverage nor default tiled storage implies a contiguous canvas or an implicit full-array copy.

### Errors and resources

State, windows, workspace, output and work are Root-accounted. Invalid UV fails with OperationFailed; incompatible structure fails with TypeMismatch; an invalid boundary parameter fails with InvalidArgument. Cancellation and resource exhaustion keep their status categories. The tile transaction publishes no partial payload or incomplete evidence on failure.

## Generic tuple observations and numeric diagnostics

Ops SDK `NumericDiagnostics` reports arithmetic attempts and implementation-specific counters through bounded generic diagnostic services. The kernel transports and merges reports without assigning pixel semantics. `computed_elements` counts newly committed Tensor samples, not dynamic rows; cache hits add no computation. Saturating counters and timing observations do not establish numerical accuracy or native device execution.
