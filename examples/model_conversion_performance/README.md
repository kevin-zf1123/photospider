# FMT-11 performance workflow

当前驱动提供直接数学模式和两种 Result workflow 模式。`math` 直接调用 private `ModelMath` 与 SIMD 实现；它不是 installed public math consumer。`generic` 和 `planar` 通过 public Result graph 编译与执行，`planar` 使用 spatial Result storage。历史 Value/planar 测量不代表当前 Result 实现的性能。

```sh
cmake --build build --target photospider_model_conversion_performance
B=build/examples/model_conversion_performance/photospider_model_conversion_performance
$B --help
$B --member M --dtype f32 --profile strict --algorithm auto --mode generic --width 133 --height 2 --repeats 3 --warmup 2
$B --member M --dtype f64 --profile x86 --algorithm reference --mode planar --width 133 --height 2 --roi-width 7 --repeats 3 --warmup 2
$B --member M --dtype f32 --profile apple --algorithm auto --mode math --width 133 --height 2 --repeats 3 --warmup 2
```

`--member` 接受 A..T；`--dtype` 接受 f32/f64；`--profile` 接受 strict、x86 或 apple；`--algorithm` 接受 auto、scalar 或 reference；`--mode` 接受 math、generic 或 planar。尺寸、ROI、重复次数、预热次数、worker 数、tile 大小与证明工作上限分别由 `--width`、`--height`、`--roi-width`、`--repeats`、`--warmup`、`--workers`、`--tile` 和 `--work` 控制。`auto` 使用 profile 准许的 SIMD 候选并逐 lane 进行数值 certification；`scalar` 关闭批量 SIMD，但仍执行相同的 certification；`reference` 关闭快速滤证路径，使用 reference 运算。证明 work 达到 `--work` 上限时，运行失败，不应作为性能结果。比较不同实现时应固定编译 flags、参数、warmup 与 repeats。

`prepare_us` 记录 math 常量准备时间，`compile_us` 记录图编译时间。Graph 模式的 `median_us`、`min_us`、`max_us`、`p90_us` 和 `timings_us` 仅计时 `execute` 调用；完整 ROI 输出的 bit checksum 在计时结束后计算。Math 模式则直接计时数值循环，并在循环内累计 checksum。checksum 用于跨运行一致性检查，不是独立正确性 oracle。预热运行不进入正式计时与计数；数学计数、Result work、source logical bytes 和 numeric diagnostics 按正式 repeats 累加。

CSV 包含当前输入参数、prepare/compile 与计时列、`accepted`、`reference`、`refinements`、`math_work`、`root_peak_host_bytes`、`run_live_payload_bytes`、`run_live_metadata_bytes`、`source_payload_bytes`、`source_logical_bytes`、`issued_work`、`numeric_evaluated`、`numeric_copied`、`numeric_views`、`strict_math_calls`、`strict_fallbacks` 和 `checksum` 等字段。`accepted`、`reference`、`refinements`、`math_work` 仅在 math 模式填写；Root 与 numeric 字段仅在 Result graph 模式填写，不适用的列为空。

在 Result graph 模式中，`source_payload_bytes` 是执行前 Root 中 source payload 的基线。`source_logical_bytes` 是各正式 repeat 请求的来源逻辑 support 大小，按 dtype 字节数计算，不代表实际内存流量。`issued_work` 与 `numeric_*` 字段累计所有正式 repeats。`run_live_payload_bytes` 与 `run_live_metadata_bytes` 分别记录 Result 存活期间相对执行前基线的最大正增量；`root_peak_host_bytes` 是 Root 累计 Host 峰值，包含 setup、warmup 与 execute 计时外的输出读取。驱动显式设置 Root Host 容量为 8 GiB、Metadata 子限额为 64 MiB；这是本例压力配置，不是运行时默认值。

`tools/fmt11/compare_performance.py` 使用 `--baseline` 和 `--candidate` 指定两个兼容的驱动可执行文件，按随机化 AB/BA 顺序运行至少三对独立进程。图模式比较完整输出 checksum；math 模式还比较 `accepted`、`reference`、`refinements` 和 `math_work`。脚本支持 Linux CPU affinity，并用本地文件锁避免该脚本的并发运行。历史 graph CSV 中的零计数与当前不适用字段的空值可以共存；graph 模式只比较 checksum。

当前验证限于一致性 smoke：160 次串行 graph 执行覆盖 A–T、Float32/Float64、generic/planar 和 auto/reference，尺寸为 width 3、height 2，repeats 1、warmup 0；40 组中每组四种组合的 checksum 一致。另有三次 Apple profile 的 NCL M、Float32、width 133、height 2 执行，math、generic 和 planar 模式均通过。它们不提供独立 golden oracle 或速度结论，完整性能矩阵尚未运行。六项 focused 检查全部通过，包括 model conversion、model math、metadata、rational arithmetic、alpha/model interop 和 Result 行为覆盖。安装包消费检查 `installed_model_conversion`、`installed_model_result` 和 `installed_alpha_model_interop` 也全部通过。
