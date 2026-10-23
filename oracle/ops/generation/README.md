# Generation mathematical oracles

Runnable references for the [generation specifications](../../../docs/built-in_ops/03-generation/generators.md).
These Python tools do not register operators or implement the C++ kernel, public
WorkflowDocument protocol, Result codecs, runtime Region callbacks or GPU execution.

## Run

Python 3.10+; exact arithmetic, RNG core, finite geometry, resource and JSONL tests
use the standard library. Optional Measured tests use mpmath 1.3.0. Install it in
an isolated environment to include those tests; without it they are visibly skipped.
NumPy is optional for non-gating spectrum diagnostics only.

```sh
python3 -m venv build/generation-oracle-venv
build/generation-oracle-venv/bin/python -m pip install -r oracle/ops/generation/requirements-oracle.txt
build/generation-oracle-venv/bin/python -m oracle.ops.generation.run_oracles --self-test --report build/generation-oracle/self-test.json
build/generation-oracle-venv/bin/python -m oracle.ops.generation.run_oracles --input oracle/ops/generation/common/requests.jsonl --candidate oracle/ops/generation/common/expected.jsonl --profile strict --report build/generation-oracle/candidate-check.json
```

Reports/logs go under ignored `build/`, not into source fixtures.
Test results are written by the current invocation.

## Evidence and limits

| Module | Implemented reference | Boundary |
| --- | --- | --- |
| `common/exact.py`, `gen*/exact.py`, `noi*/exact.py`, `pth*/exact.py` | Fraction mathematics, IEEE RNE/sqrt, basic fields, generic numeric mesh, source-width coordinates, rational-root two-circle subset | Not full shape/metadata validation; irrational two-circle roots explicitly unsupported |
| `common/rng.py`, `noi*/rng.py`, `gen08/rng.py` | Philox4x64-10 core with three upstream KATs, v1 addressed sequences, fixed Perlin polynomial, point algorithms | Packing, seed-to-key injection and Float32/Float64 half-open and open52 extraction match production v1; finite mapping checks do not establish runtime delivery; Bridson candidates use ordinary math and are Measured |
| `common/geometry.py`, `gen03/geometry.py`, `noi05/geometry.py`, `pth*/geometry.py` | Finite rational curves, flatten bounds, polygon area/Boolean boundaries, polyline fixtures | No Result ABI; many arc operations require rational segment lengths; publication helper performs necessary checks only, not general topology certification |
| `common/measured.py`, `gen*/measured.py`, `noi*/measured.py`, `pth*/measured.py` | High-precision integral/transcendental/Poisson diagnostics | All outputs are Measured; precision agreement does not certify rounding or global roots |
| `common/resources.py`, `noi07/resources.py`, `noi10/resources.py` | Raw rank structure/hash/lookup/threshold checks | Synthetic fixtures are not approved blue/STBN assets |
| run_oracles.py | Finite JSONL reference/candidate comparison | Oracle protocol, not kernel workflow JSON; accelerated sample comparison is not admission |

[Per-member coverage](../../../docs/built-in_ops/03-generation/oracle-coverage.md)
lists the available helper subset and remaining limitations for all 95 current member
specifications. No coverage level means the corresponding operator is implemented.
A helper test is not complete validation of every member using it.

## PTH-04 exact irrational-length relations

`pth04/geometry.py::polyline_arc_relation(points, fraction, segment, local_t, closed=False)` proves a supplied arc-coordinate relation for rational vertices and rational parameters. It groups squared lengths by exact rational square ratios using Python `Fraction` and `math.isqrt`; distinct square classes cannot cancel. This includes equal diagonal lengths, scaled roots and cancellation across mixed square classes. It is a candidate-equality oracle, not a solver for arbitrary curves, a junction-ownership validator or an IEEE publication certificate. The existing sampling helpers still require rational segment lengths.

## PTH-12 exact Boolean and grid references

The C++ production backend is the in-repository rational `PolygonArrangement`, with a separate radical `CircularArrangement` for circular strokes and offsets. These Python modules are independent, offline references:

- `pth12/slab.py::verify_boolean(a, b, result, operation, fill_rule_a, fill_rule_b)` verifies the whole regularized region using exact `Fraction` arithmetic and a vertical decomposition. Critical x coordinates include all source/output vertices and finite-edge intersections. Every open slab compares complete affine interval boundaries, rather than sampled pixels or area totals. Vertical edges and point contacts follow from regular closure. This proves region equality for the finite polygon input; it does not prove canonical contour serialization.
- `pth12/geometry.py::boolean_regions` supplies a separate exact boundary/halfedge reference for canonical cycles. Collinear simplification respects global boundary degree, retaining vertices where contours contact.
- `pth12/grid.py::boolean_grid` freezes ties-even input quantization, exact intermediate arrangement, then ties-even result-vertex snapping with cycle order and degenerate snapped edges retained. It invokes the slab checker on the exact intermediate region. `publish_grid` maps signed64 coordinates through exact origin/spacing and RN64, returning the exact squared publication-error maximum.
- `pth12/fixtures.py` contains 13 named cases covering an empty counterpart, rectangle configurations, shared edges, point contacts (including a collinear contact), holes, self-crossing, thin dyadic slivers and non-dyadic intersections. Callers may apply each of the four set operations and compare region, canonical encoding and independently known areas where available.

The slab proof relies on finite closed line contours, exact intersection enumeration, simultaneous same-height winding events, and equality of affine interval endpoints throughout each slab. It does not apply to circular offsets, and does not certify the Float64 publication topology premises. Temporary native execution adapters are removed after implementation; the algorithm references and fixtures remain reusable.

## RNG v1 reference

The selected Philox4x64-10 core, counter layout, key `(seed_bits,0)` and finite-grid mappings follow [production v1](../../../docs/built-in_ops/03-generation/op_specs/NOI_random_contract.md). Counter words encode x/y, frame/draw and stream/channel/domain in that order, with 72 reserved bits fixed to zero. Float32 halfopen24 uses the high 24 bits of word 0; Float64 halfopen53 uses the high 53 bits. Open52 uses the high 52 bits of its selected word.

Box-Muller uses word 0 for open52 and word 1 for halfopen53. Jitter, finite rejection, Bridson and cellular use words 0/1 for their two halfopen53 samples. The shared contract defines draw selections, rejection candidate addresses and Bridson iteration/attempt addresses. General integer rejection has a specified rule but no executable helper here; each consumer still requires its own address allocation and attempt budget.

The JSONL uniform, gradient2d and fractal references use the specified mappings. Statistics remain finite diagnostics, and Box-Muller/Bridson transcendental calculations remain Measured. Specifying a random sequence does not establish correct rounding of those transforms or any production operator/backend execution.

```sh
python3 -m oracle.ops.generation.run_oracles --statistics --report build/generation-oracle/statistics.json
```

Philox core KATs are independent of application packing. Fixed Perlin 2002 has its own permutation, no seed, and does not use Philox.

## Mathematical semantics

Production schemas and random protocols are unchecked: only static structure is checked. Domain checks in Python helpers diagnose unsupported reference inputs; they are tooling guards, not runtime validation to copy into operators, publishers or consumers. Valid-input numerical fixtures remain the acceptance evidence. ObjectId pairing, counts, offsets, finite geometry and random-address ranges are production caller premises.

Repeat/angle final rounding is not followed by nextDown/wrap. Cell point placement
and rank midpoint mappings retain their separately specified endpoint rules.
Generic meshes treat every component numerically, including a fourth component.
Point max_count is a normal target; `with_status=True` returns count/candidate/active
termination reasons. Independent iteration limits fail. A zero max_count returns an empty point list with count_reached before any random sample. The target Result schema represents that list with static capacity at least one and count=0; these list helpers do not implement Result publication.
Width helpers retain source position rather than reapplying the profile to each
fragment. They do not implement snapshot association or arbitrary curve remapping.

The linear RGB/RGBA reference follows current CRV-06 caller premises. In particular, positive exact alpha rounding to zero requires every rounded RGB component to be zero. Valid transparent tuples canonicalize to +0. The JSONL evaluator rejects a probe outside that reference domain with a `ValueError` identifying the caller precondition; it does not define or test a production `AssociationUnderflow` failure.

## JSONL

Each request has a unique string id, a supported op (`--list`) and args. Floating
outputs carry dtype and IEEE hexadecimal bits. Input bit envelopes preserve binary
values; rational envelopes are mathematical probes, not new runtime dtypes.
Boolean output is the exact rational boundary before Float64 publication.
Candidate-result quality labels are never treated as proofs.

The 14 sample expected results come from hand calculations and the external core
KAT, not by running this oracle and calling the result independent. The same cases
also appear in common/fixtures.json; these are two validation paths, not 28 independent
fixtures. Fixed-seed crosschecks provide additional finite coverage. Provenance:
[SOURCES](SOURCES.md).

## Perlin reference inputs

`noi04/samples.py` provides fixed coordinate samples and reference bits from
`noi04.rng.perlin2002_fraction` plus independent exact IEEE rounding. `cases()` yields
(input_is_float32, output_is_float32, coordinate_bits); `expected(case)` returns
the output bits, and `reference_cases()` yields inputs with reference values.
The 1,566 combinations include Float32/64, negative coordinates, subnormals,
huge finite values and deterministic random coordinates. The reference module
can be imported without argument parsing or native execution. Implementation
adapters are created temporarily for each operation change.

Reference fixture diagnostics, including `publish_polygon_fixture`, are offline oracle tooling. They may reject malformed fixtures to make test errors visible; they do not impose producer publication validation or consumer scans on the unchecked production contracts. Ordinary exact-to-Float64 publication has no topology-certification gate.
