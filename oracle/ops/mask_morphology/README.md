# MASK mathematical oracle suite

Independent small-image references for all 63 specified MASK members. The scripts
never call a production mask kernel. They are not WorkflowDocument APIs, a native
implementation, metadata interpreter or Region/resource/ownership simulator.
See the [spec index](../../../docs/built-in_ops/04-mask-morphology/masks.md) and
[shared acceptance obligations](../../../docs/built-in_ops/04-mask-morphology/op_specs/MASK_common_contract.md#independent-acceptance-and-public-implementation-gate).

## Run from the repository root

```sh
python oracle/ops/mask_morphology/run_cases.py --verify
python oracle/ops/mask_morphology/test_oracles.py --report /tmp/mask-test-report.json
```

Python 3.10+ is required. Rational, discrete and quadratic references use the
standard library only. Gaussian references and MPFR comparison tests additionally
require **MPFR 4.2+ on an LP64 Linux/macOS host**. They reuse only the repository's
manual binding in `oracle/ops/numeric/math_oracle_support.py`. Set
`PHOTOSPIDER_ORACLE_MPFR` to the absolute shared-library path when automatic lookup
fails. The module frees MPFR temporaries at context exit and does not install
allocation hooks into any kernel. Windows/LLP64 is not supported by that binding.

NumPy, SciPy and scikit-image are optional for external differential checks.
Without them those five unittest methods are skipped, and the report says so.
Actual dependency versions and skips are recorded in `test_report.json`.

## Files and reproducibility

| File | Purpose |
| --- | --- |
| `exact.py` | Integer/Fraction RN32/RN64, nearest-even midpoint/subnormal handling, exact a+b*sqrt(q), and inherited accelerated-bound checker. |
| `reference.py` | 63 independent mathematical entries, brute-force distances, connectivity and iterative reference algorithms. |
| `color_statistics.py` | Exact grouped moments, LDL-derived rounded Cholesky models and model application. |
| `ciede2000.py` | Directed MPFR CIEDE2000 and complete selection-response certificates. |
| `gaussian.py` | Certified MPFR enclosures of the full finite normalized Gaussian. |
| `fixtures.py` | Human-authored analytic expectations, including invalid-domain cases. |
| `cases.json` | Portable input/expected cases, all 63 members represented. |
| `golden.json` | Corresponding exact floating hex bits, integer results and expected error categories. |
| `run_cases.py` | Check hand expectations and immutable golden; evaluate one case or compare normalized candidate output. |
| `test_oracles.py` | Tests containing analytic vectors, IEEE edge cases, 900 MPFR rational/quadratic certificates, exhaustive and external checks. |
| `test_report.json` | Actual oracle run; counts and untested production areas. |

Default verification never rewrites golden. Regeneration is an explicit action,
and still checks all hand-derived expectations before writing:

```sh
python oracle/ops/mask_morphology/run_cases.py --emit --output /tmp/mask-golden.json
python oracle/ops/mask_morphology/run_cases.py --candidate /tmp/candidate.json
```

The candidate file has exactly the same `{"schema_version":1,"results":{...}}`
shape as `golden.json`; strict comparisons require all case IDs, field structure,
integer values and exact floating hex representations to match. Candidate output
must be produced by an independent adapter; copying golden is not an acceptance
experiment. `exact.accelerated_ok` is a numerical bound utility only: passing it
cannot certify mask bounds, site choices, exact branches, backend admission or
new accelerated registry profiles.

## Portable case format

```json
{
  "id": "example",
  "op": "threshold",
  "dtype": "float32",
  "inputs": {"input": [[0, 0.5, 1]]},
  "params": {"threshold": 0.5, "comparison": "ge"}
}
sh
python oracle/ops/mask_morphology/run_cases.py --case /tmp/case.json
```

Use `bits:XXXXXXXX` or `bits:XXXXXXXXXXXXXXXX` for exact IEEE inputs, especially
NaN, signed zero, subnormal and halfway cases. JSON numbers become actual stored
input values of the chosen dtype; decimal spelling is not an exact-decimal
semantic promise. Float64 statics and target/control values are not narrowed to
Float32. Rounding follows the 01-numeric contracts referenced by MASK-common.
`output_dtype` is separate where the member declares it. The explicit raw-byte adapter emits floating Binary; UInt8 raw inputs
must be Python/JSON integers; bool is never an integer parameter. CHW arrays are
explicit; label coordinates/counts are true integers. Component Result fields
are mathematical row arrays; `source_id` is only a synthetic association token.

The mathematical facade omits production descriptor facets, unit/basis tokens,
interpretation/override metadata and Result codecs. `dtype` is a convenience for
same-storage test inputs; spec members allowing independently typed seed/barrier
planes still require per-port type tests in the future public adapter. Defaults
in Python calls do not waive required statics in the spec. Unknown op names and
unsupported arithmetic types fail; the facade is not a schema-validation oracle.

## Certification and deliberate limits

Every rational result is rounded by integer quotient/remainder ties-to-even,
not `float32(float64(exact_expression))`. The quadratic oracle compares exact
squared expressions to representable midpoints. Gaussian constructs directed
intervals for all positive coefficients, the numerator and denominator; matching
RN_T endpoint bits certify the result. It refines up to 8192 bits and raises
`Uncertified` on failure, never chooses a guessed value. True positive Gaussian
taps remain mathematical support even when ordinary machine exp would underflow.

Manual capacity limits prevent accidental huge brute-force work: generated
footprint radius/metric-axis reach <=128, topology images<=256pixels, Gaussian
pixel*tap work<=100000, polygon pixel*n²*V<=1000000. These are **oracle limits**,
not proposed native maximum radii or guaranteed resource accounting. Caller-
controlled giant custom footprints/other inputs must also be kept small. The
reference is not a sandbox for untrusted workloads and has no host cancellation.

NaN/signed-zero branch behavior is exercised using Python floating carriers;
signaling NaN payload transport may be quieted by the host and is not certified.
Output-read omission, descriptor-only requests, source-page access, fenv flags,
true ObjectId associations, memory peaks, ABI and owner lifetime need native tests.
The restricted-mean reference validates zero-weight image samples for requested
values. A valid-only request depends on weights alone. The reference does not
instrument physical byte reads.

SciPy/skimage check matched mathematics only. Morphology comparisons explicitly
pad enough for all stages and crop once; hole checks use complementary
connectivity; EDT/Gaussian external comparisons use tolerances and do not define
strict bits or nearest-site ties. No performance or commercial compatibility
claims follow from these tests.

Grouped-model oracle objects carry ids/mean/cholesky and skipped IDs only. FMT
coordinate descriptors, native Result counts/codecs and ownership are not simulated.
Sample tables are numeric test carriers; source_description and distance_unit
controls remain mandatory in the production specifications.
