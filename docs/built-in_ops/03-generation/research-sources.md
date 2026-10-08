# Generation research sources

These primary sources provide mathematical background and terminology for the
current specifications. Photospider numerical rounding inherits NUM; color semantics
inherit FMT. The member specifications define the adopted behavior. A reference does
not establish runtime compatibility, select a production backend, approve an asset
license, or certify resource quality. Rolling documentation URLs are not version pins.

## S01 — W3C SVG 2 Paint Servers

Source: <https://www.w3.org/TR/SVG2/pservers.html>

Adoption boundary: coordinate systems, spread, linear gradients and radial gradients.
Vocabulary and geometry inform the specifications; degeneracy, sampling and rounding
follow the Photospider member contracts.

## S02 — W3C SVG 2 Paths

Source: <https://www.w3.org/TR/SVG2/paths.html>

Adoption boundary: M/L/Q/C/Z commands, subpaths and closure semantics. This reference
does not imply an SVG text parser or complete SVG compatibility.

## S03 — W3C SVG 2 Painting

Source: <https://www.w3.org/TR/SVG2/painting.html>

Adoption boundary: nonzero/evenodd filling, caps, joins, miters and dashes. Photospider
precision requirements and zero-length handling are specified independently.

## S04 — W3C CSS Color 4

Adoption boundary: interpolation spaces, hue and alpha semantics. Concrete model
calculations reuse repository CRV/FMT contracts; browser defaults are not normative.

## S05 — D. E. Shaw Research Random123

Source: <https://github.com/DEShawResearch/random123>

Adoption boundary: counter-based random generation. Photospider address packing is
specified independently of upstream API conventions. Production packing and floating
bit extraction remain unresolved in the random contract.

## S06 — Random123 philox.h

Source: <https://raw.githubusercontent.com/DEShawResearch/random123/main/include/Random123/philox.h>

Adoption boundary: Philox4x64-10 multipliers, Weyl constants and round permutation.
The Python oracle contains the selected constants and an independent implementation;
the upstream C/C++ implementation is not bundled as a production backend.

## S07 — Random123 known-answer vectors

Source: <https://raw.githubusercontent.com/DEShawResearch/random123/main/tests/kat_vectors>

Adoption boundary: three external Philox4x64-10 known-answer vectors are retained in
[the oracle fixture](../../../oracle/ops/generation/philox4x64_kat.json).
These validate the core permutation; they do not approve candidate address packing
or floating conversion. Self-generated goldens cannot replace independent answers.

## S08 — Ken Perlin, Improved Noise

Source: <https://mrl.cs.nyu.edu/~perlin/noise/>

Adoption boundary: the 2002 fixed permutation, gradient selection and quintic fade.
The permutation is recorded in the local specification. Alternative seed/hash schemes
require distinct names. The Java reference implementation is not bundled.

## S09 — Robert Bridson, Fast Poisson Disk Sampling

Source: <https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf>

Adoption boundary: active lists, annular candidates and neighborhood acceleration.
Photospider additionally specifies traversal, random slots and termination behavior.
The paper alone does not define the repository's deterministic sample sequence.

## S10 — Wolfe et al., Spatiotemporal Blue Noise Masks

Source: <https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks>

Adoption boundary: two-dimensional blue-noise quality and spatiotemporal spectral
quality are different guarantees. Structural rank validation establishes neither.

## S11 — NVIDIA STBN SDK

Source: <https://github.com/NVIDIA-RTX/STBN>

Adoption boundary: a potential source of assets and generation tools. No production
asset package is selected. Release identity, content hashes, licensing and quality
thresholds require explicit qualification before a named STBN resource is advertised.

## S12 — PBRT, Fourth Edition, Sampling 1D Functions

Source: <https://www.pbr-book.org/4ed/Sampling_Algorithms/Sampling_1D_Functions>

Adoption boundary: inverse-distribution and Box–Muller background. The member
mathematical formulas, rather than webpage implementation code, define behavior.

## S13 — PBRT, Third Edition, Noise

Source: <https://www.pbr-book.org/3ed-2018/Texture/Noise>

Adoption boundary: the relationship between multiple octaves and sampling footprints.
Frequency truncation does not establish strict band limitation.

## S14 — Jonathan Shewchuk, Robust Predicates

Source: <https://www.cs.cmu.edu/~quake/robust.html>

Adoption boundary: robust geometric predicates require guarantees beyond ordinary
approximate floating arithmetic. The predicates source code is not bundled.

## S15 — CGAL 2D Regularized Boolean Set-Operations

Source: <https://doc.cgal.org/latest/Boolean_set_operations_2/index.html>

Adoption boundary: regularized set-Boolean terminology and a potential exact-geometry
backend. The specification does not select or claim an existing CGAL integration.

## S16 — Angus Johnson, Clipper2 Overview

Source: <https://angusj.com/clipper2/Docs/Overview.htm>

Adoption boundary: candidate implementations for multiple fill rules and polygon
offsets. Coordinate quantization must not be presented as an exact solution for the
original floating-coordinate geometry. No production backend is selected here.

## S17 — SciPy BSpline

Source: <https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html>

Adoption boundary: Cox–de Boor basis recursion and valid knot intervals. Implicit
extrapolation is disabled by the Photospider member contract.

## S18 — SciPy PchipInterpolator

Source: <https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html>

Adoption boundary: shape-preserving interpolation background. Actual slope selection
and rounding boundaries inherit CRV-01B.

## S19 — NumPy Generator.poisson

Source: <https://numpy.org/doc/stable/reference/random/generated/numpy.random.Generator.poisson.html>

Adoption boundary: the Poisson PMF and count outputs. NumPy's seeded output sequence
does not define the Photospider sequence.

## S20 — Philip J. Schneider, FitCurves

Source: <https://github.com/erich666/GraphicsGems/blob/master/gems/FitCurves.c>

Adoption boundary: cubic fitting algorithm background from the Graphics Gems code
archive. Discrete residuals cannot replace a continuous Hausdorff certificate;
solver-specific behavior belongs to distinct fitting operators.

## S21 — EMVA 1288

Source: <https://www.emva.org/standards-technology/emva-1288/>

Adoption boundary: sensor-characterization background. The current references do not
establish clause-by-clause conformance or physical camera calibration.

## S22 — GIMP RGB Noise

Source: <https://docs.gimp.org/3.0/en/gimp-filter-noise-rgb.html>

Adoption boundary: artistic channel-noise functionality only. This reference does
not establish bit identity with GIMP or any commercial software.

## Evidence boundaries

GNU GSL integration pages, Basler pages and unrelated search results are not evidence
for these specifications. No commercial-product black-box comparison is claimed.
Defaults, resource limits, algorithm encodings and geometric quality thresholds are
Photospider contract choices unless a member explicitly identifies an adopted rule.
The oracle's dependencies and supported reference subsets are documented in its
[README](../../../oracle/ops/generation/README.md) and
[SOURCES](../../../oracle/ops/generation/SOURCES.md).
