FMT-09 encode/decode use Result operation ABI 2, and the current benchmark driver
uses the public Result API.

Current Result performance has no full
matrix conclusion.

## Current Result driver

```sh
cmake --build build --target photospider_transfer_performance -j4
B=build/examples/transfer_performance/photospider_transfer_performance
$B 256 srgb encode f32 strict tiled full 5 128 1 respect palette unmanaged
$B 258 hlg_oetf decode f64 strict tiled roi 1 256 1 respect sweep managed
```

The positional arguments are `size`, `curve`, `direction`, `dtype`, `profile`,
`storage`, `coverage`, `repeats`, `tile`, `workers`, `mode`, `corpus`, and
`budget`. Size is 1..4096; curves are `linear`, `power_gamma`, `power_gamma2`,
`srgb`, `bt709`, `bt2020`, `bt2020_10`, `bt2020_12`, `bt1886`, `pq`,
`hlg_oetf`, `acescc`, or `acescct`. Direction is `encode|decode`, dtype is
`f32|f64`, profile is `strict|x86_64|apple_silicon`, storage is
`generic|tiled|continuous`, coverage is `full|r|alpha|roi`, repeats is 1..1000,
tile is 128 or 256, and workers is 1..64. Mode is `respect|raw`, corpus is
`palette|sweep|file:<path>`, and budget is `unmanaged|managed`. A profile that
cannot run on the host fails explicitly. The `roi` case requests a 3x3 R-channel
region crossing the tile boundary and requires `size >= tile+2`.
