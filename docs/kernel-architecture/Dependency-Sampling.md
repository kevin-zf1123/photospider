# Dependency sampling operations

The default registry contains `image.stmap`, `numeric.radius_gather` and `numeric.radius_scatter` definitions using dependency program version 1. The radius operations execute through the current generic Value path, one sample per invocation. The STMap helper is registered but is not executable through the current registry path: `OperationRegistry::start_dependency` rejects its structural image input or inferred image output with `TypeMismatch` before continuation creation. The helper algorithm below records its implemented sampling rules and this execution boundary.

Dependency callbacks use request-level failure delivery, so distinct output observations are not combined into one semantic request. Input dependencies and control evidence use exact fragment coverage as described in [Dependency data and execution](Dependency-Data.md).

## Scope and current execution boundary

The operation definitions retain their dependency-program start functions in the immutable registry record. Public staged invocation is supported for the generic radius operations; STMap cannot reach its start function for structural image Values in the current registry. The code excerpt shows the registration fields relevant to these definitions; other fields are omitted.

```cpp
using DependencyStart = std::function<Result<DependencyContinuation>(
    const DependencyQuery&, const BufferAllocator&)>;

struct OperationDefinition final {
  // Relevant staged fields; additional registry fields are omitted.
  std::string key;
  OperationTraits traits;
  OperationCallback callback;
  DependencyStart start_dependency = {};
  DependencyValidator validate_dependency = {};
};
```

## Data model and STMap helper contract

`ValueFragments` is the input transport type for Value dependency programs. It rejects structural image facets that require `PlanarImage`; therefore the STMap helper implementation is not currently callable through the registry execution route described above. Its parameter, coordinate, boundary and tap rules remain useful for maintaining the registered helper.

`image.stmap(source, map)` takes canonical linear premultiplied Float32 RGBA `source[Hs,Ws,4]` and generic Float64 `map[Ho,Wo,2]`. It returns Float32 `[Ho,Wo,4]` with the source's image-v2 facets. Source axes must be at most 2^40. Map channel 0 is source x, channel 1 source y, in pixel coordinates with centers at `i+0.5`. The required String parameter `boundary` has no implicit default:

| Value | Address rule for an integer tap i on an axis of length N |
| --- | --- |
| `constant` | Outside taps use transparent black and read no source pixel. |
| `clamp` | Clamp to [0,N-1]. |
| `wrap` | Nonnegative remainder modulo N. |
| `reflect` | Period 2N, repeated endpoints: p<N ? p : 2N-1-p. |
| `mirror` | Period 2N-2, unrepeated endpoints: p<N ? p : 2N-2-p. |

For N=1, all nonconstant modes select coordinate zero. These mappings act on global source coordinates, never independently on individual fragments.

For the helper's four ordered taps, let $u$ and $v$ be the map coordinates, and let $l=\lfloor u-\tfrac12\rfloor$ and $t=\lfloor v-\tfrac12\rfloor$. The fractional offsets, tap weights and channel result are:

$$
f_x = u-\tfrac12-l, \qquad f_y = v-\tfrac12-t, \qquad w_{dy,dx} = (dy ? f_y : 1-f_y)(dx ? f_x : 1-f_x),
$$
$$
y_c = \mathrm{Float32}\!\left(\sum_{dy=0}^{1}\sum_{dx=0}^{1} w_{dy,dx} \, s_{dy,dx,c}\right).
$$

The sum is evaluated as sequential Float64 additions with dy outer and dx inner: top-left, top-right, bottom-left, bottom-right. A constant-boundary tap outside the source contributes transparent black. These equations describe the registered helper; structural image Values still cannot reach its callback.

For each output pixel, the program first requests both map components as Control. They must be finite and within [-2^40,2^40]. It forms `floor(x-0.5)` and `floor(y-0.5)`, then visits taps in top-left, top-right, bottom-left, bottom-right order. Each in-domain tap requests the complete RGBA pixel, even when its weight is zero. Footprints deduplicate addresses; the arithmetic retains all four taps. After reading/validating all taps, each channel uses a sequential Float64 weighted sum and one final Float32 conversion under nearest rounding with gradual underflow. Transparent constant taps retain their map and descriptor evidence.

The registered validator checks map rank, source-axis bounds and the `boundary` vocabulary during preparation, including Empty queries. The helper code checks finite coordinates and source pixels and returns `OperationFailed` for its observation; exact-set, state, discovery or allocation limits return `ResourceExhausted`. These are helper implementation rules, not a reachable image execution contract: structural image inputs and outputs fail the current Value fragment or registry guard before callback polling.

## Executable radius algorithms

Both operations take generic Float64 `source[N]` and Int64 `radius[N]`, with 1 <= N <= 2^40. They return generic Float64 `[N]`, dropping input facets. They have no parameters. Radius samples used by the operation must lie in [0,2^40].

For output index $o$, gather uses the radius at $o$ and scatter uses the radius at each candidate $i$:

$$
G(o) = \{i \in [0,N) : |i-o| \le r_o\}, \qquad S(o) = \{i \in [0,N) : |i-o| \le r_i\}.
$$

The current gather implementation converts $G(o)$ to a clipped half-open interval. With valid `output < size` and `radius <= 2^40`, it computes:

```cpp
cursor = output > radius ? output - radius : 0;
end = output + 1 + std::min(radius, size - output - 1);
```

Gather reads one output-aligned radius and the clipped source interval. Scatter scans every source radius in chunks of at most 64 candidates and retains the complete Control witness, including excluded candidates. It then reads only positive Data hits. A formerly excluded distant candidate can therefore dirty the output when its radius changes; the implementation needs no spatial inverse index.

Both accumulate source values in increasing index order with a Float64 left fold from positive zero. Each poll establishes and restores nearest rounding and gradual underflow; changing the caller's rounding mode cannot change the result. Nonfinite included source values, invalid observed radii and intermediate sum overflow fail OperationFailed. Chunking never replaces ordered addition by block sums. A gather query does not validate radii outside its own output observations; scatter's complete control scan is part of its declared dependency semantics.

The fixed 64-candidate state uses a host continuation lease. Candidate reads, stage transitions, certificates and source data remain subject to execution limits. Large scatter queries can fail explicitly when their exact scan cannot fit these bounds; no empty certificate or approximate answer is substituted.

## Execution state and errors

```text
Empty query -> static validation -> no state callback or sample read
Nonempty radius query -> start -> Control Need -> exact supply
  gather -> Data chunks of at most 64 -> ordered fold -> publish sample
  scatter -> full Control scan by chunks -> Data hits -> ordered fold -> publish sample
```

The coordinator supplies only declared Value fragments to each poll. Radius state carries its scan cursor, at most 64 hit indices, stage and Float64 accumulator.

## Validation, errors and evidence

`OperationDefinition::validate_dependency` is an optional pure metadata/parameter validator. The compiler calls it after base inference; a direct session calls it before deciding whether Q is Empty. It cannot read pixels, change inferred metadata or inspect Q. The immutable definition owns it, and callback exceptions are fenced. Empty requests can skip state construction while retaining the same static validity rules as nonempty requests.

`test_dependency_sampling` exercises both radius operations with their current generic registry path, including literal finite dependency relations, equal output bytes with changed edges, control transposition, negative/end coordinates, `N=1`, cross-chunk sums and rounding-mode invariance. The [G4 radius workflow](../../examples/g4_workflow/dynamic.cpp) exercises the supported path by patching radius data, checking gather/scatter values and dirty evidence, and confirming that a frozen execution retains the old input. The workflow contains no executable STMap claim; the registry guard rejects structural image Values before the helper callback can run.

## Generic tuple observations and numeric diagnostics

C++ `OperationOutputTraits::atomic_trailing_axes` groups complete trailing axes into one atomic observation for CPU staged Atomic outputs. Zero keeps scalar observations. For generic shape `{N,C}`, value 1 gives observation shape `{N}`; for an axis tuple `{3}`, value 1 gives one observation with shape `{1}`. The host closes a partial sample request over its complete tuple for computation, validation and certificates, then returns the requested sample intersection. Image-v2 metadata defines complete-pixel closure, but the current Value dependency executor rejects structural image plans. Grouping is preserved in compiler edge metadata, structured bridges and joint compatibility checks, and is included in operation identity. The version-11 C descriptor does not expose this C++ trait.

`DependencyPhase::report_numeric` accepts checked incremental `NumericDiagnostics` records, owned inline by the host. Reports identify the actual CPU profile and implementation, evaluated values, strict fallbacks and bounded fallback reasons. Arithmetic reported before a later failure remains visible on terminal atom progress and `OperationTiming::numeric`. Structured consumers accumulate every upstream observation. Cache hits and unrequested observations add no arithmetic counts. These physical diagnostics do not change semantic identity or establish an accuracy bound. See the manual [numeric workflow](../../examples/numeric_workflow/README.md).

## Limitations and caller handling

Use the radius operations with generic rank-one Float64 and Int64 Values. Values carrying structural image metadata require the `PlanarImage` path; rank-three generic numeric arrays remain ordinary Values. `image.stmap` has no executable planar dependency integration. Do not use its registered helper as evidence of a successful workflow. Radius scan or exact-set exhaustion returns `ResourceExhausted`, invalid observed samples return `OperationFailed`, and cancellation propagates from the execution host.
