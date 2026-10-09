# Dependency sampling operations

The default registry runs `image.stmap`, `numeric.radius_gather`, and `numeric.radius_scatter` through staged Result programs. STMap accepts a canonical image Result and a generic coordinate-map Result. Radius gather/scatter accept rank-one Float64 data and Int64 radius Results and publish generic Float64 tensor Results.

STMap records exact Data and Control supports for each output pixel, with Validation closure recorded independently. Its image output closes a requested channel footprint to the complete RGBA pixel before execution, so Result coverage and dirty mapping use pixel observations. Radius gather and scatter process requested output samples through one Result continuation, keep Data or Control support exact per output, and record `close_samples` Validation separately.

## Scope and execution paths

`OperationRegistry` registers `image.stmap`, `numeric.radius_gather`, and `numeric.radius_scatter` as Result operations. Each definition supplies `start_result`, which creates a `ResultContinuation`, and `specialize_metadata`, which checks bound Result schemas and parameters and derives the output schema before execution. A continuation poll receives a `ResultProgramPhase`; the coordinator fulfills its Result object, tensor, and I/O Needs, and the callback can read only the tensor samples authorized by those Needs. `ResultProgramPhase::report_numeric` reports bounded `NumericDiagnostics`. The C plugin surface is separately defined by Result operation ABI 2 and also uses Result ports; it does not expose a Value dependency executor.

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

Each step in these formulas is one Float64 operation rounded to nearest: `f_x` is evaluated as `(u-0.5)-l`, each weight is the Float64 product of two Float64 factors, and each product `w*s` is rounded before it is added. The weights are therefore Float64 values, not exact rational weights. With `u=2^-56`, `v=0.5`, a 1-by-2 source whose red values are `[1,-1]` and `wrap`, `u-0.5` rounds to `-0.5`, both horizontal weights become `0.5`, and the red output is `+0` (Float32 bits `0x00000000`). The exact rational expression would give `2^-55`. `image.stmap` keeps the Float64 result, and the bitwise oracle in `test_dependency_sampling.cpp` checks it under all four caller rounding modes.

### Execution phases

A nonempty request completes in three continuation polls. The program closes the requested output footprint `Q` to complete RGBA pixels before the first Need; the closed footprint `P` is the set of output pixels that the program computes and publishes.

1. The first poll requests the source descriptor (role 8, Descriptor) and the map samples under every canonical box of `P`, with Control, Validation and Descriptor roles (14). A broadcast map drops the frame/layer axes from these map boxes. Map samples outside `P` are not requested, so a nonfinite map value at an unrequested pixel is never read.
2. The second poll reads the granted map coordinates row by row, through `rectangle_run()` when the window exposes both components on the sample axis and otherwise through per-component `row_run()` calls, so planar, tiled and signed-stride maps are accepted. The storage depends on the request; see [Map coordinate storage](#map-coordinate-storage). It rejects a coordinate that is nonfinite or outside `[-2^40,2^40]`, computes the four boundary-mapped taps for each pixel and builds the Gather relation described below. It then projects that relation over `P` to obtain the exact source Data footprint and requests it with Data, Validation and Descriptor roles (13). When `constant` places every tap of every requested pixel outside the source, the request is Descriptor only (8) and reads no source payload.
3. The third poll copies each granted source pixel once into a Root-accounted cache of `S` RGBA Float32 values, where `S` is the number of distinct source Data pixels. It then publishes all of `P` in one call to the Footprint overload of `ResultBuilder::publish_tensor_kernel` and seals the Result.

Empty demand completes in the first poll: the program seals an empty Result with its descriptor relation and requests no map or source samples. `OperationTiming::invocation_count` therefore reports three polls for a nonempty request and one for Empty demand. CPU tile callbacks of the publication stage below are reported separately through `cpu_tile_callback_count`.

### Dependency evidence

The published tensor relation is a union of a constant number of relation nodes, independent of `P`:

| Node | Support |
| --- | --- |
| `ResultRelation::gather` over `P` | For each output pixel, source Data (role 1) for each present tap as a complete RGBA pixel. Indexed axes are source height and width; frame and layer follow the output frame and layer. Validation (role 4) is closed over `max(1, atomic_trailing_axes)` trailing source axes without widening Data. |
| `ResultRelation::mapped` over the map | For each output pixel, the map pixel at the same height and width, with the output frame/layer when the map is batched. With map `atomic_trailing_axes <= 1` it records Control and Validation (role 6); with a larger grouping it records Control (role 2) and a separate Validation node (role 4) closed over those trailing map axes. |
| Descriptor relation | Descriptor (role 8) for both input ports, bound with `bind_descriptor_relation`. |

The map node is built over the bounding rectangle of `P`; publication restricts the union to the exact footprint `P`, so an unrequested map pixel inside that rectangle carries no Control edge. The Gather table stores one row per output pixel with four optional taps: a presence bit and the source y/x coordinates of each tap, bit-packed at the width of the source height and width axes. It is one immutable table, not one relation node per pixel. A tap outside the source under `constant` is absent and contributes no support.

Taps remain in top-left, top-right, bottom-left, bottom-right order. Every in-domain tap is read as a complete RGBA pixel, including a tap whose weight is zero; editing that source tap can therefore mark the output potentially dirty even when its weight is zero and the output bits remain unchanged. A nonfinite component in such a tap still fails typed source validation. The map Control witness independently tracks edits to the selected map pixel. Addresses are compactly mapped from output pixels to source pixels, so a sparse request does not flatten or enumerate a huge source image.

### Publication and parallel arithmetic

The publication callback runs on the coordinator thread. It walks each write window in row order, charges work and creates spans of at most 256 pixels; each span records the global output coordinate, a pointer and byte stride for each of the two map components, and one raw output pointer and byte stride per channel. A span does not cross a map row. Each output pixel evaluates the formula above from the map values and the source cache. A source tap is located in the cache by a binary search over the canonical source boxes when there is more than one box.

When `P >= 65,536` and the host supplies the CPU staged tile service (`OperationTraits::cpu_staged_tiles`), the coordinator collects all spans first and submits one tile stage over the span index. The tile size is `ceil(spans/64)`, so the stage has at most 64 tile callbacks regardless of the worker count, and the stage requests `min(4, maximum_workers)` workers. A tile callback reads only the immutable map values and source cache and writes its own disjoint output spans; it checks cancellation before each span. Map and source Needs, window services, allocation, work accounting and publication remain on the coordinator. All tile callbacks complete before the publication callback returns. Smaller requests, or hosts without the service, evaluate each span inline on the coordinator. Output bytes, Root work and tile count are identical for one and four workers.

On AArch64 the program evaluates the four channels with NEON: it widens each tap's RGBA values to two Float64 lane pairs, multiplies each pair by the tap weight and adds it to the accumulator in tap order, then narrows once to Float32. Other targets use the equivalent scalar loop. Both paths start each channel from `+0`, perform a separate multiply and add, and round once to Float32. `dependency_sampling.cpp` is compiled with `-fno-fast-math -frounding-math -ffp-contract=off`, so the compiler does not contract the multiply and add into a fused operation or reassociate the sum. Each poll installs nearest rounding with gradual underflow and restores the caller's floating-point environment. Tile workers run under the CPU tile service's nearest-rounding environment described in [Parallel execution model](Parallel-Execution-Model.md#planar-staged-tile-service).

### Cost

Let `P` be the closed output pixel count, `S <= 4P` the distinct source Data pixel count, `B_s` and `B_o` the canonical box counts of the source Data footprint and of `P`, `R_o` the number of nonempty output rows, and `V` the number of scalar samples that typed Validation reads. `G` is the cost of the exact Footprint operations that normalize these sets. The STMap program performs

$$
T = O\!\left(P\,[1 + \log(B_s+1) + \chi \log(B_o+1)] + R_o \log(B_o+1) + S + V + G\right),
$$

where $\chi$ is 1 when the varying tap coordinates do not fit one 64-bit sort key and 0 otherwise. Map reading, tap generation, Gather table construction and interpolation are linear in `P`. Source caching is linear in `S`. Each tap lookup searches the source boxes, and each output row locates its box in `P`. For a dense rectangular request, `B_s` and `B_o` are small and `V` and `G` are linear in `P`, so the whole program is linear in `P`. A highly fragmented request or a fragmented source footprint keeps the logarithmic terms, and a large Validation closure costs at least `V`. Gather normalization marks tap origins in a bitmap when their bounding box is small enough, and otherwise sorts them with a stable byte radix sort that skips byte positions which do not vary. A dense request takes the bitmap path in `O(P + A/64)`, where `A <= 32P` is the bounding-box area. On the radix path, the sort has at most eight passes when the varying source coordinates fit one 64-bit key; otherwise each varying source axis is a separate 64-bit key, and each read of an output-mapped frame or layer coordinate searches the output boxes, which is the $\chi$ term. See [Dependency data](Dependency-Data.md#structured-result-tensor-dependencies) for Gather queries.

The Gather table stores `4 * (ceil(log2 Hs) + ceil(log2 Ws))` coordinate bits and 4 presence bits per output pixel, rounded up to whole 64-bit words for the table; a 4096-by-4096 source needs 12.5 bytes per pixel. The published relation keeps this table for its lifetime. During the request the program also holds the map coordinates (see below) and 16 bytes per cached source pixel. The dense bitmap holds `ceil(A/64)` words while normalization runs. Radix keys and scratch (8 bytes each per present tap), spans, windows, Footprint normalization scratch and output payload are additional Root charges.

### Map coordinate storage

A request uses map rows when it has at least 65,536 pixels, its closed footprint is one box whose rows are at least 256 pixels wide, the map capability comes from a single Result, and the requested map region covers the complete map domain. The output does not need to cover its complete domain: an unbatched map broadcast over a subset of frames and layers still qualifies. For each output row the program keeps one directory entry. If the map window exposes the complete row as one affine or planar run, the entry retains that owning read window and its raw pointers and signed strides, and copies nothing. Otherwise it copies the row into a Root-allocated buffer of 16 bytes per pixel. Raw and copied rows can coexist in one request.

Other requests copy all map coordinates into a Root-accounted `[P][2]` Float64 array. In both cases the program releases the windows and buffers when it is destroyed after publication; the published Gather relation keeps only tap coordinates and presence bits. A retained read window keeps the map backing it reads alive while the request runs, so the saving relative to the array depends on the map's physical layout.

### Errors and resources

The program charges its state, tensor windows, workspace, output and work through Root limits. Invalid map coordinates return `OperationFailed`; malformed image/map metadata returns `TypeMismatch`; an unknown boundary returns `InvalidArgument`. Typed image source validation can return `InvalidArgument` with `FailureReason::InvalidDomain` and its source `input_id`. Cancellation and resource exhaustion retain their status codes; a tile callback reports cancellation, resource exhaustion and other failures to the coordinator, which returns the first recorded status. A failed execution returns no partially published output.

A closed footprint with one canonical box uses the Region publication path: the Result writer uses planar backing when the full spatial sample-byte geometry is representable and affine backing for the requested region otherwise. Thus a sparse one-pixel request from a map with `Ho=Wo=2^40` writes four output samples under Root limits without allocating the full logical output domain. A footprint with several boxes uses one packed affine allocation covering exactly those boxes. A full-domain write still has to fit the available Root budget, and the same sparse mapping keeps the huge source case below from being enumerated.

Root capacities limit the request size. The Gather table, Footprint metadata and the source cache are charged as Host and Metadata capacity. With the default capacities, a large full request returns `ResourceExhausted` once these structures exceed the Metadata capacity; such requests need explicitly larger Metadata, Host and Shared capacities in the execution resource configuration. Publication also keeps its local Footprint limits of 65,536 boxes and 1,048,576 work units for the published set.

### Tests

`tests/integration/test_dependency_sampling.cpp` covers the five boundaries, map frame/layer broadcasting, a three-pixel identity map, and a sparse read from a 2^40-by-2^40 broadcast source backed by 16 payload bytes. It also checks that a zero-weight tap remains dirty evidence, constant-boundary taps request no source payload, and Empty skips a nonfinite map. With source `atomic_trailing_axes=2`, Validation closes over the complete source row while Data remains limited to selected taps. A nonfinite RGB component in a zero-weight typed tap fails source validation with `InvalidArgument`; a nonfinite map coordinate fails in the operation with `OperationFailed` first. A retained Result still reads value 7 after context retirement and keeps its two-source association.

`stmap_bitwise_oracle` compares every output channel bit pattern with an independent scalar reference over 2-by-3, 1-by-1 and 1-by-2 sources, all five boundaries, coordinates at `+/-2^40`, subnormal, signed-zero and `2^80` source values, and planning tiles of 1 by 2. Under four caller rounding modes it checks the tap-order fold of `2^100` and `-2^100` and the `2^-56` counterexample above, and checks that the caller mode is restored. `stmap_sparse_dependencies` checks exact per-pixel Data dirty mapping, projection of one output channel to its two source pixels, holes in a sparse request, a NaN map value outside the request, and separate Data and Validation closure for grouped source and map tensors. `stmap_batches_and_strides` covers broadcast and batched maps over a 2-by-2 frame/layer source and nonspatial and spatial map backing with signed strides. `typed_validation_work_batches` checks the 256-sample validation charge batches described in [Managed resources](Managed-Resources.md#execution--state). `stmap_map_rows` runs 65,536-pixel requests over negative-stride affine, negative-stride planar, 32-by-64 tiled and partly split affine maps, so raw and copied rows occur in one request, and compares all pixels bitwise across layouts and sampled pixels with the scalar oracle. It also checks exact Data projection and inverse projection across the raw/copied boundary, broadcast and batched maps over two frames and two layers, caller rounding restoration, and that a relation retained after its Result is released holds no Payload and less than 256 KiB of Metadata. `stmap_cpu_tiles` runs a 256-by-256 request with one and four workers under all five boundaries and an upward caller rounding mode, and requires byte-identical output, equal work, equal tile count of at most 64, the oracle values and exact source support.

The scaling case runs 8-by-8 through 64-by-64 maps with explicitly raised dependency work limits. Every map coordinate is 0.5, so under `clamp` each output pixel reads source pixels 0 and 1 of a three-pixel row. The test checks every output channel and exact source support. Editing source pixel 1 makes the whole output potentially dirty, while editing the unused pixel 2 leaves it clean. While the Result is held, live Payload grows by less than 1 MiB; after release it returns to the baseline. Root work stays below `1600 * P`, and each fourfold increase of `P` increases work by less than five times. The sparse scaling case requests a checkerboard of single pixels on 16-by-16 through 64-by-64 maps; it requires three polls, Root work below `15000` per requested pixel and less than a sixfold increase per fourfold size step. These bounds are regression checks for these workloads only, and the 1 MiB delta is not a context payload limit, peak-memory bound or RSS claim.

Cancellation is tested at two points on a 50-by-50 map. The first holds the callback after its body finishes, cancels the Run, and requires `Cancelled` with Payload back at the baseline. The second pauses the callback thread at a positive phase work charge inside one poll and cancels there. That work charge must return `Cancelled`, the Run must report `Cancelled`, and Payload must return to the baseline. Cancellation inside a tile worker is covered by the generic CPU tile service tests, not by an STMap-specific fault injection.

## Result radius gather and scatter

`numeric.radius_gather(source, radius)` and `numeric.radius_scatter(source, radius)` take two Result inputs. Each Result contains exactly one tensor and no fields; both tensors are unbatched rank one with the same length `N`, where `1 <= N <= 2^40`. The source uses Float64 and the radius uses Int64. Structurally valid schema ids, versions, member keys and facets accepted by their Result input contracts remain valid. Both operations return port `value` as a `photospider.tensor` v1/member `samples` Result with Float64 shape `[N]` and empty facets.

For output index $o$, gather selects source indices using the radius at $o$; scatter selects them using each candidate $i$:

$$
G(o) = \{i \in [0,N) : |i-o| \le r_o\}, \qquad S(o) = \{i \in [0,N) : |i-o| \le r_i\}.
$$

The gather implementation turns $G(o)$ into a clipped half-open interval. With `output < size` and `radius <= 2^40`, it computes:

```cpp
cursor = output > radius ? output - radius : 0;
end = output + 1 + std::min(radius, size - output - 1);
```

For every requested output sample, the Result program requests that radius value as Control, Validation and Descriptor (role 14). It requests the other input's Descriptor (role 8) alongside it. Radius values outside `[0,2^40]` fail with `OperationFailed`. Gather then requests source values in the clipped interval as Data, Validation and Descriptor (role 13), in chunks of at most 64 samples. It accumulates in increasing source index order.

Scatter scans the complete radius tensor in Control chunks of at most 64 samples. It retains the full Control witness, including candidates excluded from the output, then requests Data only for matching source indices. A changed remote radius can add a source edge and dirty an output even when the current output bits are unchanged. The operation needs no inverse spatial index.

The dependency relation maps each output sample to its exact source Data or radius Control support. `close_samples` Validation is recorded independently with role 4. Descriptor relations for both inputs remain present even when a Data request is empty. This makes typed validation distinguishable from the mathematical support of the selected sum.

Each poll reserves 32,768 bytes of Root scratch. Work that the continuation charges through its phase debits Root. Need admission and dependency projection in the coordinator debit Root and also the separate Run-wide allowance `ExecutionOptions::maximum_dependency_work` (default 1,048,576), so the cumulative Root work of a Run is not comparable with that allowance. The continuation keeps at most 64 candidate indices in an inline array and processes the requested output footprint sample by sample. Each sum starts at positive zero and uses a Float64 left fold in increasing source index order. The operation establishes nearest rounding and gradual underflow for each poll, then restores the caller's floating-point environment. Nonfinite included source values, intermediate sum overflow and invalid observed radius values fail with `OperationFailed`; typed Result validation can report `InvalidArgument` with `FailureReason::InvalidDomain` and the source `input_id`. Cancellation and resource exhaustion retain their status codes.

Empty demand performs static metadata specialization and returns an empty Result without requesting sample payload. For nonempty demand, the builder publishes each point privately and seals once after all requested samples are complete. Failure or cancellation before seal returns no partial Result. Output coverage follows the requested footprint `Q`; a full output is produced when the caller requests the full domain.

## Execution state and errors

```text
STMap Empty -> static metadata/parameter checks -> empty Result, no payload Need
STMap output Q -> close to full RGBA pixels P
  poll 1: Need(map Control|Validation|Descriptor over P, source Descriptor)
  poll 2: read and validate map -> four taps per pixel -> Gather relation
          -> Need(source Data|Validation|Descriptor over projected taps,
                  or Descriptor only when every tap is outside under constant)
  poll 3: cache source pixels -> one publication of P
          -> P < 65,536 or no tile service: coordinator evaluates spans
          -> otherwise: <= 64 tile callbacks on <= 4 workers, then join
          -> commit P or roll back -> seal Result

Radius Empty -> static metadata checks -> empty Result, no payload Need
Radius output Q -> for each requested output index
  Need(radius Control|Validation|Descriptor, source Descriptor)
  gather -> Need(source Data|Validation|Descriptor chunks) -> ordered fold
  scatter -> full radius Control scan -> Need(source Data hits) -> ordered fold
  -> privately publish point -> seal once after Q
```

Radius state carries the requested-output cursor, at most 64 candidate indices, stage and Float64 accumulator. Each Result Need is validated against its input schema and authorized tensor coverage.

## Validation, errors and evidence

STMap and radius definitions use `OperationDefinition::specialize_metadata` for static Result validation. The registry supplies complete input Result metadata and parameters; the specializer validates the declared constraints and returns the inferred output schema before execution. It does not read tensor payload. `OperationRegistry::start_result` then validates the resolved `ResultProgramQuery` and invokes the definition factory to create its continuation. During execution, `ResultContinuation::poll` receives the current `ResultProgramPhase`, including the callback's authorized Result inputs and services; the coordinator fulfills Needs between polls.

`tests/integration/test_dependency_sampling.cpp` covers seeded gather/scatter cases, exact Data/Control/Validation supports, radius edits that change support without changing output bits, typed and opaque facets, included nonfinite values, ordered sums, Empty demand, cancellation and Root rollback. For a 100,000-sample scatter, cancellation is tested both after the callback body finishes and at a phase work charge inside the radius Control element loop; each case returns `Cancelled` and returns Payload to the baseline. A broadcast view with `N=2^40` and eight payload bytes per input supports a gather of the last sample without scanning distant Control values. Current end-to-end demand and dynamic-binding coverage lives in [`dependency_workflows/demand.cpp`](../../tests/integration/dependency_workflows/demand.cpp), [`dynamic.cpp`](../../tests/integration/dependency_workflows/dynamic.cpp), and [`dependency_workflow_fixture.hpp`](../../tests/support/dependency_workflow_fixture.hpp). The independent STMap mapping, boundary and numeric oracle is in [`test_dependency_sampling.cpp`](../../tests/integration/test_dependency_sampling.cpp). These are the current behavior entry points.

## Generic tuple observations and numeric diagnostics

C++ `OperationOutputTraits::atomic_trailing_axes` groups complete trailing axes into one atomic observation for CPU staged Atomic outputs. Zero keeps scalar observations. For generic shape `{N,C}`, value 1 gives observation shape `{N}`; for an axis tuple `{3}`, value 1 gives one observation with shape `{1}`. The host closes a partial sample request over its complete tuple for computation, validation and certificates, then returns the requested sample intersection. The Result executor closes STMap output observations to complete RGBA pixels. Grouping is preserved in compiler edge metadata, structured bridges and joint compatibility checks, and is included in operation identity. The Result operation ABI 2 `ps_result_tensor_spec_v2` descriptor also exposes `atomic_trailing_axes` alongside the batch-rank and layout fields.

Structured Result callbacks report bounded `NumericDiagnostics` through `ResultProgramPhase::report_numeric`. Reports identify the actual CPU profile and implementation, evaluated values, strict fallbacks and bounded fallback reasons. The host admits the per-output/per-backend `OperationTiming` record against Root resources before dispatching a Result callback. Each poll's report is merged once, including a poll that returns Need or fails. For a failed callback, the host latches the first cause before the failure-report merge, so a merge failure does not replace that cause. If `BackendUnavailable` qualifies for CPU fallback, the host records the failed GPU attempt before restarting on CPU.

The facilities example also drives a singleton upstream Result callback to its configured Host limit after reporting one evaluated value, then returns `OperationFailed` with `ShortIo`, Io origin and Group scope. Its C2 caller has already reported two values, so the result aggregates three with joint grouping enabled or disabled while preserving the upstream failure identity. The timing record was admitted before callback dispatch, so recording these counters does not need to extend its Root-owned container after the callback uses the remaining Host capacity.

`NumericDiagnostics::evaluated_values` counts reported arithmetic attempts, including work performed before a later failure. `OperationTiming::computed_elements` is separate: for Result publication it counts newly published field rows and tensor samples. A later Result revision counts only rows and tensor coverage added since the prior revision, and cache-hit publication adds no computed elements. The counter saturates at `UINT64_MAX` and sets `computed_elements_saturated` when the logical count exceeds that range or the sample cardinality is unrepresentable; an exact count of `UINT64_MAX` leaves the flag clear. Diagnostic saturation does not invalidate an otherwise legal Result. These diagnostics do not change semantic identity or establish an accuracy bound. See the manual [numeric workflow](../../examples/numeric_workflow/README.md).

Structured execution merges reports from every executed observation under its producer output and backend. Cache hits and unrequested observations contribute no arithmetic counts.

## Limitations and caller handling

Use `image.stmap` with canonical `photospider.image` source Results and a generic Float64 map Result. All three registered keys use Result inputs and outputs. `ValueFragments` remains a typed-backing helper for atlas preparation and materialization; it is not a separate workflow or dependency-execution entry point. For pure, cacheable Result programs, the completed Result cache can reuse a sealed output across frozen or generation identities when its exact tensor footprint `Q`, typed transitive source proof and replayed Need-ready facts match the current bindings. A cache hit creates a new ObjectId and current association while sharing the cached physical backing; repeated requests within one frozen execution still use weak same-ID producer sharing. The cache requires an exact `Q` match, and rebuilding current dependency evidence can rerun an evicted Whole ancestor. Incomplete source owners, observed backend fallback and `photospider.path_set` are excluded; optional cache work or Root capacity can make a lookup miss or retention be skipped. See [Structured Results and tensor slots](Global-Results.md) for the cache ownership and proof contract. Radius work or output admission failures return `ResourceExhausted`; typed source validation follows the Result input-validation contract, invalid numeric observations return `OperationFailed`, and cancellation propagates from the execution host.
