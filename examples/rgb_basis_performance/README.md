# FMT-10 Result 性能 smoke 与历史 Value/planar 数据

FMT-10 A/B/C 当前以 Result ABI 2 注册，D 使用公共 authoring helper 展开为
A→可选 C→B。性能驱动已迁移到公共 Result API。旧参数说明、计时字段、算法
对照和测量记录保留在下方“历史 Value/planar 记录”中；这些内容描述旧运行路径，
不是当前 Result 性能数据。

## 当前 Result 驱动

构建并查看 CLI：

```sh
cmake --build build/fmt10 --target photospider_fmt10_performance -j 2
EXE=build/fmt10/examples/rgb_basis_performance/photospider_fmt10_performance
"$EXE" --help
"$EXE" 130 f32 a strict cross 1 planar 128 1 materialize respect 0 3
```

13 个位置参数保持不变，依次为 size、dtype、member、profile、coverage、
repeats、storage、tile、workers、layout、metadata mode、warmups 和 gate_edge。
size 为 3..4096；dtype 为 `f32|f64`；member 为 `a|b|c|d|i`；profile 为
`strict|accelerated_apple_silicon|accelerated_x86_64`；coverage 为
`full|color|alpha|cross`；repeats 为 1..100；storage 为 `planar|generic`；
tile 为正的二次幂；workers 为 1..64；layout 为 `auto|view|materialize`；
metadata mode 为 `respect|raw`；warmups 为 0..20；gate_edge 为 1..size。
`planar` 输入使用 spatial Result layout 和 tiled planes，`generic` 使用
generic tensor layout。输入 Result tensor 形状为 `[size,size,4]`，分量类型为
Float32 或 Float64。

`a`、`b`、`c`、`d` 分别执行 RGB→XYZ、XYZ→RGB、白点适应和公共 D helper；
`i` 是相同白点的 C identity，用于检查 view 与 materialize。D 固定为
sRGB→CAT16 D50→ProPhoto。`color` 请求第一个颜色分量，`alpha` 请求 alpha，
`cross` 请求 y/x 从 tile-1 起的 3×3 ROI 和四个通道，因此 tile=128 时 ROI 为
[127,130)×[127,130)。输入先完整发布；result cache 关闭。

驱动先编译同一参数的 strict reference graph，再对当前 graph 执行；每轮计时执行
后的门禁检查最多 `gate_edge×gate_edge` 的请求区域。Float32 与 strict 输出按位
比较；加速 Float64 使用四个 Float32 ULP 步长界，D 使用 16 步长界。这个门禁
复核当前运行与 strict 实现的一致程度，不是独立精确 oracle。Independent
coefficient/rounding oracles 由 `test_rgb_basis` 覆盖。`execute_ms` 只计 public
execution call；门禁读取发生在计时之后。`input_ms`、`author_ms` 和 `compile_ms`
分别计输入 Result 构建、helper graph expansion 和实际 graph compilation。

Result CSV schema 为：

```text
size,dtype,member,profile,coverage,storage,tile,workers,layout,mode,iteration,input_ms,author_ms,compile_ms,execute_ms,source_payload_bytes,run_live_payload_bytes,run_live_metadata_bytes,source_logical_bytes,issued_work,root_peak_host_bytes,numeric_evaluated,numeric_fallbacks,gate_samples,warmups
```

`source_payload_bytes` 是输入发布后的 Root live payload。重复执行前的 Root
baseline 在 strict reference、编译和 warmups 后读取；`run_live_payload_bytes` 与
`run_live_metadata_bytes` 表示该基线之上的 live 增量。`source_logical_bytes` 是
当前 output dependencies 报告的 source support 元素数乘 dtype 宽度，不是物理
流量。`issued_work` 是该次执行的 Root work 增量。`root_peak_host_bytes` 是累计
Root Host peak，包含 setup、reference/warmups 和 gate read。上述值不是进程 RSS 或
独立 output size。`numeric_evaluated` 与 `numeric_fallbacks` 来自 operation
diagnostics；`gate_samples` 是门禁比较的 tensor 元素数；`warmups` 记录本次运行
配置。CSV 每行对应一次 measured execute，execute 中位数另写到 stderr。

该示例显式配置 Root Host 总容量为 8 GiB、Metadata 子限额为 64 MiB，且 Payload
上限仍为 8 GiB。Root 统计同时包含输入、输出、中间 Result 和 strict reference
所需的结果。这是性能示例的显式压力配置，不是生产默认值；当前没有 4096 尺寸的
完整矩阵证据。

25 项 serial Result smoke 均通过门禁：20 项覆盖
A/B/C/D/I×Float32/Float64×generic/planar，size=3 且 gate 覆盖全部请求区域；另外
五项覆盖 tile=128 的 A strict cross ROI、Apple Silicon identity view、Apple Silicon
D generic、raw B alpha-only，以及 Apple Silicon C color-only；这五项的 gate 也覆盖
各自完整的请求区域。该 strict reference
门禁不是独立 oracle。完整性能矩阵、4096 尺寸、跨平台行为和速度结论均未验证。
`test_rgb_basis` 与 `test_rgb_basis_math` 已通过；integration suite 保留 4,032 项
independent oracle 检查。额外的 65,536-channel identity view 在 512 KiB Metadata
budget 下通过 byte oracle，view count 为 65,536。`installed_rgb_basis` consumer、
独立 installed-package performance consumer build，以及 size-3 Float32 D case 的
36 个请求元素门禁均通过。这些 correctness/consumer 检查不构成性能测量；完整证据
边界见 FMT-10 实现页。

## 历史 Value/planar 记录

构建目标 `photospider_fmt10_performance` 默认 EXCLUDE_FROM_ALL，须显式构建。所有计时输出为 CSV；输入预先完整发布，result cache 关闭，准备、编译和执行分别计时。记录 dtype/member/profile、ROI、存储、tile/workers/layout、backed/reserved、读/复制字节、peak live 与可用的数值统计。

```sh
cmake --build build/fmt10 --target photospider_fmt10_performance -j 2
EXE=build/fmt10/examples/rgb_basis_performance/photospider_fmt10_performance
"$EXE" --help
"$EXE" 130 f32 d strict cross 3 planar 128 1 materialize respect
```

位置参数依次为：size、f32/f64、a/b/c/d/i、profile、full/color/alpha/cross、repeats、planar/generic、tile、workers、auto/view/materialize、respect/raw、warmups（默认 0）、gate_edge（默认 3）。`i` 是同白点 C，适合对照 view 与 materialize；非 identity 的 forced view 应失败。A/B 使用 sRGB basis，C 使用 CAT16 D65→D50，D 为 sRGB→CAT16 D50→ProPhoto。输入为有限、精确二进制分数，含负值和 HDR。

`cross` 请求 y/x=[tile-1,tile+2)，例如 tile=128 时 [127,130)。为区别 reservation 与真实 backing，仍保留完整 size×size×4 描述。`color` 只请求第一个颜色分量；`alpha` 只请求 alpha。

每次运行都先执行同一图的 strict 小区域参考，再对计时输出的最多 gate_edge×gate_edge 请求区域核对（默认 3×3）。Float32/strict 为按位检查；accelerated Float64 的 native gate 使用四 Float32 steps。D 的 gate 只用保守的 16-step smoke 阈值，不是 D 的误差契约或误差传播证明（矩阵条件数可能放大逐节点误差）。独立数值 oracle 和逐节点误差正确性由单测/集成测试负责。**这不是全帧独立正确性校验。**

示例显式允许 4 GiB 输入 backed 上限、8 GiB executor live 上限和高数值工作预算，以容纳 4096² 压力场景；不适合直接当作生产默认资源配置。planar numeric 列为零通常表示接口未提供 reporter；source_read_bytes 不等价于 L1/L2/DRAM 硬件访问流量；view 的 backed_bytes 可能反映保留的原 owner，而非新分配量。

建议 macOS / FreeBSD 的正式矩阵（先单个任务验证内存，再批量执行）：

```sh
EXE=build/fmt10/examples/rgb_basis_performance/photospider_fmt10_performance
PROFILE=accelerated_apple_silicon  # Intel mac / FreeBSD amd64 改为 accelerated_x86_64
for dtype in f32 f64; do
  for member in a b c d; do
    for roi in full color alpha cross; do
      "$EXE" 4096 "$dtype" "$member" "$PROFILE" "$roi" 5 planar 128 1 materialize respect
    done
done
done
"$EXE" 4096 f32 i strict full 5 planar 128 1 view respect
"$EXE" 4096 f32 i strict full 5 planar 128 1 materialize respect
```

分别构建 CERTIFIED+SIMD、CERTIFIED+scalar、EXACT，做相同输入/ROI/profile 对照。不要将不同 storage/profile 的时间比当作单一 SIMD 提升，也不要将容器的微基准外推为机器吞吐。

实机需要追加：严格 Float64 的大整数占比；证书 miss 和 cancellation 敏感值；编译阶段重复几何推导；generic fragment miss；row-run/瓦片/page admission；D 的真实中间 owner 峰值；1/2/4/... workers；cold-page 与 warm-page（本示例只有预发布输入）；cache 开关（本示例默认仅 off）。macOS 用 Instruments/Time Profiler/Allocations 与系统内存统计；FreeBSD 用可用的 pmcstat、DTrace 或采样工具，先确认硬件计数器权限。记录 CPU、OS、Clang、编译选项、ISA、页大小、功耗/热状态和每次原始 CSV。


## 2026-09-25 修复版的对照方式

精确核另有 `PHOTOSPIDER_FMT10_EXACT_KERNEL=LIMB|REFERENCE`。默认 LIMB 使用
8192-bit 固定容量整数暂存；REFERENCE 保留审查时的分配式精确算法。
CERTIFIED/EXACT、SIMD、精确核是三个独立维度，不改变 profile 的数值契约。

`warmups` 为完整请求的非计时执行次数；`gate_edge` 可扩大 strict 参考区域。
例如 33×33 的全部请求检查、两次完整预热（仍非独立 Fraction oracle）：

```sh
"$EXE" 33 f32 d strict full 5 planar 16 1 materialize respect 2 33
```

`compare.py` 使用 Python 标准库，支持 macOS、Linux、FreeBSD；它持有
`/tmp/photospider-performance.lock`，串行运行两个现有 executable，逐 case 交替顺序，
写入原始 CSV、stderr、命令及中位数/范围，不自动构建，也不删除共享锁。
输入、profile、尺寸和请求一致；不把不同 profile 的差异称为优化收益。
为了同时支持审查版旧 benchmark，不传新增可选参数；全样本结果保留，另报告
去掉 iteration 0 的 steady-state 近似摘要。这不等于硬件频率、冷/热页或宿主负载受控。

```sh
python3 examples/rgb_basis_performance/compare.py \
  --baseline /path/to/audit/photospider_fmt10_performance \
  --candidate "$EXE" --out out/fmt10-paired-new-run \
  --profile accelerated_apple_silicon --size 1024 --repeats 7
# FreeBSD amd64: --profile accelerated_x86_64 --cpu 0
# 严格 F64 对照建议先 --profile strict --size 256，确认资源和耗时后扩大。
```

输出目录必须不存在，避免覆盖上次数据。实机应记录 compiler/flags、系统页大小、
CPU 亲和性和电源/热状态；隔离其他编译和性能任务。共享锁是协作锁，不控制系统其他负载。
不要把一幅图、一次 whole-planar callback 的 1/2/4 workers 数据解释成内部多线程扩展；
本轮修复的是所有 callback 受上下文调度上限约束，不是将单个 FMT-10 回调并行拆分。


私有 `photospider_fmt10_kernel` 目标提供数学层隔离对照，编译后示例：

```sh
cmake --build build/fmt10 --target photospider_fmt10_kernel -j2
KERNEL=build/fmt10/examples/rgb_basis_performance/photospider_fmt10_kernel
"$KERNEL" reference f64 image 1000
"$KERNEL" exact f64 image 1000
"$KERNEL" certified f32 image 1000
"$KERNEL" certified f64 wide 1000
```

模式 `exact` 使用构建所选精确核（默认 LIMB），`reference` 显式调用旧分配式算法。
同一 image/wide 数据、同一 dtype 下二者必须按位相同。certified 使用本机 AVX2/Apple NEON 候选；F32 保持严格按位结果，
F64 明确使用 accelerated 语义；后者与 exact 的速度比不是保持相同 FP64 契约的收益。
该工具先逐行运行 reference gate，再预热和计时；输出每行 ns、certified/exact 数量及
普通 scalar/array `operator new` 成功调用计数。计数覆盖计时段，排除几何准备、门禁、
预热及打印；不覆盖 aligned new、直接 malloc、内存池或 OS 分配，因此不是总分配/RSS。
覆盖只存在于此独立 benchmark executable，不进入产品库。image 为有限负值/HDR 分数，
wide 加入大指数跨度；这两套固定小输入不能代表真实图像分布，也不含 executor/work-budget
上下文。要确认完整流水线收益仍使用上面的 public execute 对照。
