# Dependency sampling operations

## Scope and execution paths

Operations own algorithms and sample interpretation. Their pure `OperationDefinition::lower` callback emits fixed task reads, writes and scratch before execution. The compiler validates the physical program; preparation binds immutable kernels and Run admission supplies allocation owners. A data-dependent gather declares every tile its input data could address, then selects samples within those fixed reads during invocation. Runtime branch selection uses explicit IR conditional jumps.

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

### Fixed tasks and parallel arithmetic

Each selected output tile emits one `stmap-bilinear-fixed-v1` CPU task. Source reads cover the complete H/W domain of the selected output batch coordinates. Map reads cover the same output Y/X region and both UV components; an unbatched map omits the batch prefix. Each region closes on its input's own TileGrid, including maps whose UV components occupy different tiles. Dynamic UV values select samples within these fixed reads. Every present tap, including a zero-weight tap, is read; constant-boundary absent taps contribute transparent zero.

The executor schedules independent output tiles and preallocates their dense RGBA storage. The operation uses fixed local arrays, declares zero scratch and performs no nested worker scheduling. Every invocation establishes nearest rounding with gradual underflow and restores the caller's floating environment. Its arithmetic preserves the top-left, top-right, bottom-left, bottom-right accumulation order and separate binary64 rounding steps.

### Views and ownership

Kernel preparation verifies exact sorted read slots, dtype, complete regions, the single output tile, empty task parameters and zero scratch. Invocation borrows immutable affine input views, including signed/zero strides and unaligned addresses, through completion. Input views need not share one allocation. DenseHost bindings use the compiler's explicit import COPY tasks. Successful output Results own independent dense tile allocations and can outlive the Run and context.

### Errors and resources

Lowering bounds source tile enumeration and cumulative read references before expanding each region. The operation builder rejects aggregate record/reference exhaustion before appending entries; final validation also checks derived graph edges. Source directories scale with the statically reachable spatial domain, while output allocation scales with selected tiles.

Before converting UV to integer addresses, each pixel checks that both coordinates are finite and within +/-2^40. Invalid used coordinates return `OperationFailed`; coordinates outside the selected output pixels are not scanned. Incompatible structure returns `TypeMismatch`, and an invalid boundary parameter returns `InvalidArgument`. Cancellation is polled before each pixel and returns without publishing that task's output. Resource failures retain their category. Final RET publication and observer ownership follow the fixed executor's ordered-retirement contract.

The independent reference in `oracle/ops/transform/geo06/reference.hpp` defines boundary mapping and straight-alpha resampling with explicit rounding after every arithmetic operation. Temporary public/direct checks compare raw output bits, including transparent colors, partial constant boundaries and the rounding witness above. These checks do not establish GPU execution or compatibility with normalized third-party STMap formats.
