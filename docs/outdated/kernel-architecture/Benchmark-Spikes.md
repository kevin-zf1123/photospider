# Archived Benchmark Spikes

> Archived on 2026-07-14. These questions did not produce an architectural
> decision and therefore are not current kernel documentation. Follow-up work
> is tracked by the generic-data and heterogeneous-execution project.

These benchmark spikes must be completed before tightening Metal context access
policy or changing the portable CPU row-alignment requirement.

## Metal Adapter Access

The Metal buffer adapter is an implemented but currently unenabled candidate.
Before enabling it as a production runtime boundary, compare its APIs against
the current backend-specific Metal operation path and direct backend `context`
interpretation for:

- 16x16 and 64x64 tiles
- lightweight operations such as copy, add, and curve transform
- upload/download-heavy workloads
- mixed CPU/GPU scheduler paths where adapter overhead can affect RT latency

Until the adapter is wired into the build and this benchmark has data, do not
describe adapter APIs as the production Metal path. Direct `context` access
remains backend-specific and implementation-sensitive.

## ARM Mac Alignment

Compare the required 64-byte row alignment with an optional 128-byte alignment
mode on Apple Silicon for:

- row-wise SIMD kernels
- tiled HP operations
- RT tile updates
- memory usage and padding overhead

No benchmark outcome has been recorded yet. A follow-up OpenSpec change should
capture implementation details, scripts, raw results, and the decision on
whether 128-byte alignment becomes configurable or remains only an experiment.
