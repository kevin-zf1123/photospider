---
spec_schema_version: 1
id: NOI-06
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# NOI-06: Fractal sums

Bases are versioned member mathematics, not opaque callbacks. Unrounded basis values and exact octave coordinates participate in the final expression. Octaves share scaled samples of the chosen base field; independence is not claimed. Periodicity with lacunarity requires proof.

## Interface

Each member has `strict`, `accelerated_apple_silicon` and `accelerated_x86_64` keys. Coordinates are explicit inputs, independent of output tile size, canvas origin or ROI.

The first input is a one-tensor Float32/Float64 Result with complete `sample_shape()` `S+[D]` (including batch axes), rank 2..8, where D is 2 for `gradient2d_philox` and 3 for `perlin2002_3d`; D alone does not select a basis. NOI-06D adds a second Float32/Float64 `footprint` input of exact shape S. Input dtypes may differ. Output `values` is `photospider.tensor` v1 / `samples` of shape S, with no atomic component axis, input batch-axis partition or color metadata. Extents are positive and every logical product is at most 2^40.

| Static parameter | Type | Default / meaning |
| --- | --- | --- |
| `basis` | String | `gradient2d_philox`; alternatively `perlin2002_3d` |
| `octaves` | Int64 | 4; caller premise 1..32 |
| `frequency` | Float64 | 1; finite and positive |
| `lacunarity` | Float64 | 2; finite and greater than 1 |
| `gain` | Float64 | 0.5; finite in [0,1] |
| `normalization` | String | `amplitude_sum`; alternatively `none` |
| `dtype` | String | `float64`; alternatively `float32` |
| `seed`, `stream` (gradient basis) | Int64 | Required; seed is a 64-bit bit pattern, stream is in the uint32 caller domain |
| `frame` (gradient basis) | Int64 | 0; uint32 caller domain |
| `period_x`, `period_y` (gradient basis) | Int64 | 0; no period, or a positive period through 2^31-1 |

The Perlin basis uses its fixed permutation and period 256; supplying gradient seed, stream, frame or period parameters with it is a static interface mismatch. Gradient basis corners inherit NOI-04B's signed32 caller domain and Euclidean period reduction. A term with exactly zero amplitude or zero attenuation is skipped before its scaled coordinates are formed. No octave receives a new seed or random stream.

## Mathematics

Exact binary inputs, octave scales and weights are formed exactly; the unrounded polynomial basis is evaluated and only the complete sum is rounded. Accelerated Float64 uses one final [NUM FP32 four-ULP bound](../../01-numeric/op_specs/NUM_accelerated_contract.md), never a per-octave allowance. Numerical parameter and payload domains are unchecked; shape, type, enum and presence mismatches are static errors.

## Members

| ID | Specialization |
| --- | --- |
| [NOI-06A](NOI-06A_fbm_sum.md) | fbm sum |
| [NOI-06B](NOI-06B_turbulence_sum.md) | turbulence sum |
| [NOI-06C](NOI-06C_ridged_sum.md) | ridged sum |
| [NOI-06D](NOI-06D_footprint_fbm.md) | footprint fbm |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md).
