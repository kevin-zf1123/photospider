# Dependency sampling operations

## Scope and execution paths

The registry owns each immutable operation definition. Its continuation retains prepared metadata and resource owners, while each poll borrows the current phase and authorized input capabilities. Callbacks record consumed source ObjectIds in the output association and publish a Result.

## STMap Result contract

`image.stmap(source, map, boundary)` requires a source Result with schema `photospider.image` v1, one `pixels` tensor, no fields, canonical linear-premultiplied Float32 RGBA, and sample shape `[F,L,Hs,Ws,4]` with `batch_axes=[F,L]`. The map is a Result with exactly one tensor and no fields, any structurally valid schema id/version/member key, Float64 shape `[Ho,Wo,2]`, and empty facets. Its `batch_axes` are either empty, which broadcasts the same map over every source frame/layer, or equal to the source batch axes. The source H/W axes are at most 2^40. Output port `value` is a Result with canonical `photospider.image` v1/member `pixels`, Float32 shape `[F,L,Ho,Wo,4]`, and the source frame/layer extents.

The required String parameter `boundary` has no default, including for Empty requests:

| Value | Address rule for an integer tap `i` on an axis of length `N` |
| --- | --- |
| `constant` | Outside taps use transparent black and read no source pixel. |
| `clamp` | Clamp to `[0,N-1]`. |
| `wrap` | Nonnegative remainder modulo `N`. |
| `reflect` | Period `2N`, repeated endpoints: `p<N ? p : 2N-1-p`. |
| `mirror` | Period `2N-2`, unrepeated endpoints: `p<N ? p : 2N-2-p`. |

For `N=1`, every nonconstant mode selects coordinate zero. These mappings use global source coordinates, not fragment-local coordinates. Static preparation checks the canonical source metadata, map tensor shape/facets/batch compatibility, source axis bounds and boundary name. Empty demand performs those checks without requesting sample payload.

For map coordinates `u` and `v`, the operation selects the four neighbors from `l=floor(u-0.5)` and `t=floor(v-0.5)`. The fractional offsets, weights and output channel are:

$$
f_x = u-\tfrac12-l, \qquad f_y = v-\tfrac12-t, \qquad w_{dy,dx} = (dy ? f_y : 1-f_y)(dx ? f_x : 1-f_x),
$$
$$
y_c = \mathrm{Float32}\!\left(\sum_{dy=0}^{1}\sum_{dx=0}^{1} w_{dy,dx} \, s_{dy,dx,c}\right).
$$

Each step in these formulas is one Float64 operation rounded to nearest: `f_x` is evaluated as `(u-0.5)-l`, each weight is the Float64 product of two Float64 factors, and each product `w*s` is rounded before it is added. The weights are therefore Float64 values, not exact rational weights. With `u=2^-56`, `v=0.5`, a 1-by-2 source whose red values are `[1,-1]` and `wrap`, `u-0.5` rounds to `-0.5`, both horizontal weights become `0.5`, and the red output is `+0` (Float32 bits `0x00000000`). The exact rational expression would give `2^-55`. `image.stmap` keeps the Float64 result. Use this bitwise acceptance case under all
four caller rounding modes when changing the operation; create and remove the
temporary checker according to the testing policy.

### Publication and parallel arithmetic

The publication callback runs on the coordinator thread. It walks each write window in row order, charges work and creates spans of at most 256 pixels; each span records the global output coordinate, a pointer and byte stride for each of the two map components, and one raw output pointer and byte stride per channel. A span does not cross a map row. Each output pixel evaluates the formula above from the map values and the source cache.

When `P >= 65,536` and the host supplies the CPU staged tile service (`OperationTraits::cpu_staged_tiles`), the coordinator collects all spans first and submits one tile stage over the span index. The tile size is `ceil(spans/64)`, so the stage has at most 64 tile callbacks regardless of the worker count, and the stage requests `min(4, maximum_workers)` workers. A tile callback reads only the immutable map values and source cache and writes its own disjoint output spans; it checks cancellation before each span. Map and source Needs, window services, allocation, work accounting and publication remain on the coordinator. All tile callbacks complete before the publication callback returns. Smaller requests, or hosts without the service, evaluate each span inline on the coordinator. Output bytes, Root work and tile count are identical for one and four workers.

On AArch64 the program evaluates the four channels with NEON: it widens each tap's RGBA values to two Float64 lane pairs, multiplies each pair by the tap weight and adds it to the accumulator in tap order, then narrows once to Float32. Other targets use the equivalent scalar loop. Both paths start each channel from `+0`, perform a separate multiply and add, and round once to Float32. `stmap.cpp` is compiled with `-fno-fast-math -frounding-math -ffp-contract=off`, so the compiler does not contract the multiply and add into a fused operation or reassociate the sum. Each poll installs nearest rounding with gradual underflow and restores the caller's floating-point environment. Tile workers run under the CPU tile service's nearest-rounding environment described in [Parallel execution model](Parallel-Execution-Model.md#planar-staged-tile-service).

### Map coordinate storage

A request uses map rows when it has at least 65,536 pixels, its closed footprint is one box whose rows are at least 256 pixels wide, the map capability comes from a single Result, and the requested map region covers the complete map domain. The output does not need to cover its complete domain: an unbatched map broadcast over a subset of frames and layers still qualifies. For each output row the program keeps one directory entry. If the map window exposes the complete row as one affine or planar run, the entry retains that owning read window and its raw pointers and signed strides, and copies nothing. Otherwise it copies the row into a Root-allocated buffer of 16 bytes per pixel. Raw and copied rows can coexist in one request.

### Errors and resources

The program charges its state, tensor windows, workspace, output and work through Root limits. Invalid map coordinates return `OperationFailed`; malformed image/map metadata returns `TypeMismatch`; an unknown boundary returns `InvalidArgument`. Typed image source validation can return `InvalidArgument` with `FailureReason::InvalidDomain` and its source `input_id`. Cancellation and resource exhaustion retain their status codes; a tile callback reports cancellation, resource exhaustion and other failures to the coordinator, which returns the first recorded status. A failed execution returns no partially published output.

## Generic tuple observations and numeric diagnostics

Structured Result callbacks report bounded `NumericDiagnostics` through `ResultProgramPhase::report_numeric`. Reports identify the actual CPU profile and implementation, evaluated values, strict fallbacks and bounded fallback reasons. The host admits the per-output/per-backend `OperationTiming` record against Root resources before dispatching a Result callback. Each poll's report is merged once, including a poll that returns Need or fails. For a failed callback, the host latches the first cause before the failure-report merge, so a merge failure does not replace that cause. If `BackendUnavailable` qualifies for CPU fallback, the host records the failed GPU attempt before restarting on CPU.

`NumericDiagnostics::evaluated_values` counts reported arithmetic attempts, including work performed before a later failure. `OperationTiming::computed_elements` is separate: for Result publication it counts newly published field rows and tensor samples. A later Result revision counts only rows and tensor coverage added since the prior revision, and cache-hit publication adds no computed elements. The counter saturates at `UINT64_MAX` and sets `computed_elements_saturated` when the logical count exceeds that range or the sample cardinality is unrepresentable; an exact count of `UINT64_MAX` leaves the flag clear. Diagnostic saturation does not invalidate an otherwise legal Result. These diagnostics do not change semantic identity or establish an accuracy bound.
