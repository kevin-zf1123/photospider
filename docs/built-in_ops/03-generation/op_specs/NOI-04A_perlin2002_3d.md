---
spec_schema_version: 1
id: NOI-04A
parent_id: NOI-04
function: perlin2002_3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented
clarification_status: resolved
operation_keys:
  - noise.perlin2002_3d_v1_strict_cpu_whole
  - noise.perlin2002_3d_v1_strict_cpu_tiled
  - noise.perlin2002_3d_v1_strict_gpu
---

# NOI-04A: perlin2002 3d

Inherits [NOI-04](NOI-04_gradient_noise_contract.md) and [GEN common](GEN_common_contract.md). The fixed permutation and gradient selection are specified in [NOI_perlin2002_permutation](NOI_perlin2002_permutation.md). The member is independent of the Philox addressed-sequence protocol. Upstream operations construct coordinates and define their units.

## Interface

Operation keys: `noise.perlin2002_3d_v1_strict_cpu_whole`, `noise.perlin2002_3d_v1_strict_cpu_tiled`, `noise.perlin2002_3d_v1_strict_gpu`. All three follow the same exact polynomial and final IEEE rounding contract.

Input: one Result containing one numeric tensor `coordinates[S...,3]`; the input schema selects its sole tensor member, so any valid member key is accepted and `coordinates` is the semantic role. Elements are Float32 or Float64, the rank is 2..8, every extent is positive, and the tensor holds at most 2^40 scalar elements. Output `values`: a `photospider.tensor` version 1 Result with tensor key `samples` and shape `S...`, carrying structure and numerical samples only. Coordinate units and downstream color interpretation are explicit configuration of the consumer. Optional static String `dtype` is `float32` or `float64`, default `float64`. Coordinates are the only numerical input; seed, custom permutation, origin and frequency are not parameters.

## Mathematics

Use the fixed 256-entry permutation, exact floor and modulo-256 reduction, quintic fade `6t^5-15t^4+10t^3`, and the specified `grad(hash & 15)` corner rule. Evaluate the eight exact corner contributions and round the complete polynomial once to the requested IEEE output format, ties-to-even. Integer lattice inputs produce positive zero; a negative nonzero value that underflows retains negative zero. The period is 256 on each axis and the magnitude bound is 2. Strict output follows this mathematical expression rather than Java's intermediate double rounding.

## Behavior

The whole-input variants and the GPU variant depend on the complete coordinate tensor. The tiled CPU variant computes each output sample from its own xyz triple. Consumed coordinates are finite as an unchecked caller premise.

## Acceptance

Finite Float32 and Float64 inputs, negative coordinates, subnormals and large finite coordinates are evaluated against the exact fixed polynomial. The whole and tiled CPU variants and the GPU variant agree bit-for-bit.
