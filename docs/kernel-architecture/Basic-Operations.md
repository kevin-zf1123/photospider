# Basic operations

## Operation inputs and outputs

| Key | Inputs and output | Required parameters |
| --- | --- | --- |
| `curve.sample_linear`, `curve.sample_monotone` | One Result containing generic Float32/Float64 controls `[K,2]` to `photospider.tensor` v1/member `samples`, `[count]`; unbatched, `K >= 2`, finite controls with strictly increasing x; complete-input | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`); Int64 `count` in `[2,1048576]` |
| `field.apply_lut_1d` | Field Result and same-dtype generic table Result `[N]`, `2 <= N <= 2^53`; output `photospider.tensor` v1/member `samples` with field sample shape; complete-input | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`) |
| `field.smoothstep` | Field Result to canonical Float32 `photospider.image` v1/member `pixels`, sample shape `[1,1,H,W]`; member-tile continuation | Finite Float64 `edge0 < edge1` |
| `numeric.minimum`, `numeric.maximum` | Two same-dtype Float32/Float64 tensor Results with identical `sample_shape()`; output port `value` is a `photospider.tensor` v1 Result with member `samples` | None |
| `numeric.abs` | One Float32/Float64 tensor Result; output port `value` is a `photospider.tensor` v1 Result with member `samples`, same sample shape and dtype | None |

Each of the seven operations above accepts Result inputs containing exactly one tensor. Schema id, version, and tensor key may be any structurally valid values. Inputs use Float32 or Float64. Curve controls are unbatched `[K,2]`. A field has rank-two cell shape, and its `batch_axes` are empty or `[1,1]`. The LUT table is unbatched and must match the field dtype. External output port is `value` for every operation. Outputs other than smoothstep use the generic `photospider.tensor` v1 schema with member `samples`; smoothstep restores the image coverage Result schema.

## Math and region behavior

Curve samplers place `count` query coordinates across the closed domain, including both endpoints. Linear interpolation passes through each control point. Monotone interpolation uses a shape-preserving cubic Hermite curve with weighted harmonic interior slopes and limited endpoint slopes; two controls produce a linear segment. `reject` requires the requested domain to lie within the control x range; `clip` returns the nearest endpoint outside it. Collapsed query coordinates fail.

`field.apply_lut_1d` maps the closed input domain to the table's first and last entries and linearly interpolates between table samples. The String policy rejects out-of-domain field values or clips them to endpoint values. This table input is an ordinary generic array.

## Execution and errors

These Result programs read authorized tensor windows and publish through the Result builder. They poll cancellation and restore the caller's floating-point environment; failed operations discard unpublished output. PCHIP keeps three Float64 workspace arrays per control point. Complete-input shared stages require their complete declared support to fit the execution resources.

Invalid static parameters return `InvalidArgument`; incompatible dtype, shape or structural metadata returns `TypeMismatch`. Consumed nonfinite arithmetic samples, collapsed numerical coordinates and unrepresentable output values return `OperationFailed`. Cancellation and resource exhaustion retain their own codes. Publication exposes no incomplete tile on failure.

Image meaning is defined by each operation. Ordinary images default to straight alpha without an automatic RGBA compliance validator. Explicit association conversions and algorithm-specific checks remain operation behavior; no kernel schema tag certifies the pixels.
