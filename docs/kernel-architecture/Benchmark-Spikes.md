# Benchmark Spikes

These benchmark spikes must be completed before tightening Metal context access
policy or changing the portable CPU row-alignment requirement.

## Metal Adapter Access

Compare Metal adapter APIs against direct backend `context` interpretation for:

- 16x16 and 64x64 tiles
- lightweight operations such as copy, add, and curve transform
- upload/download-heavy workloads
- mixed CPU/GPU scheduler paths where adapter overhead can affect RT latency

Until this benchmark has data, adapter APIs are the recommended path and direct
`context` access remains an implementation-sensitive escape hatch.

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
