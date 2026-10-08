## 当前 Result 驱动

```sh
cmake --build build/fmt10 --target photospider_fmt10_performance -j 2
EXE=build/fmt10/examples/rgb_basis_performance/photospider_fmt10_performance
"$EXE" --help
"$EXE" 130 f32 a strict cross 1 planar 128 1 materialize respect 0 3
```
