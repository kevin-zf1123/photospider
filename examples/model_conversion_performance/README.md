# FMT-11 performance workflow

这是性能分析入口，不是独立正确性 oracle。先运行 `test_model_math` 和
`test_model_conversion`。构建目标为 `photospider_model_conversion_performance`；
可执行文件位于构建目录的 `examples/model_conversion_performance/`。

```sh
cmake --build build-fmt11 --target photospider_model_conversion_performance
B=build-fmt11/examples/model_conversion_performance/photospider_model_conversion_performance
$B --help
$B --member M --dtype f32 --profile strict --algorithm auto --mode math --width 1024 --height 1 --repeats 7
$B --member M --dtype f32 --profile strict --algorithm reference --mode math --width 1024 --height 1 --repeats 7
$B --member M --dtype f64 --profile x86 --algorithm auto --mode planar --width 133 --height 2 --repeats 7
$B --member M --dtype f64 --profile x86 --algorithm scalar --mode planar --width 133 --height 2 --repeats 7
$B --member E --dtype f32 --profile strict --algorithm auto --mode planar --width 1024 --height 128 --roi-width 7 --repeats 7
```

macOS Apple Silicon 将 `--profile x86` 改为 `--profile apple`。不支持的加速 profile
明确失败，不会将 scalar 结果标成成功的 AVX2/NEON 结果。FreeBSD arm64 上 `apple`
profile 仍不适用；应测试 strict，新增 NEON 候选路径目前遵循既有 Apple profile 的平台限制。

`--member A..T` 对应规格成员。S 的图执行使用公开 helper 所采用的 MASK 构件，
没有 `color.gray_to_black_white` 原生注册项。所有 benchmark 图使用 raw 及 materialize，
不测语义元数据检查成本，也不测 Q 的 view 路径。

三种 mode：`math` 直接测数值核心，单线程，另报常量准备时间；`generic` 测 Value 图；
`planar` 测 tiled PlanarImage 图。后两者单独报告编译时间，重复复用同一执行计划和输入，
每轮生成新输出；计时不包含输入导入或输出全量导出，包含执行阶段真实分配和调度。
`--roi-width` 只请求左侧窄条，不是全图计算后裁剪。跨通道稀疏/非连续区域由集成测试覆盖。

`auto` 使用适用的滤证算法及新增矩阵 SIMD 候选；`scalar` 关闭新增矩阵 SIMD 候选，
仍使用同一滤证算法及底层 NUM/SLEEF（它们自身可能用 ISA）；`reference` 关闭这些快速滤证，
使用精确有理数/定向区间路径。它不是错误舍入的普通 libm 对照。

CSV 中 accepted/reference/refinements/math_work 仅在 math 模式有效；图模式这些列为 0，
不是“无回退/无工作”。`--warmup N` 默认为 2，允许 0；预热不计入计时、数学计数、work、copy/tiles
或 checksum。计数累计所有正式 repeats；median/min/max/p90 是单轮时间统计，
偶数轮的 median 取中间两项均值，p90 使用 nearest-rank。
`timings_us` 保留用分号分隔的逐轮原始时间。图模式在执行计时结束后，按逻辑坐标
摘要 ROI 内**全部输出位**；math 模式保持在数值循环内累计 checksum。
全量摘要可以发现原先首样本摘要遗漏的差异，但哈希相等不是数学正确性的证明；
仍需独立 oracle 与回归。读取输出也会影响下一轮缓存状态，因此前后比较必须使用
同一新版 harness、参数、warmup 和 repeats，不能直接混比旧版首像素结果。
`peak_buffer_bytes` 是框架 buffer 模型峰值，不是完整 managed arena、committed backing 或 RSS。
图中 `source_read_bytes=0` 可以表示直接绑定预先存在的 Value/image，不代表没有读样本。

先用小尺寸测 E/F/D/H/J/L reference，留意 `--work` 的显式证明工作上限；不要拿
ResourceExhausted 当作性能结果或通过降低精度绕过。大图比较应保持相同输入、ROI、tile、
profile、线程数和工作预算。宽度建议 1/2/3/4/7/63/64/65/127/128/129/133/1920/3840；
测试 tile 64/128/256、worker 1/2/物理核数、Float32/Float64。

严格的速度结论需在空闲实机独立重复进程、记录编译器/flags/CPU/OS/温控状态，
排除并发构建。macOS 用 Instruments Time Profiler/Allocations、`/usr/bin/time -l`；
FreeBSD 用 pmcstat（已配置 PMC 时）、DTrace、`/usr/bin/time -l`。关注 SIMD 占比、
证明回退/GCD/矩阵求逆准备、每 tile 一次的大 scratch 分配、row_run 与依赖元数据成本。

## 可重复的前后对比

`tools/fmt11/compare_performance.py` 接受 `--baseline` 和 `--candidate` 两个可执行
文件；`--accelerated-profile apple|x86` 选择加速 profile，Darwin arm64 默认 apple，
其他平台默认 x86；场景名称同步使用所选 profile。两者必须由**相同本文件 harness**、相同编译器/flags，分别链接修改前后
数值实现得到。脚本执行至少三对独立进程，固定随机种子交错 AB/BA，支持 Linux
CPU affinity，并用本地文件锁防止该脚本自身并发运行（不能排除其他用户进程）。
原始 JSONL 保存命令、stdout/stderr、退出码、每轮时间及数值/工作量计数；汇总
保存每对进程 median、整体 median、极值与 speedup。失败和 checksum/work 不一致
均导致非零退出。测试期间不要同时编译。

先检查参考路径与 Float64 的 GCD 成本，再比较 strict/x86 的大图/窄 ROI、worker
以及 tile。SIMD 命中不等于端到端更快；小 ROI、任务调度与线程争用可能主导。
不能把历史机器的单次结果和本轮 Linux 热态多次结果拼成同一提速比。
