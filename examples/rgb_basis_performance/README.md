# FMT-10 性能与正确性门禁示例

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
