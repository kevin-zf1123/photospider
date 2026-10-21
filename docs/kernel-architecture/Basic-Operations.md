# Basic operations

## Operation inputs and outputs

| Key | Inputs and output | Required parameters |
| --- | --- | --- |
| `curve.sample_linear`, `curve.sample_monotone` | One Result containing generic Float32/Float64 controls `[K,2]` to `photospider.tensor` v1/member `samples`, `[count]`; unbatched, `K >= 2`, finite controls with strictly increasing x | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`); Int64 `count` in `[2,1048576]` |
| `field.apply_lut_1d` | Field Result and same-dtype generic table Result `[N]`, `2 <= N <= 2^53`; output `photospider.tensor` v1/member `samples` with field sample shape | Finite Float64 `domain_min < domain_max`; String `out_of_domain` (`reject` or `clip`) |
| `field.smoothstep` | Field Result to canonical Float32 `photospider.image` v1/member `pixels`, sample shape `[1,1,H,W]` | Finite Float64 `edge0 < edge1` |
| `numeric.minimum`, `numeric.maximum` | Two same-dtype Float32/Float64 tensor Results with identical `sample_shape()`; output port `value` is a `photospider.tensor` v1 Result with member `samples` | None |
| `numeric.abs` | One Float32/Float64 tensor Result; output port `value` is a `photospider.tensor` v1 Result with member `samples`, same sample shape and dtype | None |

Each of the seven operations above accepts Result inputs containing exactly one tensor. Schema id, version, and tensor key may be any structurally valid values. Inputs use Float32 or Float64. Curve controls are unbatched `[K,2]`. A field has rank-two cell shape, and its `batch_axes` are empty or `[1,1]`. The LUT table is unbatched and must match the field dtype. External output port is `value` for every operation. Outputs other than smoothstep use the generic `photospider.tensor` v1 schema with member `samples`; smoothstep restores the image coverage Result schema.

## Math and region behavior

Curve samplers place `count` query coordinates across the closed domain, including both endpoints. Linear interpolation passes through each control point. Monotone interpolation uses a shape-preserving cubic Hermite curve with weighted harmonic interior slopes and limited endpoint slopes; two controls produce a linear segment. `reject` requires the requested domain to lie within the control x range; `clip` returns the nearest endpoint outside it. Distinct representable query coordinates, finite controls with strictly increasing x, and output values representable in the destination dtype are caller preconditions.

`field.apply_lut_1d` maps the closed input domain to the table's first and last entries and linearly interpolates between table samples. Under `reject`, consumed field values lying within the closed domain are a caller precondition; `clip` returns endpoint values outside it. This table input is an ordinary generic array.

## Execution and errors

The four curve/field operations prepare immutable schemas and numerical parameters, then lower to fixed CPU Host invocations. Each curve call emits one task that reads every control tile and writes all selected output tiles in increasing order. It evaluates global output indices, so sparse selection preserves the original sampling grid. Linear sampling uses zero scratch. PCHIP builds its interval widths, secants and slopes once in `24*K` bytes of preallocated Host scratch, private to that invocation. Multiple Runs share immutable preparation and own separate numerical workspaces.

Each field output tile has its own task. LUT tasks read the corresponding field region and the complete table; smoothstep tasks read the corresponding field region, projecting `[1,1,H,W]` output coordinates to `[H,W]` for an unbatched source. Input coverage follows each source's own tile grid, including batch and atomic axes. Both field operations use zero scratch. For example, selecting one spatial smoothstep tile reads only the source tiles covering that rectangle; selecting two separated curve tiles still builds PCHIP slopes once and evaluates only those selected indices.

Preparation checks canonical writes, exact input tile membership, task parameters and scratch capacity before obtaining the fixed entry point. Runtime invocations read borrowed input views and write preallocated output views, poll cancellation and restore the caller's floating-point environment. Successful task outputs become available to dependent tasks. The executor exposes public Result owners when all required tasks of the call succeed and the call retires in order. A failed call exposes no partial public Result.

Finite arithmetic samples, ordered curve controls, distinct query coordinates, representable outputs and the `reject` policy's payload domain conditions are caller preconditions. The kernels perform no full-input validation scan. Numerical algorithms retain guards for representable PCHIP secants, scaled interpolation distances, compensated-product alignment and interpolation precision; LUT retains bounded cell correction and a checked floating-point-to-index conversion. These guards report `OperationFailed` through bounded invocation-local error state, without allocating exception strings. Static parameter errors return `InvalidArgument`, incompatible schemas return `TypeMismatch`, and cancellation and resource exhaustion retain their own codes.

Image meaning is defined by each operation. Ordinary images default to straight alpha without an automatic RGBA compliance validator. Explicit association conversions and algorithm-specific checks remain operation behavior; no kernel schema tag certifies the pixels.
