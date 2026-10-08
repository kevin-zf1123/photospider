# FMT-02 CPU performance and profiling

## Current Result driver

```sh
cmake --build build --target photospider_channel_assembly_performance -j 8
./build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  130 A tiled roi materialize 1 strict
```

## Full Result matrix

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S examples/channel_assembly_performance -B build/channel-assembly-performance-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/channel-assembly-performance-consumer --target photospider_channel_assembly_performance -j 8
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --target photospider_channel_assembly_performance -j 8
python3 examples/channel_assembly_performance/run.py \
  build/examples/channel_assembly_performance/photospider_channel_assembly_performance \
  build/fmt02-performance/current
```

The binary accepts `size A|B|C|view continuous|tiled full|one|roi
layout repetitions profile`. All workloads are **Float32**. `A` assembles four
independently owned component planes, `B` concatenates RGB plus alpha, and `C`
maps one RGBA input as [2,0,2,3], reusing one source and omitting channel 1.
`view` runs FMT-01 split followed by FMT-02 assembly from a common root owner.
`one` selects output channel 2. `roi` requests all four output channels in
[127,130) x [127,130), crossing four 128x128 tiles. ROI requires size >=130.
