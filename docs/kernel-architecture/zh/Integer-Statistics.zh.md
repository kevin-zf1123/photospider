# 整数直方图与全局调色

英文权威版本：[Integer-Statistics.md](../Integer-Statistics.md)。

## 1. 模块边界与职责

统计 API 描述整数编码的标量 raster、稀疏直方图、全局参数和调色输出。CPU producer 扫描不可变 Value 快照，并在执行根的容量、work、I/O 与 stage 预算下发布必需 Result backing。直方图 producer 在发布前持有有界 bin 计数器和临时状态；下游 Result 保留输入关联及其 backing。

## 2. 核心数据结构与内存布局

```cpp
struct StatisticsSpec final {
  std::uint64_t height = 1, width = 1;
  std::uint32_t bins = 8;
};
enum class StatisticsRepresentation : std::uint32_t {
  Histogram = 1, Parameters, Graded
};
Result<SchemaTemplate> statistics_schema(
    StatisticsRepresentation representation, const StatisticsSpec& spec);
Result<double> statistics_mean(std::int64_t total, std::int64_t count);
```

`StatisticsSpec` 要求 H、W 为正数，bins 为 1 到 65536。Raster 样本数受限于 `(INT64_MAX-4095)/8`。输入是无 facet 的 Int64 HW 值和 UInt8 HW mask。mask 字节非零时选中对应样本；选中值必须在 `[0,bins)` 内。mask 排除的值不参与统计。operation 不推断颜色，也不应用传递函数。

| Result schema | 字段与发布方式 |
| --- | --- |
| `photospider.integer_histogram` | RuntimeCount Int64 `bin`；对应 Int64 `count` 行；CompleteBundle |
| `photospider.integer_statistics` | Int64 `[count,total,valid]` 和 Float64 `mean`；CompleteBundle |
| `photospider.graded_scalar` | 固定 H*W 个 Float64 `pixels`；HW domain；StablePrefix |

Histogram ID 严格递增，且只保存计数为正的 bin。完整 histogram seal 后，缺少的 bin 表示零。空选择没有数据行。空统计量为 `[0,0,0]` 和不具语义的零 mean；非空且全为零的选择仍然有效，mean 为零。物理 Result window 大小不进入 schema identity。

## 3. 调度与状态机

```text
Int64 raster + UInt8 mask
          |
          v
 histogram producer --完整稀疏 Result--> parameter producer
                                               |
                                        完整参数 Result
                                               |
                                      grade producer 校验全局参数
                                               |
                                      有界输出 / stable prefixes
```

Histogram factory 在绑定输入前检查必要 request-stage 下界。令 `S=H*ceil(W/512)*ceil(bins/512)`；source request polls 加一个 completion poll 必须能够放入 stage 上限。`S>=1,000,000` 时工厂返回 `ResourceExhausted`。实现使用除法检查以避免溢出。通过准入不代表资源已预留；有效 producer 上限还受到 dependency option（默认 4096 stages）和根 stage budget 的约束。后续 I/O、work、stage 或容量耗尽会阻止完整 histogram 发布。

Histogram 和 Parameters 使用 CompleteBundle 及 Conservative Cartesian support。Parameters 等待完整 histogram，再检查稀疏 ID、正计数、总和、整数溢出及选中计数是否超过 HW。Grade 在产生任何像素 prefix 前校验完整全局参数 Result。每个输出像素依赖其 source 样本、共享参数和 descriptor support。Descriptor observation 与数据行分离，空集合也如此。后续像素处理失败时，已发布的 stable prefix 仍有效。

Histogram source strip 按行截断，每个字段最多 4096 字节，且不受 Result read-window 大小影响。其他 source 读取和 Result page 最多使用 `min(user_page_bytes,4096)`。24 字节 `count_total_valid` 记录不可拆分；所选 window 小于 24 字节时返回 `ResourceExhausted`。Histogram counter/临时状态、source/result window、必需 backing，以及每次初始化、重置、扫描，分别消耗适用的根容量或 work。

## 4. 算法与数学

`bin_ranges_512` producer 预留 `min(bins,512)` 个 Int64 counter，对不可变 source 按每个 bin 范围重读，并按递增顺序追加正计数。Counter 存储最多 4096 字节。若 raster 样本数为 N、bin 数为 B，工作量为 `O(N*ceil(B/512)+B)`。该方法用重复 source 读取换取有界计数器内存；不分配完整 B-counter 数组，也不建立未计账输入 spool。

Parameters 使用通过检查的非负 Int64 算术计算 `D=sum(count)` 与 `T=sum(bin*count)`。当 `D>0` 时，mean 是精确比值正确舍入到 binary64 的值：

$$
\mu = \operatorname{round}_{64}(T/D), \qquad 0 \le T/D < 65536.
$$

整数长除法提取 53 位有效数，并比较余数实现最近偶数舍入。将 T 和 D 分别转为 binary64 再相除可能得到不同结果。公开 `statistics_mean(total,count)` helper 在相同比值域内接受非负 Int64 total 和正 Int64 count。空输入发布 `[0,0,0]` 和不具语义的零 mean；非空全零输入有效且 mean 为零。

`statistics.grade` 要求有限非负 Float64 target、有效参数和正 mean。使用已舍入 mean 前，会先按精确整数比值检查 `bins-1`。计算为：

$$
\mathrm{gain}=\operatorname{round}_{64}(target/mean),\qquad
 y_i=\operatorname{round}_{64}(\mathrm{gain}\cdot\operatorname{round}_{64}(x_i)).
$$

实现使用最近偶数舍入、渐进下溢、不收缩 FMA，并恢复调用方浮点环境。Gain 或输出非有限时为 `ArithmeticOverflow`；空统计、零 mean 或参数不一致时为 `InvalidDomain`。不会裁剪或替换成近似结果。

## 5. 限制与非目标

- 输入是整数编码标量域；颜色、亮度和 transfer 转换不属于该操作。
- Histogram 的固定 stage 前置检查只适用于该 recipe；Parameters 和 Grade 不执行此检查。
- Float64 grade 是测量结果，没有认证数值误差界。Managed capacity 不限制进程 RSS。
- 物理 page 几何不改变 schema identity 或共享结果 key。

公开入口与可运行用法见 [statistics workflow](../../../examples/statistics_workflow/README.md)。
