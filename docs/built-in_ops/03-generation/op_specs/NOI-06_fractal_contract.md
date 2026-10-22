---
spec_schema_version: 1
id: NOI-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-06: Fractal sums

Bases are versioned member mathematics, not opaque callbacks. Unrounded basis values and
exact octave coordinates participate in the final expression. Octaves share scaled
samples of the chosen base field; independence is not claimed. Periodicity with
lacunarity requires proof.

## Registered CPU interface

All four members are implemented with `strict`, `accelerated_apple_silicon`,
and `accelerated_x86_64` keys. The named accelerated profile is admitted only on
its architecture; these members do not register GPU kernels. Coordinates are
explicit inputs, independent of output tile size, canvas origin or ROI.

The first ordered input is a one-tensor Float32/Float64 Result with explicit
complete `sample_shape()` `S+[D]`, including batch axes, rank 2..8 and D selected
by `basis`. D is 2 for
`gradient2d_philox`, 3 for `perlin2002_3d`. D alone does not select a basis.
NOI-06D adds a second Float32/Float64 `footprint` input of exact shape S. Input dtypes
may differ. Output `values` is `photospider.tensor` v1 / `samples`, shape S,
no atomic component axis, input batch-axis partition or color metadata. Extents are positive and all logical
products are at most 2^40.

| Static parameter | Type | Default / meaning |
| --- | --- | --- |
| `basis` | String | `gradient2d_philox`; alternatively `perlin2002_3d` |
| `octaves` | Int64 | 4; caller premise 1..32 |
| `frequency` | Float64 | 1; finite and positive |
| `lacunarity` | Float64 | 2; finite and greater than 1 |
| `gain` | Float64 | 0.5; finite in [0,1] |
| `normalization` | String | `amplitude_sum`; alternatively `none` |
| `dtype` | String | `float64`; alternatively `float32` |
| `max_work` | Int64 | INT64_MAX; per-invocation operational work cap |
| `seed`, `stream` (gradient basis) | Int64 | Required explicit values; seed is a 64-bit bit pattern, stream is in the uint32 caller domain |
| `frame` (gradient basis) | Int64 | 0; uint32 caller domain |
| `period_x`, `period_y` (gradient basis) | Int64 | 0; no period, or positive period through 2^31-1 |

The Perlin basis uses its fixed permutation and period 256; supplying gradient
seed/stream/frame/period parameters with it is a static interface mismatch.
Gradient basis corners inherit NOI-04B's signed32 caller domain and Euclidean
period reduction. Exact zero-amplitude/zero-attenuation terms skip basis
construction before forming scaled coordinates. No octave is assigned a new
seed or random stream.

Each selected output tile has one fixed Host CPU invocation reading matching
coordinate tiles including all D components, plus matching footprint tiles for D.
Each read closes on its source grid. Empty creates no sample task. Changes to
shared static parameters invalidate the affected call; coordinate and footprint
changes follow these pointwise dependencies.

Strict imports exact binary inputs, forms exact octave scales and weights,
evaluates the unrounded polynomial basis and rounds only the complete sum. The
Float32 interval path proves the same final rounding; accelerated Float64 uses
one final [NUM FP32 four-ULP bound](../../01-numeric/op_specs/NUM_accelerated_contract.md),
never a per-octave allowance. Uncertain classification or range falls back to
exact arithmetic. Numerical parameter and payload domains are unchecked; static
shape/type/enum/presence mismatches are checked. Fixed arithmetic scratch and
work are bounded: capacity/work exhaustion returns `ResourceExhausted`, and
cancellation propagates without approximation or successful partial publication.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-06A](NOI-06A_fbm_sum.md) | fbm sum |
| [NOI-06B](NOI-06B_turbulence_sum.md) | turbulence sum |
| [NOI-06C](NOI-06C_ridged_sum.md) | ridged sum |
| [NOI-06D](NOI-06D_footprint_fbm.md) | footprint fbm |

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile means bit-identical output; numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. Other member/backend limitations remain explicit; CPU validation does not establish GPU or cross-architecture execution coverage.
