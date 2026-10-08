# Layer values and the Result boundary

[Chinese reader version](zh/Layer-Runtime.zh.md).

Layer value types and their pure arithmetic helpers remain available in the C++ API. The Result schema validator rejects `photospider.layer`, `photospider.layer_response`, `photospider.raw_rgba_sum`, `photospider.layer_contributions`, `photospider.weighted_layer_sum`, and `photospider.optional_layer`, as well as any Result facet keyed `photospider.layer`. These schemas do not provide a supported Result image representation. Images use [Result Tensor storage and region access](../kernel-specs/Tensor-Storage-and-Region-Access.md); `PlanarImage` is typed backing for that storage, not a separate public image path.

## 1. Scope and ownership

`photospider/data/layer.hpp` owns in-memory value contracts and pure calculations. Cancellation, resource budgets, and allocators used by the public Result helpers live under `photospider/core/` (for example, `photospider/core/resources.hpp` and `photospider/core/cancellation.hpp`). `layer.hpp` does not expose a Layer operation factory or a runnable Layer Result workflow. A `LayerPixel` can be passed directly to the value helpers without creating a Result.

The image pipeline stores samples in Result Tensors and retains field or tensor backing through Result owners and read windows. `PlanarImage` may provide typed backing inside that pipeline. Layer's coverage/emission pair remains an in-memory value and does not define image Tensor storage or Region access.

## 2. Data layout and memory

```cpp
struct CoveragePixel final { std::array<float, 3> p{}; float a = 0; };
struct LayerPixel final { CoveragePixel coverage; std::array<float, 3> emission{}; };
struct LayerResponsePixel final { std::array<float, 3> q{}; float t = 1; };
struct RawRgbaSumPixel final { std::array<float, 3> p{}; float mass = 0; };
struct WeightedLayerSum final { std::array<double, 8> components{}; };
struct LayerContribution final { std::array<double, 8> components{}; };
struct OptionalLayerPixel final { bool valid = false; LayerPixel value; };

Result<SchemaTemplate> layer_schema(LayerRepresentation, const LayerSpec& = {});
Status validate_layer_result(const ResultRef&, const ResourceBudget&,
                             std::uint64_t maximum_window,
                             const CancellationToken& = {},
                             const std::function<Status(std::uint64_t)>& = {});
```

`CoveragePixel` stores associated color `P` and coverage `A`; every `P` component is finite, `A` is finite in `[0,1]`, and `A=0` requires `P=0`. `LayerPixel` adds independent finite, signed emission `E`. Version-one arithmetic names linear sRGB primaries, D65, relative scene units. Response stores finite `Q=P+E` and `T=1-A` in `[0,1]`, so it cannot recover the original coverage/emission split.

Raw sum stores finite `P` and finite nonnegative additive mass `M`; `M=0` requires `P=0`, and mass is not transmittance. A contribution stores binary64 `P,A,E,w`, with the first seven values exactly lifted from binary32 and finite `w>=0`. A published weighted sum stores `Np[3],Na,Ne[3],W`; it requires finite values, `W>=0`, `0<=Na<=W`, `Na=0 => Np=0`, and `W=0 => all numerators are zero`. Internal reduction scratch may temporarily violate the association relation. An optional Layer with `valid=false` represents an empty weighted observation, not black.

## 3. Execution and state machine

```text
Layer value structs --pure helpers--> Layer / Response / RawSum values
          |
          +-- layer_schema descriptions --> Result schema validation rejects

image samples --> Result Tensor storage --> registered image operations
                         |
                         +-- optional PlanarImage typed backing
```

Layer schema descriptions stop at Result schema validation and cannot reach publication. Pure helpers return local values independently of image execution. Errors from the helpers remain caller-visible.

## 4. Algorithms and math

Pure helpers validate inputs and return a new value without mutating inputs. Their binary32 primitives use nearest ties-to-even, gradual underflow, no FMA contraction, and a separate rounding step at each specified operation. They restore the caller's floating-point environment. Nonfinite results and association underflow return failures; the helpers do not publish partial values.

`layer_over(front,back)` computes `T=round32(1-Af)` and separately rounded `P=Pf+T*Pb`, `A=Af+T*Ab`, `E=Ef+T*Eb`. `layer_opacity` requires finite opacity in `[0,1]`, scales `P` and `A`, and preserves `E`. `layer_emit` requires finite gain and emission; both may be signed. Front emission adds `gain*Eg`; behind emission adds `T*(gain*Eg)`. Flattening an explicit opaque background `B` computes `(P+E)+(1-A)*B` and returns opaque coverage. Response over computes `Qf+Tf*Qb` and `Tf*Tb`.

`raw_rgba_plus` adds `P` and `A` and accumulates nonnegative mass. Conversion either requires `M<=1` or caps alpha at `min(M,1)` while retaining `P`; it never divides by mass. Weighted leaf and add helpers perform binary64 arithmetic and validate each published intermediate. Finalization divides each numerator by `W`, rounds to binary32, then validates the complete Layer. At `W=0`, it returns `valid=false` without division.

The pure `weighted_layer_leaf` and `weighted_layer_add` helpers do not select an accumulation tree. Callers that combine leaves choose and own their grouping policy.

`SchemaTemplate::validate` returns `TypeMismatch` for the six Result schema IDs and the `photospider.layer` facet. `layer_schema` can construct a description for a pure value contract, but that description cannot pass the Result validation boundary. The optional work hook to `validate_layer_result` adds an independent work limit; Root work and I/O are also charged.

Pure helper behavior is covered by `test_layer` and the installed-package consumer `installed_layer_values`. These checks cover the arithmetic API; they do not exercise a Layer operation, image workflow, or GPU path.

## 5. Limitations and non-goals

- No `make_layer_operation` factory or Layer image workflow is available.
- Layer values do not define Result Tensor storage, Region access, tiled image execution, or a GPU backend. `PlanarImage` is typed backing within the Result image path, not a separate public image API.
- The arithmetic contract supplies no certified numerical error bound or process-RSS bound.
- The in-memory helpers do not convert color spaces or infer a working space from sample values. Callers handle helper errors including invalid associations, arithmetic overflow, and weighted association underflow.
