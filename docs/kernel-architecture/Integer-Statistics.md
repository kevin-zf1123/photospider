# Integer histogram and global grade

Package 0.10 exposes `statistics_schema`, `statistics_mean`, and
`make_statistics_operation` through the installed C++ API. The C operation ABI
remains 9. These CPU stages use structured protocol 2, managed root admission,
mandatory temporary backing and existing shared Result lifetimes.

## Schema and success domain

`StatisticsSpec{height,width,bins}` requires positive HW dimensions and 1..65536
bins. The raster sample count is at most `(INT64_MAX-4095)/8`, so encoded scalar
backing offsets fit the temporary-storage contract. Input Values are facet-free
Int64 HW and UInt8 HW; mask values other than zero select samples. Selected
integers must be in `[0,bins)`. There is no implicit color, luminance or transfer
conversion. Masked-out samples may lie outside the bin domain.

Version-one schemas have a canonical `integer_statistics_v1` metadata facet
containing profile 1, H, W and bins as four little-endian UInt64 words.

| Schema | Publication and fields |
| --- | --- |
| `photospider.integer_histogram` | CompleteBundle; RuntimeCount Int64 `bin`, FieldRows Int64 `count` |
| `photospider.integer_statistics` | CompleteBundle; one Int64[3] `count_total_valid`, one Float64 `mean` |
| `photospider.graded_scalar` | StablePrefix; fixed H*W Float64 `pixels`, HW domain |

The fixed mathematical histogram has bins `[j,j+1)`. Its sparse encoding stores
only positive counts with strictly increasing bin IDs, including bin zero when
selected zero-valued samples exist. Runtime row count is the number of nonzero
bins, not the number of selected samples. Missing bins mean zero only after
seal. Parameters validates all imported rows, rejects duplicate/out-of-range
IDs and nonpositive counts, and checks both integer arithmetic and total sample
count against HW. Producers using these public factories validate the semantic
contents; arbitrary third-party publishers remain responsible for their
registered schema's semantic obligations.

`D=sum(count)` and `T=sum(bin*count)` use checked nonnegative Int64 arithmetic.
An empty selection emits `[0,0,0]` and a nonsemantic zero mean placeholder. A
nonempty all-zero selection emits `[D,0,1]`. For D>0, mean is the correctly
rounded binary64 value of the exact rational T/D. Integer long division extracts
53 significant bits and uses remainder comparison for ties-to-even; converting
T and D separately to double before division is not equivalent. Public
`statistics_mean` also accepts the full nonnegative Int64 numerator/positive
Int64 denominator domain where T/D<65536.

`statistics.grade` requires a finite nonnegative Float64 `target` parameter,
complete consistent parameters, valid=1 and mean>0. It checks the exact integer
ratio against bins-1 before relying on the rounded mean. Operations are
`gain=round64(target/mean)`, then
`y[i]=round64(gain*round64(Int64(x[i])))`, nearest ties-to-even, gradual
underflow, no contraction. The floating environment is restored. Nonfinite
gain or output returns ArithmeticOverflow; empty, zero-mean or inconsistent
parameters return InvalidDomain. No approximate result replaces these failures
and no CertifiedBound is implied.

## Dependencies, paging and lifetime

Histogram scans the full selected domain before any publication. Parameters
waits for the complete histogram. Both use Conservative Cartesian support over
their inputs, including the mask's complete validation/control domain. Result
descriptor observations are retained separately at reserved `[0,1)`, so an
empty collection still has count/descriptor dependencies.

Grade validates complete global parameters before any prefix. Its relation
unites compact identity support for each source pixel with one shared global
parameter support expression and the parameter descriptor. The same relation
owner covers all prefixes; no per-pixel global dependency vector is allocated.
The union is Conservative. A later pixel or operational failure leaves already
certified prefix ranges immutable; it does not manufacture a complete image.

The named `bin_ranges_512` recipe reserves min(B,512) Int64 Payload counters
before allocation. It rereads the immutable source snapshot once per 512-bin
range, appends positive counts in increasing global bin order, and seals only
after every range. This is the Design's bin-range multipass fallback, with work
O(N*ceil(B/512)+B) and at most 4096 resident counter bytes. Initialization,
reset/scanning and every pass consume root work. Extra passes may exhaust the
work budget; no full B-counter allocation or unaccounted input spool is needed.
Histogram source strips stop at HW row boundaries and use at most 4096 bytes
per field, independently of the selected Result I/O window. Value requests are
admitted by the root capacity budget; a 24-byte Result window does not force
three source samples per poll on every bin-range pass. Histogram output,
parameter input and grade output pages use min(user Result window,4096).
The 24-byte
parameter record is indivisible and fails when the selected window is smaller.
Grade writes bounded Float64 windows to mandatory backing and yields stable
prefixes to active downstream consumers. Stage/work/capacity exhaustion remains
a distinct resource failure. Physical page geometry is absent from semantic
schema identity and shared-result keys.

Result associations retain predecessor ObjectIds and their backing. Reading a
window retains this ownership after the result and ExecutionContext are gone.
The final result/window release reclaims mandatory backing. Optional cache-off
duplicate DAG expressions share one producer; separate Run input snapshots do
not alias their global parameter results. These are managed-capacity guarantees,
not allocator/driver/process RSS hard bounds.

## Executable acceptance

[statistics_workflow](../../examples/statistics_workflow/README.md) contains
source → histogram → parameters → grade → active paged sink, standalone install
consumer commands and a separate Python Fraction/Counter reference. It checks
every pixel, dynamic/empty counts, multipage histograms, cross-row HW reads,
small windows and budgets, cache-off aliases, different Run snapshots and
post-context ownership. `test_statistics` verifies exact binary64 rounding and
schema rejection; `test_statistics_callback` drives the public parameter
callback with malformed external sparse records, integer overflow and a large
integer rational golden case. Fixture pixel tolerances are measured agreement,
not certified bounds. The `--large` workflow checks 200x200 samples, 65536 bins
and a 24-byte Result window at the existing one-million-stage producer cap;
a smaller regression fixes the producer envelope at 5000 stages. All source
callbacks assert the independent 4096-byte strip cap.
