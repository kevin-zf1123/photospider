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

For each requested output pixel, the same continuation first requests the map pixel with Control, Validation and Descriptor roles (14), alongside the source descriptor. It validates both map coordinates as finite and within `[-2^40,2^40]`, computes the four boundary-mapped taps, then requests source Data for present full RGBA taps together with Validation and Descriptor roles (13). If `constant` places every tap outside the source, the source descriptor remains in the relation but no source payload is requested. Map Control and source Data relations cover only the selected map pixel and taps; typed Validation is recorded separately over `close_samples`.

Taps remain in top-left, top-right, bottom-left, bottom-right order. Every in-domain tap is read as a complete RGBA pixel, including a tap whose weight is zero; editing that source tap can therefore mark the output potentially dirty even when its weight is zero and the output bits remain unchanged. The map Control witness independently tracks edits to the selected map pixel. Addresses are compactly mapped from output pixels to source pixels, so a sparse request does not flatten or enumerate a huge source image. The four products are accumulated with sequential Float64 additions in tap order and converted once to Float32. The Result writer closes requested channel samples to complete RGBA pixels, preserves global frame/layer and spatial coordinates, and publishes through an all-or-nothing Result transaction.

The program charges its state, tensor windows, workspace, output and work through Root limits. Invalid map coordinates return `OperationFailed`; malformed image/map metadata returns `TypeMismatch`; an unknown boundary returns `InvalidArgument`. Typed image source validation can return `InvalidArgument` with `FailureReason::InvalidDomain` and its source `input_id`. Cancellation and resource exhaustion retain their status codes. A failed execution returns no partially published output.

The Result writer uses affine backing for a requested output region when full spatial sample-byte geometry cannot be represented. Thus a sparse one-pixel request from a map with `Ho=Wo=2^40` writes four output samples under Root limits without allocating the full logical output domain; a full-domain write still has to fit the available Root budget. The same sparse mapping keeps the huge source case below from being enumerated.

`test_dependency_sampling` covers the five boundaries, map frame/layer broadcasting, a three-pixel identity map, and a sparse read from a 2^40-by-2^40 broadcast source backed by 16 payload bytes. It also checks that a zero-weight tap remains dirty evidence, constant-boundary taps request no source payload, Empty skips a nonfinite map, and the 10-by-10 fixture adds less than 1 MiB of live payload between the before/after measurements. That fixture delta is not a context payload limit, peak-memory bound or RSS claim. With source `atomic_trailing_axes=2`, Validation closes over the complete source row (the fixture has `H=1,W=3`), while Data remains limited to selected taps. A nonfinite RGB component in a zero-weight typed tap fails source validation with `InvalidArgument`; a nonfinite map coordinate fails in the operation with `OperationFailed` first. Four caller rounding modes produce the same tie result and restore the caller mode. The test also checks active cancellation after work admission releases payload back to baseline, then reads value 7 from a retained Result after context retirement and verifies the two-source association.

Run the focused repository test with:

```sh
cmake --build build/kernel-dev --target test_dependency_sampling -j8
ctest --test-dir build/kernel-dev -R '^test_dependency_sampling$' --output-on-failure
```

Installed consumers exercise the public Result workflows for STMap and radius, including demand replacement and generation behavior. Reproduce the package checks with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build --target photospider_dependency_workflow photospider_sampling_consumer -j8
ctest --test-dir build/kernel-dev/consumer-build -R '^(installed_dependency_sampling|installed_dependency_radius_workflow)$' --output-on-failure
```

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

Each poll reserves 32,768 bytes of Root scratch and charges Run work. The continuation keeps at most 64 candidate indices in an inline array and processes the requested output footprint sample by sample. Each sum starts at positive zero and uses a Float64 left fold in increasing source index order. The operation establishes nearest rounding and gradual underflow for each poll, then restores the caller's floating-point environment. Nonfinite included source values, intermediate sum overflow and invalid observed radius values fail with `OperationFailed`; typed Result validation can report `InvalidArgument` with `FailureReason::InvalidDomain` and the source `input_id`. Cancellation and resource exhaustion retain their status codes.

Empty demand performs static metadata specialization and returns an empty Result without requesting sample payload. For nonempty demand, the builder publishes each point privately and seals once after all requested samples are complete. Failure or cancellation before seal returns no partial Result. Output coverage follows the requested footprint `Q`; a full output is produced when the caller requests the full domain.

## Execution state and errors

```text
STMap Empty -> static metadata/parameter checks -> empty Result, no payload Need
STMap output Q -> close to full RGBA pixel
  Need(map Control|Validation|Descriptor, source Descriptor)
  -> validate map -> map four taps
  -> Need(source Data|Validation|Descriptor, or Descriptor only for constant outside)
  -> sample and publish pixel -> next pixel or seal Result

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

`test_dependency_sampling` covers seeded gather/scatter cases, exact Data/Control/Validation supports, radius edits that change support without changing output bits, typed and opaque facets, included nonfinite values, ordered sums, Empty demand, cancellation and Root rollback. A broadcast view with `N=2^40` and eight payload bytes per input supports a gather of the last sample without scanning distant Control values. The [G4 `--radius-only` workflow](../../examples/g4_workflow/README.md) covers Result-based dynamic edits, demand replacement and retained-output behavior. STMap coverage is described above.

## Generic tuple observations and numeric diagnostics

C++ `OperationOutputTraits::atomic_trailing_axes` groups complete trailing axes into one atomic observation for CPU staged Atomic outputs. Zero keeps scalar observations. For generic shape `{N,C}`, value 1 gives observation shape `{N}`; for an axis tuple `{3}`, value 1 gives one observation with shape `{1}`. The host closes a partial sample request over its complete tuple for computation, validation and certificates, then returns the requested sample intersection. The Result executor closes STMap output observations to complete RGBA pixels. Grouping is preserved in compiler edge metadata, structured bridges and joint compatibility checks, and is included in operation identity. The Result operation ABI 2 `ps_result_tensor_spec_v2` descriptor also exposes `atomic_trailing_axes` alongside the batch-rank and layout fields.

Structured Result callbacks report bounded `NumericDiagnostics` through `ResultProgramPhase::report_numeric`. Reports identify the actual CPU profile and implementation, evaluated values, strict fallbacks and bounded fallback reasons. The host admits the per-output/per-backend `OperationTiming` record against Root resources before dispatching a Result callback. Each poll's report is merged once, including a poll that returns Need or fails. For a failed callback, the host latches the first cause before the failure-report merge, so a merge failure does not replace that cause. If `BackendUnavailable` qualifies for CPU fallback, the host records the failed GPU attempt before restarting on CPU.

The facilities example also drives a singleton upstream Result callback to its configured Host limit after reporting one evaluated value, then returns `OperationFailed` with `ShortIo`, Io origin and Group scope. Its C2 caller has already reported two values, so the result aggregates three with joint grouping enabled or disabled while preserving the upstream failure identity. The timing record was admitted before callback dispatch, so recording these counters does not need to extend its Root-owned container after the callback uses the remaining Host capacity.

`NumericDiagnostics::evaluated_values` counts reported arithmetic attempts, including work performed before a later failure. `OperationTiming::computed_elements` is separate: for Result publication it counts newly published field rows and tensor samples. A later Result revision counts only rows and tensor coverage added since the prior revision, and cache-hit publication adds no computed elements. The counter saturates at `UINT64_MAX` and sets `computed_elements_saturated` when the logical count exceeds that range or the sample cardinality is unrepresentable; an exact count of `UINT64_MAX` leaves the flag clear. Diagnostic saturation does not invalidate an otherwise legal Result. These diagnostics do not change semantic identity or establish an accuracy bound. See the manual [numeric workflow](../../examples/numeric_workflow/README.md).

Structured execution merges reports from every executed observation under its producer output and backend. Cache hits and unrequested observations contribute no arithmetic counts.

## Limitations and caller handling

Use `image.stmap` with canonical `photospider.image` source Results and a generic Float64 map Result. All three registered keys use Result inputs and outputs. `ValueFragments` remains a typed-backing helper for atlas preparation and materialization; it is not a separate workflow or dependency-execution entry point. For pure, cacheable Result programs, the completed Result cache can reuse a sealed output across frozen or generation identities when its exact tensor footprint `Q`, typed transitive source proof and replayed Need-ready facts match the current bindings. A cache hit creates a new ObjectId and current association while sharing the cached physical backing; repeated requests within one frozen execution still use weak same-ID producer sharing. The cache requires an exact `Q` match, and rebuilding current dependency evidence can rerun an evicted Whole ancestor. Incomplete source owners, observed backend fallback and `photospider.path_set` are excluded; optional cache work or Root capacity can make a lookup miss or retention be skipped. See [Structured Results and tensor slots](Global-Results.md) for the cache ownership and proof contract. Radius work or output admission failures return `ResourceExhausted`; typed source validation follows the Result input-validation contract, invalid numeric observations return `OperationFailed`, and cancellation propagates from the execution host.
