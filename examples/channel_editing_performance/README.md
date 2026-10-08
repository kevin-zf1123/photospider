# FMT-03 CPU performance and optimization

## Current Result driver

```sh
cmake --build build --target photospider_channel_editing_performance -j 8
./build/examples/channel_editing_performance/photospider_channel_editing_performance \
  130 fill tiled roi materialize 1 strict
```

## Full Result matrix

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S examples/channel_editing_performance -B build/channel-editing-performance-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/channel-editing-performance-consumer --target photospider_channel_editing_performance -j 8
```

```sh
cmake --build build --target photospider_channel_editing_performance -j 8
python3 examples/channel_editing_performance/run.py \
  build/examples/channel_editing_performance/photospider_channel_editing_performance \
  build/fmt03-performance/current
```

The binary accepts `size member continuous|tiled full|one|roi
layout repetitions profile`. `member` is A, B, repeat, fill, subset, insert or
identity. All workloads are **FP32**. The 45-case serial matrix includes the
required 128x128, 4096x4096 continuous and 4096x4096 tiled cases, strict and native
Apple Silicon profiles, materialize/auto/view, single-channel and cross-tile ROI.
