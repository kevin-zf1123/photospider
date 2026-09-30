# Layer values and the Result boundary

[Chinese reader version](zh/Layer-Runtime.zh.md).

Layer value types and their pure arithmetic helpers remain available in the C++ API. The Result coordinator rejects `photospider.layer`, `photospider.layer_response`, `photospider.raw_rgba_sum`, `photospider.layer_contributions`, `photospider.weighted_layer_sum`, and `photospider.optional_layer`, as well as any Result facet keyed `photospider.layer`. These packed fields do not satisfy the current planar image storage contract. Use [Tensor storage and region access](../kernel-specs/Tensor-Storage-and-Region-Access.md) for images.

## 1. Scope and ownership

`layer.hpp` owns in-memory value contracts and pure calculations. `layer_operation.hpp` still declares factories for the former staged operations, but their Result outputs cannot pass current publication validation. No Layer Result workflow is a supported image storage path. A `LayerPixel` can still be passed directly to the value helpers without creating a Result.

The planar image owner stores image samples and retains its backing for views. Layer's coverage/emission pair has no planar image owner or Region contract. The two ownership models cannot be substituted by attaching metadata.

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
Layer values --pure helpers--> Layer / Response / RawSum values
      |                                  |
      +-- former OperationDefinition ---+--> Result coordinator rejects layer schema
PlanarImage -------------------------------> supported image storage and Region path
```

The Result path ends at coordinator schema validation; it does not reach complete publication. Pure helpers return local values independently of that path. Errors from the helpers remain caller-visible.

## 4. Algorithms and math

Pure helpers validate inputs and return a new value without mutating inputs. Their binary32 primitives use nearest ties-to-even, gradual underflow, no FMA contraction, and a separate rounding step at each specified operation. They restore the caller's floating-point environment. Nonfinite results and association underflow return failures; the helpers do not publish partial values.

`layer_over(front,back)` computes `T=round32(1-Af)` and separately rounded `P=Pf+T*Pb`, `A=Af+T*Ab`, `E=Ef+T*Eb`. `layer_opacity` requires finite opacity in `[0,1]`, scales `P` and `A`, and preserves `E`. `layer_emit` requires finite gain and emission; both may be signed. Front emission adds `gain*Eg`; behind emission adds `T*(gain*Eg)`. Flattening an explicit opaque background `B` computes `(P+E)+(1-A)*B` and returns opaque coverage. Response over computes `Qf+Tf*Qb` and `Tf*Tb`.

`raw_rgba_plus` adds `P` and `A` and accumulates nonnegative mass. Conversion either requires `M<=1` or caps alpha at `min(M,1)` while retaining `P`; it never divides by mass. Weighted leaf and add helpers perform binary64 arithmetic and validate each published intermediate. Finalization divides each numerator by `W`, rounds to binary32, then validates the complete Layer. At `W=0`, it returns `valid=false` without division.

The pure `weighted_layer_leaf` and `weighted_layer_add` helpers do not select an accumulation tree. Callers that combine leaves choose and own their grouping policy.

`SchemaTemplate::validate` returns `TypeMismatch` for the six Result schema IDs and the `photospider.layer` facet. `layer_schema` can construct the field description, but that description cannot pass the current Result validation boundary. The optional work hook to `validate_layer_result` adds an independent work limit; root work and I/O are also charged.

## 5. Limitations and non-goals

- `make_layer_operation` can construct definitions, but those definitions do not restore Result publication support.
- Layer is not planar image storage and does not define planar Region access, tiled image execution, or a GPU backend.
- The arithmetic contract supplies no certified numerical error bound or process-RSS bound.
- The in-memory helpers do not convert color spaces or infer a working space from sample values. Callers handle helper errors including invalid associations, arithmetic overflow, and weighted association underflow.
