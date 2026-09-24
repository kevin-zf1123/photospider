# FMT-02 regression check after FMT-03

Measured 2026-09-24 on the same Apple M5, macOS 27.2 and Clang 21.1.3
RelWithDebInfo CPU configuration as the original FMT-02 report. One configured
worker, FP32, 128x128 tiles, preprovisioned sources, fresh materialized output,
two warmups and no completed-result cache. This check is local macOS CPU only.

## Finding and correction

The first 24-case run after FMT-03 passed every independent byte oracle but had
several slower results against historical timings. We preserved the original
FMT-02 benchmark executable (built 06:26:30; original final-native summary was
written 06:27:43) before rebuilding the current target. It has no FMT-03 helper
or scalar-provider symbols. Both executables use the unchanged FMT-02 driver,
compiler settings and public execution path. The historical CSV is context;
acceptance uses contemporaneous alternating old/new processes.

Three alternating rounds over all 24 cases followed by seven longer rounds over
five representative cases reproduced a small increase: 4096 tiled A full
18.769 -> 19.450 ms (+3.63%), C full 18.985 -> 19.407 ms (+2.22%), and A single
channel 4.243 -> 4.351 ms (+2.54%). The copy counts and memory accounting did
not change. Small C was only +0.64% under the longer test, so its earlier +5.4%
short-run difference was not treated as a stable regression.

The single-variable correction separates the scalar fill traversal from the
spatial copy traversal. `copy_spatial_channel_piece` retains the original
FMT-02 function body; `fill_scalar_channel_piece` retains the bounded exact-bit
FMT-03 fill. A single dispatch recognizes a generic one-axis fixed sample.
Metadata preparation and the executor's page/publication paths are unchanged.
Ordinary generic negative/zero-stride spatial inputs keep their previous path.

The seven-round counterfactual after this change measured A/C full at -1.04% /
-1.32% relative to the contemporaneous old executable, and A single channel at
-0.95%. This supports a shared hot-loop code-generation effect as the cause of
the earlier materialization cost. It does not identify a particular CPU
instruction or establish a universal speedup. The spatial function body is
source-identical to HEAD apart from its name. Independent code review found no
blocker/required issue in dispatch, bounds, ownership or cancellation.

## Final full matrix

The final matrix again alternates old/new order across cases and rounds, three
rounds per case. Each process checks every requested first-execution byte with
an independent oracle, outside timing. Large processes measure 15 executions;
128x128 and ROI processes measure 51. The table takes the median of the three
per-process p50s and p95s, in **microseconds**. p95 uses the driver's empirical
`floor((n-1)*.95)` convention. These are small-sample latency statistics.

| Size | Member/storage/request/layout/profile | Before p50 / p95 | Final p50 / p95 | p50 change |
| --- | --- | ---: | ---: | ---: |
| 128² | A/continuous/full/materialize/strict | 55.208 / 64.000 | 53.875 / 59.959 | -2.41% |
| 4096² | A/continuous/full/materialize/strict | 19391.200 / 20174.200 | 19240.500 / 19619.600 | -0.78% |
| 128² | A/tiled/full/materialize/strict | 53.417 / 64.917 | 56.417 / 65.333 | +5.62% |
| 4096² | A/tiled/full/materialize/strict | 19684.600 / 20105.600 | 19270.800 / 19950.200 | -2.10% |
| 4096² | A/tiled/one/materialize/strict | 4276.880 / 4516.120 | 4393.880 / 4594.750 | +2.74% |
| 4096² | A/tiled/roi/materialize/strict | 26.916 / 32.500 | 26.333 / 31.042 | -2.17% |
| 4096² | A/tiled/full/auto/strict | 18942.200 / 19879.400 | 19195.700 / 19683.800 | +1.34% |
| 4096² | A/tiled/full/materialize/native | 19187.700 / 20182.700 | 19243.200 / 20153.200 | +0.29% |
| 128² | B/continuous/full/materialize/strict | 51.417 / 55.750 | 51.583 / 56.833 | +0.32% |
| 4096² | B/continuous/full/materialize/strict | 19090.500 / 19654.500 | 18693.100 / 19046.200 | -2.08% |
| 128² | B/tiled/full/materialize/strict | 52.541 / 60.916 | 52.167 / 56.916 | -0.71% |
| 4096² | B/tiled/full/materialize/strict | 18896.300 / 19429.000 | 19120.200 / 19637.300 | +1.18% |
| 4096² | B/tiled/one/materialize/strict | 4309.420 / 4499.960 | 4448.920 / 4883.460 | +3.24% |
| 4096² | B/tiled/roi/materialize/strict | 25.583 / 32.250 | 24.291 / 29.208 | -5.05% |
| 4096² | B/tiled/full/auto/strict | 19641.800 / 20256.800 | 19389.400 / 20188.500 | -1.29% |
| 4096² | B/tiled/full/materialize/native | 19624.500 / 20153.700 | 19130.100 / 19783.500 | -2.52% |
| 128² | C/tiled/full/materialize/strict | 54.333 / 60.667 | 53.083 / 57.083 | -2.30% |
| 4096² | C/tiled/full/materialize/strict | 19226.900 / 19661.200 | 19442.800 / 20588.600 | +1.12% |
| 4096² | C/tiled/roi/auto/strict | 27.292 / 36.417 | 27.167 / 35.500 | -0.46% |
| 4096² | view/tiled/full/view/strict | 1542.960 / 1596.000 | 1563.290 / 1654.120 | +1.32% |
| 4096² | view/tiled/full/auto/strict | 1554.830 / 1609.460 | 1547.170 / 1575.330 | -0.49% |
| 4096² | view/tiled/full/materialize/strict | 20857.000 / 22288.300 | 20698.300 / 21027.000 | -0.76% |
| 4096² | view/tiled/one/view/strict | 366.250 / 386.834 | 369.208 / 395.291 | +0.81% |
| 4096² | view/tiled/roi/view/strict | 48.708 / 53.000 | 49.041 / 56.750 | +0.68% |

All 24 cases pass. The unweighted geometric mean of the 24 p50 ratios is
**-0.21%**; this is not a total-workload throughput score. Large full
materializations span **-2.52% to +1.34%** in this final matrix. Per-case paired
changes can have different signs because the machine is not a controlled
quiescent performance lab. In particular, isolated ROI p95 excursions should
not be interpreted as a persistent tail-latency guarantee.

The largest positive short-run median change, 128 A tiled (+5.62%), received
seven additional alternating rounds with **1001 measured executions per process**.
It became **52.625 -> 53.083 us (+0.87%, +0.458 us)**, p95 61.875 -> 62.750 us;
individual paired p50 changes ranged -1.85% to +3.07%. It did not reproduce the
short-run +5.6% result. The final evidence supports **no sustained material
FMT-02 performance regression** at this workload/hardware scope, while retaining
these sub-microsecond differences and noise in the record.

For every final case, logical source bytes, copied bytes, output backed bytes,
output metadata, output virtual span and modeled peak bytes have **zero delta**.
These are capacity/accounting measurements, not RSS. View ownership, exact
requested coverage and scalar fill remain covered by the integration oracles.

## FMT-03 and correctness recheck

After separating the copy paths, all 45 FMT-03 cases pass again. Its
4096/tiled/fill/full/materialize strict p50 is **19.829 ms**, compared with
20.541 ms before isolation and 49.973 ms before the original scalar optimization.
Seven focused CTests pass: channel assembly, editing, extraction, planar workflow,
compiler, Value and execution demand. Both FMT-02 and FMT-03 installed-package
consumers also pass after reinstalling the final library. ClangFormat 21, cpplint
and diff checks pass.
No full CTest, sanitizer, Windows/WSL or GPU performance run was added.

## Reproduction and retained evidence

```sh
cmake --build build --target photospider_channel_assembly_performance -j 8
python3 examples/channel_assembly_performance/run.py \
  build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  build/fmt02-regression/new-current

# Requires the retained pre-FMT-03 executable under build/fmt02-regression/.
python3 build/fmt02-regression/paired.py another-comparison
python3 build/fmt02-regression/focused.py another-focused-comparison
```

Artifacts under `build/fmt02-regression/`:

- `before-fmt03`: preserved original FMT-02 executable, not the newly rebuilt one.
- `after-matrix/`: initial current matrix compared to historical data.
- `paired-raw.csv`, `paired-summary.csv`: first three-round comparison.
- `focused/raw.csv`: seven-round confirmation before path isolation.
- `isolated/raw.csv`: seven-round single-variable isolation experiment.
- `final/paired-raw.csv`, `final/paired-summary.csv`: final 24-case comparison,
  with every process's CSV and raw-sample log.
- `small-a-confirm/raw.csv`: seven-round 1001-execution small-A confirmation.
- `paired.py`, `focused.py`, `small_a.py`: the exact serial orchestration scripts.

The final FMT-03 rerun is
`build/fmt03-performance/after-fmt02-regression/summary.csv`, with all 45 case
logs/CSVs. The original `final-native-45/` remains the pre-isolation evidence;
these two revisions are not silently mixed. Generated artifacts remain ignored.
