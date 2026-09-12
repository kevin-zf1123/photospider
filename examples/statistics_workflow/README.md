# Paged integer statistics workflow

This installed-public-API example registers `make_statistics_operation` and
executes source → histogram → global parameters → grade → streaming sink.
The sink requests certified prefixes while grade is active and checks every
pixel. Duplicate histogram nodes must start once and return the same ObjectId
with the optional cache disabled. No private kernel headers are used.

```sh
cmake --build build/issue257-shared --target photospider_statistics_workflow -j 8
build/issue257-shared/examples/statistics_workflow/photospider_statistics_workflow
```

An isolated consumer of an installed package:

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-delivery/install"
cmake -S examples/statistics_workflow -B out/phase-a-delivery/statistics-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-delivery/install"
cmake --build out/phase-a-delivery/statistics-consumer -j 8
out/phase-a-delivery/statistics-consumer/photospider_statistics_workflow
```

## Contract and independent reference

Inputs are facet-free Int64 HW values and UInt8 HW masks. Any nonzero mask
selects a sample; selected values must lie in `[0,bins)`. Bin `j` denotes
`[j,j+1)`. This deliberately narrow scalar profile performs no color conversion.
The mathematical bin domain is fixed, but only positive counts are stored,
ordered by bin ID. Missing bins are zero only after the histogram seals.

The example uses `x[i]=(3*i+1)%8`, target 2, and small exact integer sums.
Its map-based histogram reference is independent of the dense-counter product
implementation; its pixel reference uses long double evaluation of the rational
expression. The pixel tolerance is **measured fixture agreement**, not a
CertifiedBound. The declared product order is nearest binary64 mean, then
`gain=target/mean`, then `gain*double(x[i])`.

This Python standard-library reference can be run separately:

```sh
python3 - <<'PY'
from collections import Counter
from fractions import Fraction
for n in (1, 17, 1003, 65537):
    values = [(3*i+1)%8 for i in range(n)]
    counts = Counter(values)
    mean = Fraction(sum(values), n)
    print(n, sorted(counts.items()), 'total=', sum(values),
          'mean=', float(mean).hex(),
          'first_pixels=', [float(Fraction(2*x, 1)/mean) for x in values[:8]])
PY
```

`test_statistics` additionally checks binary64 hexadecimal golden values derived
from Python `Fraction`, including cases where separately converting large
integers to double before division gives the wrong answer.

The executable covers counts 1/17/1003, page windows 64/256/4096 bytes, partial
and empty masks, valid zero-mean statistics, invalid selected samples, ignored
masked-out samples, changed source snapshots, undersized pages, exhausted work
and host capacity, the 65,536-bin multipass recipe, disk exhaustion after a field is written, cancellation after
histogram publication, cross-row HW, 257-bin paged output, and 65,537 logical
input samples. Failed runs assert that Disk and Payload accounting returns to zero. It checks result and window
ownership after `ExecutionContext` destruction and final mandatory disk release.
Empty statistics have `count=total=0, valid=0`; all-zero selected values have
`count>0, total=0, valid=1`. Both fail explicitly when asked to grade.

Resource accounting is the product's managed-capacity ledger, not a process RSS
hard bound. The `bin_ranges_512` recipe uses admitted `min(bins,512)*8` Payload
bytes and rereads the immutable source once per 512-bin range. Large bin domains
therefore have a concrete bounded-memory path; extra passes may exhaust work. Source and output fields use windows no larger
than 4096 bytes, and mandatory result backing is paged. Dependency relations
remain Conservative and retain descriptor observations for empty collections.
