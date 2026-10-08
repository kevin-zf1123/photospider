# FMT-11 performance workflow

当前驱动提供直接数学模式和两种 Result workflow 模式。

`math` 直接调用 private `ModelMath` 与 SIMD 实现；它不是 installed public math consumer。

`generic` 和 `planar` 通过 public Result graph 编译与执行，`planar` 使用 spatial Result storage。

```sh
cmake --build build --target photospider_model_conversion_performance
B=build/examples/model_conversion_performance/photospider_model_conversion_performance
$B --help
$B --member M --dtype f32 --profile strict --algorithm auto --mode generic --width 133 --height 2 --repeats 3 --warmup 2
$B --member M --dtype f64 --profile x86 --algorithm reference --mode planar --width 133 --height 2 --roi-width 7 --repeats 3 --warmup 2
$B --member M --dtype f32 --profile apple --algorithm auto --mode math --width 133 --height 2 --repeats 3 --warmup 2
```

`--member` 接受 A..T；`--dtype` 接受 f32/f64；`--profile` 接受 strict、x86 或 apple；`--algorithm` 接受 auto、scalar 或 reference；`--mode` 接受 math、generic 或 planar。`auto` 使用 profile 准许的 SIMD 候选并逐 lane 进行数值 certification；`scalar` 关闭批量 SIMD，但仍执行相同的 certification；`reference` 关闭快速滤证路径，使用 reference 运算。证明 work 达到 `--work` 上限时，运行失败，不应作为性能结果。
