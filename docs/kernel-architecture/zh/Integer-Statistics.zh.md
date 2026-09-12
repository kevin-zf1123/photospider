# 整数 histogram 与全局 grade

英文权威文档：[Integer-Statistics.md](../Integer-Statistics.md)。

Package 0.10 的安装 C++ API 提供 `statistics_schema`、`statistics_mean` 和
`make_statistics_operation`。C operation ABI 保持 9。CPU 阶段使用 structured
protocol 2、根预算准入、强制临时存储和既有共享 Result 生命周期。

## Schema 与成功域

`StatisticsSpec{height,width,bins}` 要求 HW 为正，bins 为 1..65536，样本总数
不超过 `(INT64_MAX-4095)/8`，确保标量存储偏移满足临时存储契约。输入为无
facet 的 Int64 HW 数值和 UInt8 HW mask，非零 mask 选择样本。被选择的整数
必须位于 `[0,bins)`；未选择值可以超出范围。不隐含颜色、亮度或传递函数转换。

版本 1 的 `integer_statistics_v1` metadata facet 按四个 little-endian UInt64
保存 profile 1、H、W 和 bins。

| Schema | 发布方式与字段 |
| --- | --- |
| `photospider.integer_histogram` | CompleteBundle；RuntimeCount Int64 `bin`，FieldRows Int64 `count` |
| `photospider.integer_statistics` | CompleteBundle；一个 Int64[3] `count_total_valid`，一个 Float64 `mean` |
| `photospider.graded_scalar` | StablePrefix；固定 H*W 个 Float64 `pixels`，HW domain |

数学 histogram 使用固定区间 `[j,j+1)`。稀疏编码只存正计数，bin ID 严格
递增；有被选择的零值时必须保留 bin 0。动态行数是非零 bin 数，不是样本数。
只有 seal 后缺失 bin 才代表零。参数阶段校验所有导入行，拒绝重复、越界 ID
和非正计数，并核验整数运算及样本总数不超过 HW。公开工厂创建的生产者负责
语义校验；第三方发布者仍须履行所注册 schema 的语义义务。

`D=sum(count)`、`T=sum(bin*count)` 使用有溢出检查的非负 Int64。空集合产生
`[0,0,0]`，mean=0 只是无语义占位；非空全零样本产生 `[D,0,1]`。D>0 时，
mean 是精确有理数 T/D 正确舍入到 binary64 的值。整数长除法提取 53 位有效
数字，并比较余数完成 ties-to-even。分别把 T、D 转成 double 后相除不等价。
公开 `statistics_mean` 支持非负 Int64 分子、正 Int64 分母且 T/D<65536。

`statistics.grade` 要求有限非负 Float64 参数 `target`，完整一致的参数、
valid=1 且 mean>0。先以整数运算检查真实比值不超过 bins-1，再检查舍入后的
mean。运算依次为 `gain=round64(target/mean)`、
`y[i]=round64(gain*round64(Int64(x[i])))`，使用最近偶数舍入、渐进下溢和无
收缩运算，结束后恢复浮点环境。非有限 gain/输出返回 ArithmeticOverflow；
空、零均值或不一致参数返回 InvalidDomain。不用近似结果替换失败，不声称
CertifiedBound。

## 依赖、分页与生命周期

Histogram 在首次发布前扫描完整选择域；参数阶段等待完整 histogram。两者
使用覆盖输入的 Conservative Cartesian 关系，包含 mask 的完整验证和控制域。
Result descriptor 另以保留的 `[0,1)` 观察记录，空集合也保留 count 依赖。

Grade 在首次前缀前验证完整参数。关系由每个源像素的紧凑 identity、共享全局
参数支持及 descriptor 合并而成；所有前缀复用同一关系 owner，不创建每像素
全局依赖向量。合并保证为 Conservative。后续像素或运行失败保留此前已认证
范围的不可变性，但不产生完整图像。

命名方案 `bin_ranges_512` 在分配前准入 min(B,512) 个 Int64 Payload 计数器。
每个 512-bin 范围重读同一不可变源快照，按全局 bin 顺序追加正计数，全部范围
完成后才 seal。这实现 Design 的按 bin 范围多遍扫描路径，工作量为
O(N*ceil(B/512)+B)，驻留计数器最多 4096 字节。初始化、清零、扫描及每一遍
样本处理都消耗根 work；额外遍数可导致工作预算耗尽，不申请完整 B 计数器或
未计账的输入 spool。源 strip 在 HW 行边界停止，每字段
不超过 4096 字节；histogram 输出和参数输入遵守相同页上限。24 字节参数
记录不可拆分，窗口过小明确失败。Grade 向强制存储写入有限窗口并向活动下游
发布稳定前缀。stage/work/capacity 耗尽保持独立资源失败。页几何不进入语义
schema 或共享结果 key。

Result association 保留上游 ObjectId 及存储。读取窗口在 Result 和
ExecutionContext 销毁后仍保留关联所有权，最后一个结果或窗口释放后回收
强制存储。关闭可选缓存时，相同 DAG 表达式共享生产者；不同 Run 输入快照
的全局参数不会互相别名。这是 managed-capacity 保证，不是进程 RSS 硬上界。

## 可执行验收

[statistics_workflow](../../../examples/statistics_workflow/README.md) 提供完整
source → histogram → parameters → grade → 活动分页 sink、独立安装 consumer
命令和 Python Fraction/Counter 参考。验证逐像素结果、动态/空计数、多页
histogram、跨行 HW、小窗口和预算、cache-off 别名、不同 Run 快照及上下文
销毁后的所有权。`test_statistics` 验证 binary64 舍入和 schema 错误；
`test_statistics_callback` 通过公开 callback 检查外部畸形稀疏数据、整数溢出
和大整数有理数参考。像素容差只是 fixture 实测符合，不是认证上界。
