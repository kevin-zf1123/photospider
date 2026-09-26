# FMT-04 / FMT-05 CPU 性能与交接入口

本工具使用公开 workflow/compile/execute API；先建输入、建图、编译，再复用编译计划。
计时范围不包含输入生成和结果校验。每个命令第一轮逐位验证全部请求样本，保留两轮热身，输出后续延迟的 p50/p95、回调累计时间、源读取字节、结果复制字节和受控内存峰值。stderr 保留各轮原始耗时。

```sh
cmake -S . -B build-alpha -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=ON
cmake --build build-alpha --target test_alpha_operations photospider_alpha_performance -j 2
./build-alpha/test_alpha_operations
python3 tests/support/generate_alpha_golden.py --check
python3 examples/alpha_performance/run.py \
  build-alpha/examples/alpha_performance/photospider_alpha_performance \
  --output alpha-smoke.csv --log alpha-smoke.log
```

构建要求沿用仓库：将用户提供的 SLEEF 3.9.0 源码放在 `third_party/sleef`，不是再次嵌套一层 `sleef-3.9.0/`。Alpha 乘除本身不调用 SLEEF 超越函数；仓库其他 NUM/CRV 算子仍需要该依赖。

## 单次命令

```sh
build-alpha/examples/alpha_performance/photospider_alpha_performance \
  512 associate tiled full simd f32 11 strict auto 1 mixed
```

位置参数：`size member storage request algorithm dtype repetitions profile layout workers distribution [managed]`。

- `member`：`associate / unassociate / set / extract / remove / opaque`。
- `storage`：`generic / continuous / tiled`；默认 128×128 tile。
- `request`：`full / red / alpha / roi`。ROI 从坐标 127 开始，测试边界；小图自动限制在范围内。提取输出保留 singleton channel axis；其 red/alpha 都指唯一输出通道。Remove 不接受 alpha 请求。
- `algorithm`：`auto / scalar / simd / reference`。只影响 FMT-04。Strict auto 采用标量；命名加速配置 auto 采用本机 SIMD；reference 为整数精确乘除与舍入。
- `dtype`：`f32 / f64`。B/C 的七 dtype 位拷贝另由集成测试覆盖。
- `profile`：`strict / accelerated_apple_silicon / accelerated_x86_64`。命名配置不匹配平台会失败，不伪装成加速。
- `layout`：FMT-05 的 `auto / view / materialize`；FMT-04 始终物化。本工具的 set 是已有内部 alpha 的 identity 赋值，不覆盖所有外部输入类型。
- `workers`：执行上下文 worker 数。当前单个 planar 算子不会因为增加 worker 自动拆成私有并行循环；测试 1/2/4 workers 主要用于判断宿主排队成本，不能当作算子并行伸缩性证明。
- `distribution`：`half / opaque / mixed / small`；small 是 2^-20。真实 subnormal、NaN payload、溢出等由独立 correctness fixture 检验。

`--matrix` 扩展到大小 1/17/130/512/2048、两 dtype、多请求和算法。大图整数参考路径默认省略，可单独运行。此工具显式提高依赖 work budget 并关闭可选缓存校验，避免把默认 budget 中止当成算术耗时；应用应按实际负载设置预算。

## 在 macOS / FreeBSD 上应重点分析

分别测试 Apple Silicon、x86_64/AVX2；没有测到的平台不填“通过”。固定编译器、Release flags、SLEEF 版本、CPU 型号、页大小、tile 大小、worker 数和电源/温度条件，保存完整命令。不要开 `-ffast-math`、FTZ 或近似除法后仍宣称 strict 一致。

先运行 correctness，再比较三个计算路径和 auto。分别看编译、第一次运行、热运行与 callback；端到端时间减 callback 只是宿主/分配/排队等合计，不能直接叫“纯调度开销”。小请求、单 alpha 拷贝、opaque 零源读取、Set view 是观察宿主成本的对照；完整大图更适合看 SIMD 与内存带宽。回调计时是微秒整数，小请求有计时量化误差。

FMT-04 核心采用 128 样本分块，直接 mul/div；本轮已将有限值校验、运算及结果检查融合进专用标量／SIMD 路径，异常块完整回退。仍可实测 64/128/256 块大小、宽度尾段、非对齐行、稀疏请求和次正规数分布后再选择默认策略。AVX2 division 不保证比标量在所有尺寸上更快。NEON 与 AVX2 已在本轮 M5/macOS 和 i9/FreeBSD 上执行验证，详见 [native review](NATIVE_REVIEW_2026-09-26.md)。

FMT-05A generic 支持完整单 owner 仿射映射的视图证明（含负 stride 和同 owner 外部平面）；planar 的视图优化目前保守地只接受内部 identity 映射。其余 auto 物化，强制 view 返回 ViewUnavailable。不能拿一个 R-only 请求为完整输出伪造可表示的布局。进一步放宽 planar 同 owner 外部平面视图是审查/优化对象，不应删除校验或借用多 owner 虚拟页面。

除单算子外，还要测 associate→unassociate、extract→set、上下游多个 tile、并发不同 bindings、上下文销毁后视图寿命，以及取消/图失效与内存紧张。当前 exact planar 调度按端口规范化依赖并合并共享 alpha，但不新增线程池、不做跨算子表达式融合；长链中递归子计划和重复元数据计算仍是重点。

## Reviewed revision: managed budgets and repeatable comparisons

The optional final driver argument is `managed[off|on]` (default `off`). `on`
constructs an `ExecutionContext` with the standard `ResourceLimits`. Added CSV
fields are `managed`, `issued_work`, `managed_peak_host`,
`managed_peak_metadata`, and `managed_peak_referenced`. Work is cumulative over
all executions in that process, including two warmups; it is **not** per-pixel
work. These capacity counters follow the resource model, not process RSS. A
resource-limit failure remains a failing benchmark, never a missing/zero sample.
Large managed runs may require deliberately raising the driver's resource limits;
do not disable the checks and report them as managed measurements.

```sh
python3 examples/alpha_performance/run.py \
  build-alpha/examples/alpha_performance/photospider_alpha_performance \
  --managed on --output managed.csv --log managed.log
```

`compare.py` compares two already-built binaries in before/after/after/before
order. Each process checks every requested output sample on its first execution
outside the timed interval, performs two warmups, and then records 11 hot runs
by default. Summary p50 is the median of the two process p50 values, **not** a
confidence interval or a pooled percentile. `raw.csv` and `commands.log` retain
every process and every hot latency. Small-ROI differences can be dominated by
run-to-run host variation; don't interpret every ratio above one as a finding.

The script takes `/tmp/photospider-performance.lock` using a nonblocking exclusive
POSIX lock and rejects visible compiler/linker activity before and between cases.
Other build/profiling jobs must honor the same lock. It does not isolate the
machine from GUI/background workloads or control clock frequency/temperature.
Use a new output directory for each run; existing results are never overwritten.

```sh
# --before is the correctness-fixed but not numerically optimized build.
python3 examples/alpha_performance/compare.py \
  --before /path/to/fix-only/photospider_alpha_performance \
  --after /path/to/optimized/photospider_alpha_performance \
  --output alpha-abba --suite extended --workers 1 --repetitions 11
# FreeBSD: add --launcher 'cpuset -l 0'. Linux: add --cpu <allowed CPU>.
# macOS: omit affinity; capture power/thermal/system-activity observations.
```

The extended suite includes 38 configurations (152 independent processes):
f32/f64, scalar/SIMD/strict-auto, managed on/off, mixed/half/opaque/small alpha,
continuous 2048-square images, 3x3 ROIs crossing the 128 boundary, generic/tiled/
continuous storage, small images, Set view/materialize, extract/remove/opaque,
and small exact-reference checks. It is a targeted regression/performance suite,
not an exhaustive benchmark. The scalar/NEON/AVX2 comparison must be repeated on
the intended machine; ratios from another CPU/OS/compiler are not transferable.

For the reviewed revision, semantic fast paths classify and validate integer
lanes before FP operations, use direct target-precision multiply/divide, and
repair exceptional spans through the complete original slow/reference path.
Cancellation and sample-work precharge remain at bounded 128-sample blocks;
checked row spans are reused only within their original fragment/tile bounds.
Strict `auto` still selects scalar, accelerated `auto` selects SIMD; this policy
was not changed on the basis of measurements from a different target machine.

`--suite latency --repetitions 101` repeats the 11 small-ROI/small-image/helper
configurations with more hot observations. Keep both positive and negative
ratios; a longer within-process run still is not a statistical guarantee across
machines or sessions. Compiler detection recognizes versioned executable names
(e.g. clang++22 and clang++-22). An explicit `--allow-stopped-compilers` permits
SIGSTOP-paused compiler processes only and records their snapshot. Prefer
finished builds instead; never use this flag to permit active compilation.

## Native revision-2 review

The [2026-09-26 native review](NATIVE_REVIEW_2026-09-26.md) records three additional
correctness fixes, successful native M5/FreeBSD validation, before/after timing,
negative small-ROI observations, profiling limits and local reproduction artifacts.
