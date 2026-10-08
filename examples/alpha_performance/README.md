# FMT-04 / FMT-05 Result performance driver

The driver builds inputs and a workflow through the public Result API, compiles it, executes it repeatedly, and checks requested output samples through Result read windows. Input construction is reported in `setup_us`; oracle checks are outside the timing fields. The first execution is reported separately. It is the first warmup; one additional warmup precedes the repeated timing samples.

```sh
cmake --build build --target test_alpha_operations photospider_alpha_performance -j 8
ctest --test-dir build -R '^test_alpha_operations$' --output-on-failure
python3 examples/alpha_performance/run.py \
  build/examples/alpha_performance/photospider_alpha_performance \
  --output alpha-smoke.csv --log alpha-smoke.log \
  --profile strict --workers 1 --managed off --repetitions 1
```

The positional argument order is `size member storage request algorithm dtype repetitions profile layout workers distribution managed`. `member` is `associate / unassociate / set / extract / remove / opaque`; `storage` is `generic / continuous / tiled`; `request` is `full / red / alpha / roi`; `algorithm` is `auto / scalar / simd / reference`; `dtype` is `f32 / f64`; `profile` is `strict / accelerated_apple_silicon / accelerated_x86_64`; `layout` is `auto / view / materialize`; `distribution` is `half / opaque / mixed / small`; and `managed` is `off / on`. The binary preserves the CLI ordering and option names.

`run.py` defaults to its own multi-case smoke set: sizes 17 and 130, all three storage modes and six members, with the available algorithms. `--matrix` expands the set to sizes 1, 17, 130, 512, and 2048, both dtypes, multiple requests and algorithms. The matrix has no fixed short runtime. A hand-selected smoke run is a separate selection and must not be reported as the `run.py` default suite.

The CSV preserves the argument and timing fields and reports dependency support and Root resource counters:

```text
size,member,storage,request,algorithm,dtype,profile,layout,workers,distribution,repetitions,setup_us,compile_us,first_us,p50_us,p95_us,callback_p50_us,source_logical_bytes,source_payload_bytes,source_metadata_bytes,run_live_payload_bytes,run_live_metadata_bytes,root_peak_payload_bytes,root_peak_metadata_bytes,managed,issued_work,managed_peak_host,managed_peak_metadata,managed_peak_referenced
```

`source_logical_bytes` is the logical element count in `run.dependencies.source_support` for the `input` port, multiplied by the selected dtype width (4 or 8 bytes). It describes requested logical support, not memory traffic. `source_payload_bytes` and `source_metadata_bytes` are Root live-resource baselines captured before execution. `run_live_payload_bytes` and `run_live_metadata_bytes` are the positive differences above that baseline while the Result remains alive. `root_peak_payload_bytes` and `root_peak_metadata_bytes` are cumulative Root peaks; they include setup, earlier runs and oracle windows. They are not isolated output sizes or process RSS.

The driver always enforces Root capacities of 2 GiB for Host and 64 MiB for Metadata. `managed=off` suppresses optional managed counters only; `issued_work` and `managed_peak_*` are blank while Root limits remain enforced. `managed=on` reports those counters. Work counts are cumulative over the process, including warmups.

## Repeatable before/after comparisons

`compare.py` runs each case in before/after/after/before order. It performs first-run output verification, two warmups and 11 measured repetitions per process by default. Its `core`, `extended` and `latency` suites contain 8, 38 and 11 configurations respectively; extended and latency therefore launch 152 and 44 processes. The summary reports the median of each variant's process-level p50 values. It is not a pooled percentile or confidence interval. `raw.csv`, `commands.log`, and `environment.json` preserve per-process observations, commands, binary hashes and host details; the script also writes `summary.csv`.

Use a new output directory for each comparison. The script takes `/tmp/photospider-performance.lock` with a nonblocking exclusive POSIX lock and checks for visible compiler/linker activity before and between cases. Other builds and measurements should honor the same lock. The lock does not isolate GUI/background workloads or control clock frequency and temperature.

```sh
python3 examples/alpha_performance/compare.py \
  --before /path/to/before/photospider_alpha_performance \
  --after /path/to/after/photospider_alpha_performance \
  --output alpha-abba --suite extended --workers 1 --repetitions 11
# FreeBSD: add --launcher 'cpuset -l 0'. Linux: use --cpu <allowed CPU> where supported.
# macOS: omit affinity and record power, thermal, and system-activity observations.
```

The comparison script unions columns from both binaries and leaves fields blank when one binary does not provide them. A blank is missing data, not a zero counter or evidence that the metrics have the same meaning. `issued_work_equal` is blank (unknown) when either row has `managed=off` or lacks an `issued_work` value; it compares values only when every row reports managed work.

Fourteen hand-selected small Result performance smoke cases passed their output oracle, including Apple Silicon named SIMD, Float64 reference, and zero-copy Set/extract/remove observations. The default `run.py` suite and the full performance matrix were not run. The focused CTest set passed, including `test_alpha_operations` (17 operation groups) and `test_alpha_authoring`; the installed alpha operations consumer passed, and the standalone performance consumer configured and built. These results do not establish full-matrix or cross-platform performance.

The [2026-09-26 native review](NATIVE_REVIEW_2026-09-26.md) is a historical report for the earlier implementation. Its Value/planar measurements are not evidence for the current Result path. Preserve that report as historical data; do not merge its timings with the Result metrics above.
