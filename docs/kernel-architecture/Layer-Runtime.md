# Layer values (composite operations SDK)

[Chinese reader version](zh/Layer-Runtime.zh.md).

This page describes an operations-SDK type. Layer values and their pure arithmetic helpers are declared in `photospider/ops/composite/layer.hpp`, implemented in `plugins/ops/09-composite/layer.cpp`, and built into `Photospider::ops_sdk`. The kernel has no Layer type, no Layer schema, and no Layer-specific validation. Images in workflows use [Result Tensor storage and region access](../kernel-specs/Tensor-Storage-and-Region-Access.md); `PlanarImage` is typed backing for that storage.

## 1. Scope and ownership

The composite SDK owns the in-memory value contracts and pure calculations. The header exposes no operation factory and no Result schema. A `LayerPixel` is passed directly to the value helpers; an operation that stores Layer data in a Result chooses its own tensor layout and passes any interpretation as explicit parameters.

## 2. Data layout and memory

```cpp
struct CoveragePixel final { std::array<float, 3> p{}; float a = 0; };
struct LayerPixel final { CoveragePixel coverage; std::array<float, 3> emission{}; };
struct LayerResponsePixel final { std::array<float, 3> q{}; float t = 1; };
struct RawRgbaSumPixel final { std::array<float, 3> p{}; float mass = 0; };
struct WeightedLayerSum final { std::array<double, 8> components{}; };
struct LayerContribution final { std::array<double, 8> components{}; };
struct OptionalLayerPixel final { bool valid = false; LayerPixel value; };

Status validate_coverage(const CoveragePixel&);
Status validate_layer(const LayerPixel&);
Status validate_raw_rgba_sum(const RawRgbaSumPixel&);
Status validate_weighted_layer_sum(const WeightedLayerSum&);
Status validate_layer_contribution(const LayerContribution&);
```

`CoveragePixel` stores associated color `P` and coverage `A`; every `P` component is finite, `A` is finite in `[0,1]`, and `A=0` requires `P=0`. `LayerPixel` adds independent finite, signed emission `E`; its seven components form one indivisible observation in one linear color space chosen by the caller. The helpers do not name or convert that space. Response stores finite `Q=P+E` and `T=1-A` in `[0,1]`, so it cannot recover the original coverage/emission split.

Raw sum stores finite `P` and finite nonnegative additive mass `M`; `M=0` requires `P=0`, and mass is not transmittance. A contribution stores binary64 `P,A,E,w`, with the first seven values exactly lifted from binary32 and finite `w>=0`. A published weighted sum stores `Np[3],Na,Ne[3],W`; it requires finite values, `W>=0`, `0<=Na<=W`, `Na=0 => Np=0`, and `W=0 => all numerators are zero`. Internal reduction scratch may temporarily violate the association relation. An optional Layer with `valid=false` represents an empty weighted observation, not black. The validation functions return `TypeMismatch` for an invalid imported association.

## 3. Execution and state machine

```text
Layer value structs --pure helpers--> Layer / Response / RawSum values
                                   \
                                    +--> failure Status; inputs unchanged
```

The helpers are synchronous, thread-safe functions. They return local values independently of image execution. Errors remain caller-visible.

## 4. Algorithms and math

Pure helpers validate inputs and return a new value without mutating inputs. Their binary32 primitives use nearest ties-to-even, gradual underflow, no FMA contraction, and a separate rounding step at each specified operation. They restore the caller's floating-point environment. Nonfinite results and association underflow return `OperationFailed` with a stable reason; the helpers do not publish partial values.

`layer_over(front,back)` computes `T=round32(1-Af)` and separately rounded `P=Pf+T*Pb`, `A=Af+T*Ab`, `E=Ef+T*Eb`. `layer_opacity` requires finite opacity in `[0,1]`, scales `P` and `A`, and preserves `E`. `layer_emit` requires finite gain and emission; both may be signed. Front emission adds `gain*Eg`; behind emission adds `T*(gain*Eg)`. `layer_flatten` with an explicit opaque background `B` computes `(P+E)+(1-A)*B` and returns opaque coverage. `response_over` computes `Qf+Tf*Qb` and `Tf*Tb`.

`raw_rgba_plus` adds `P` and `A` and accumulates nonnegative mass. `raw_rgba_coverage` either requires `M<=1` or caps alpha at `min(M,1)` while retaining `P`; it never divides by mass. Weighted leaf and add helpers perform binary64 arithmetic and validate each published intermediate. `weighted_layer_finalize` divides each numerator by `W`, rounds to binary32, then validates the complete Layer. At `W=0`, it returns `valid=false` without division.

The pure `weighted_layer_leaf` and `weighted_layer_add` helpers do not select an accumulation tree. Callers that combine leaves choose and own their grouping policy.

## 5. Limitations and non-goals

- No Layer operation factory, Layer Result schema, or Layer image workflow is available. The kernel does not recognize or reject any Layer-named schema id; schema ids are structural names only.
- Layer values do not define Result Tensor storage, Region access, tiled image execution, or a GPU backend.
- The arithmetic contract supplies no certified numerical error bound or process-RSS bound.
- The helpers do not convert color spaces or infer a working space from sample values. Callers handle helper errors including invalid associations, arithmetic overflow, and weighted association underflow.

The composite operation catalog in [Blending and compositing](../built-in_ops/09-composite/blending.md) lists how bundled operations relate to these types.
