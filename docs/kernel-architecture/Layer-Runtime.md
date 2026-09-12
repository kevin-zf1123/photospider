# Layer, emission and weighted runtime

Implemented in package 0.10, C++ API. The C operation ABI remains 9. These CPU
operations use structured protocol 2 and managed mandatory backing. They do not
extend the legacy image-v2 coverage meaning or enable a GPU implementation.

## Representation and observation

`layer_schema(kind, LayerSpec{H,W,1})` creates a closed version-one schema. Space
1 means linear sRGB primaries, D65 white, scene reference and relative units for
both coverage color and emission. Other spaces require another supported mode;
there is no implicit assignment, conversion or scene/display interchange.

| Family | Records and invariant |
| --- | --- |
| `photospider.layer` | H*W coverage Float32[4] and emission Float32[3]; finite P,A,E; 0<=A<=1; A=0 implies P=0 |
| `photospider.layer_response` | H*W Float32[4] Q,T; finite; 0<=T<=1 |
| `photospider.raw_rgba_sum` | H*W Float32[4] P,M; finite M>=0; M=0 implies P=0 |
| `photospider.layer_contributions` | Dynamic ordered Float64[8] P,A,E,w; first seven are exact lifted binary32 Layer samples; finite w>=0 |
| `photospider.weighted_layer_sum` | One Float64[8] Np,Na,Ne,W; finite W>=0, 0<=Na<=W; Na=0 implies Np=0; W=0 implies all numerators zero |
| `photospider.optional_layer` | One UInt8 valid record; 0 or 1 associated coverage/emission records, exactly matching valid |

Contributions, WeightedSum and OptionalLayer use a collection domain and a
one-location working-space specification. Contributions have runtime count,
including zero. They carry unweighted samples so unpublished arithmetic scratch
can have a temporarily invalid association that later cancels. They are not
published WeightedSum records. An emission-only Layer with A=0 remains valid;
zero averaging weight produces no Layer observation.

All six schemas require CompleteBundle. The coordinator validates every field
before observer invocation, shared-result publication or downstream use. Thus a
coverage-only consumer cannot skip nonfinite emission validation. New derived
results bind and retain ordered Result input ObjectIds and their backing; an old
Layer is retained independently of a new Response, sum or finalized Layer.
Static schema validation is allocation-bounded and performs no I/O. Explicit
`validate_layer_result` uses accounted, bounded one-record windows, root work and
I/O admission, cancellation and an optional additional work hook.

## Numerical contract

Layer primitives use binary32 nearest ties-to-even, gradual underflow, no FMA
contraction and rounding after each specified primitive. The caller's floating
point environment is restored. Schema version one and each canonical operation
key identify this arithmetic; no reassociation or Layer/Response rewrite is
installed in the compiler. Signed-zero bits are not automatically normalized.
P and E may be finite signed/HDR numbers.

For Ts=round32(1-As), ordered over computes separately rounded products and adds:
`(Ps+Ts*Pb, As+Ts*Ab, Es+Ts*Eb)`. Coverage opacity scales P and A and preserves E.
Front emission adds `gain*Eg`; behind emission adds `Ts*(gain*Eg)` with rounding
and finite checks at each primitive. Flatten explicitly computes
`(P+E)+(1-A)*B` for a supplied opaque background and emits A=1. Response is
`Q=P+E,T=1-A`. Response over computes `Qs+Ts*Qb,Ts*Tb`; Q,T cannot reconstruct
P,A,E. Raw plus adds P and A into unbounded nonnegative mass M. Checked conversion
requires M<=1; cap-alpha-keep-color sets A=min(M,1) without dividing P.

`layer.weighted_reduce` first requests the complete contribution descriptor.
For [lo,hi), mid=lo+(hi-lo)/2 with integer floor; evaluate left, then right, then
add. A singleton computes each w*component in binary64 and copies w. Every
component, including Na and W, uses the same complete ordinal tree. Non-power-of-
two counts are not padded. A charged depth-64 host stack and bounded input pages
preserve this tree independently of page geometry. Empty input emits a zero sum.
Products and internal adds reject nonfinite results. Association invariants are
checked on the final sum, before it is published. Finalization divides by W in
binary64, rounds each component to binary32, then validates the whole Layer.
W=0 emits valid=false without division. Public `weighted_layer_leaf` and
`weighted_layer_add` helpers return independently valid WeightedSum values, so
they have a narrower success domain than unpublished reduction scratch; the
runtime reducer deliberately does not use these helpers for its internal tree.

For example, with w=2^-1074, P=+1/-1 and A=.5, both Na products round to zero.
The two-leaf sum has Np=0, Na=0, W=2^-1073 and succeeds. A singleton P=1 has
Np=2^-1074, Na=0 and fails AssociationUnderflow. Similarly binary32 opacity .5
on P=1,A=2^-149 fails rather than clearing P or converting it to emission.
`Status.reason` supplies `AssociationUnderflow`, `ArithmeticOverflow`,
`InvalidAssociation` or `EmptyWeightedResult` independently of diagnostics.
These numerical failures produce no partial operation result. This is not a
certified error-bound or RSS guarantee.

The success domains of Layer and Response algebra differ: P=FLT_MAX,A=.5,
E=-FLT_MAX over itself overflows component arithmetic, while its collapsed
Response over itself succeeds. Conversely P=E=FLT_MAX,A=1 is a valid Layer whose
collapse overflows. These examples prohibit an implicit algebraic replacement.

## Public operations and workflow

`make_layer_operation(LayerOperation, LayerSpec)` returns an actual
`OperationDefinition` with a staged CPU implementation, ready for
`OperationRegistry::register_operation`. The fixed raster/space is frozen in the
registered output schema; the caller may choose another operation key when
registering multiple fixed raster variants. Input dimensions and space are
checked before input I/O. Builder growth bounds use the actual output extent:
N rows and 28N bytes for Layer, 16N for Response/RawSum and 64N for raster
contributions; reduction uses one 64-byte row and finalization at most 29 bytes.
They do not inherit the general builder's one-million-row default. Mandatory
disk, Host, work and stage budgets still apply. `test_layer` separately checks
large-raster admission without claiming complete large-raster execution.
The canonical keys are:

| Keys | Input / parameter / output |
| --- | --- |
| `layer.assemble` | canonical RGBA Value + facet-free Float32 HWC3 explicitly interpreted as emission -> Layer |
| `layer.over` | front Layer, back Layer -> Layer |
| `layer.opacity` | Layer; required Float64 factor in [0,1], rounded to binary32 -> Layer |
| `layer.emit_front`, `layer.emit_behind` | Layer + explicit RGB emission Value; required finite Float64 factor in +/-FLT_MAX, rounded to binary32 -> Layer |
| `layer.flatten` | Layer + explicit RGB opaque background Value -> typed dense Whole RGBA Value |
| `layer.response`, `layer.response_over` | Layer -> Response; two Responses -> Response |
| `layer.coverage_raw_plus` | two Layers, explicitly select coverage only -> RawSum |
| `layer.raw_checked`, `layer.raw_capped` | RawSum -> Layer with E=0, using the selected mass policy |
| `layer.weight` | Layer raster; required finite Float64 weight>=0 -> unweighted raster-ordinal contributions carrying that weight |
| `layer.weighted_reduce` | dynamic contribution collection -> one WeightedSum |
| `layer.weighted_finalize` | WeightedSum -> OptionalLayer |
| `layer.require_valid` | OptionalLayer -> Layer or EmptyWeightedResult |

The three-channel Value ports deliberately require no facets: their meaning is
assigned explicitly by these operations' named working-space contract. They do
not accept contradictory existing metadata. Flatten uses the existing Typed
Image port and image-v2 facet; it requires Whole output and sufficient admitted
capacity for its dense output. Other operations page mandatory Result backing.
They do not claim arbitrary image tiling or all effects. Host, Payload, Disk,
I/O and work limits can return ResourceExhausted without a completion promise.
New state, pages, witnesses and bounded adapter vectors are admitted by the root;
legacy Footprint/Value bookkeeping and platform allocations retain the exclusions
in [Managed-Resources.md](Managed-Resources.md). Measured managed-capacity peaks
are not process RSS bounds. Support is Conservative(All), with descriptor support
also for empty collections, never marked Exact. Result descriptor support is a
separate role-8 observation at [0,1), independent of data rows; callers report
count/basis edits in that descriptor role. No synthetic data row is created.

Run from the repository:

```sh
cmake --build build/issue257-shared --target test_layer photospider_layer_workflow -j8
ctest --test-dir build/issue257-shared -R '^(test_layer|photospider_layer_workflow)$' --output-on-failure
```

Standalone installed consumer:

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-delivery/install"
cmake -S examples/layer_workflow -B out/phase-a-delivery/layer-consumer -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/layer-consumer -j8
out/phase-a-delivery/layer-consumer/photospider_layer_workflow
```

`examples/layer_workflow/main.cpp` supplies real RegionalSources, WorkflowDocument,
Compiler and ExecutionContext calls, then checks output after context destruction.
Its rational raster reference is RGBA `[12.5,-2,1.25,1]`. Dynamic count=3,5,7
midpoint fixtures containing `2^54,-2^54,1` expect Np=0, while linear accumulation
or padding can produce 1. At weights `[2^53,1,1]`, Np=Na=W=9007199254740994.
Windows 64/192/256 preserve these references. It also checks empty/zero/negative
weights, strict underflow, scratch cancellation, explicit empty-image failure,
insufficient page/work budgets, duplicate cache-off consumers and final pin
lifetime. Its 8192-record case stores 512 KiB of contribution data under a
256 KiB managed Host limit with a finite explicit 100000-stage limit.
`tests/unit/test_layer.cpp` adds independent integer dyadic over references,
FMA and reassociation counterexamples, mass-vs-weight, signed zero and floating
environment restoration. The expected references are independent of producer
page partition and are not Python model accounting.
