# 依赖采样算子

## 范围与执行路径

算子负责算法和样本解释。纯 `OperationDefinition::lower` 回调在执行前生成固定 task 的 reads、writes 和 scratch。Compiler 校验物理程序；preparation 绑定不可变 kernel，Run admission 提供 allocation owner。数据相关 gather 声明输入数据可能寻址的全部 tile，再在 invocation 中于固定读取范围内选择 sample。运行期分支选择使用显式 IR conditional jump。

## STMap Result 契约

`image.stmap(source, map, boundary)` 接受单 Tensor Float32 HWC RGBA source（可带显式 batch 轴），以及 cell shape 为 `[Ho,Wo,2]` 的单 Tensor Float64 map。Source 要求显式 H/W/channel 轴 0/1/2 和一个 atomic 末尾轴。Map 检查 dtype、cell shape 和 batch 兼容性，不要求其 spatial-layout 或 atomic-axis 元数据与 source 相同。输入 schema id、version 和成员 key 可为任意合法结构值。Map batch 轴为空时广播到 source batch，否则必须与 source 完全一致。Source H/W 至多 2^40。输出 `value` 使用 `photospider.image` v1/member `pixels`、source batch 轴和 Float32 `[Ho,Wo,4]` cell。

UV 使用绝对来源像素中心坐标，固定 bilinear filter。必填 String `boundary` 无默认值：

| Boundary | 长度 N 的轴上的整数 tap 规则 |
| --- | --- |
| constant | 越界 tap 贡献透明零，不读取 source。 |
| clamp | 限制到 [0,N-1]。 |
| wrap | 对 N 取非负余数。 |
| reflect | 周期 2N，重复端点。 |
| mirror | 周期 2N-2，不重复端点；N=1 时选择零。 |

所有非常量模式在 N=1 时选择零。规则使用完整来源域，不使用片段局部坐标。Preparation 不读取样本，检查结构、batch 兼容、轴上限和 boundary 选择。

对 map 坐标 u,v，令 l=floor(u-0.5)、t=floor(v-0.5)、fx=(u-0.5)-l、fy=(v-0.5)-t。四个 bilinear 权重为 `(dx ? fx : 1-fx)*(dy ? fy : 1-fy)`。算子把来源 RGB 解释为 straight，并进行 alpha-aware 重采样：

$$ A=\sum_i w_i A_i,\qquad P_c=\sum_i w_i A_i RGB_{i,c},\qquad
RGB'_c=\begin{cases}P_c/A&A\ne0\\+0&A=0.\end{cases} $$

使用 Float64 累加后收窄到 Float32 输出。加权 alpha 为零时跳过 RGB 乘法，透明 hidden RGB 不贡献颜色。算子不进行 transfer 解码、gamma/ICC 转换或 gamut clipping；需要的颜色转换由上下游显式完成。不绑定默认 finite-premultiplied 或 straight 合规 validator。UV 必须有限且在算法绝对 2^40 坐标界内；source 像素解释仍由该算法负责。

一个不透明 alpha 的舍入参考使用 u=2^-56、v=0.5、wrap，以及红色值为 [1,-1] 的 1×2 source。Float64 `(u-0.5)` 舍入为 -0.5，两个水平权重均为 0.5，Float32 红色输出为 +0（位模式 0x00000000）；精确有理权重则得到 2^-55。检查四种调用方舍入模式时保留此独立数值参考，临时检查程序遵循测试保留政策。

### 固定任务与并行算术

每个所选输出 tile 生成一个 `stmap-bilinear-fixed-v1` CPU task。Source reads 覆盖所选输出 batch 坐标对应的完整 H/W 域。Map reads 覆盖相同输出 Y/X 区域及两个 UV 分量；无 batch map 省去 batch 前缀。各区域按输入自身的 TileGrid 闭包，包括 UV 分量位于不同 tile 的 map。动态 UV 在固定 reads 内选择 sample。所有存在的 tap 均被读取，包括零权重 tap；constant 边界的越界 tap 贡献透明零。

Executor 调度独立输出 tile，并预分配 dense RGBA 存储。算子使用固定局部数组，声明 scratch 为零，不嵌套调度 worker。每个 invocation 建立 nearest rounding 和 gradual underflow，并恢复调用方浮点环境。算术保持左上、右上、左下、右下的累加顺序和独立 binary64 舍入步骤。

### View 与所有权

Kernel preparation 检查精确排序的 read 槽、dtype、完整区域、单个输出 tile、空 task 参数及零 scratch。Invocation 借用不可变 affine 输入 view 直至完成，支持 signed/zero stride 和非对齐地址。输入 view 可以使用独立 allocation。DenseHost 绑定通过 compiler 的显式 import COPY task 适配。成功输出 Result 拥有独立 dense tile allocation，可在 Run 和 context 销毁后继续存活。

### 错误与资源

Lowering 在展开每个区域前限制 source tile 枚举数量和累计 read 引用。Operation builder 在追加条目前拒绝 aggregate record/reference 超限；最终校验还检查派生图边。Source 目录大小取决于静态可达空间域，输出分配取决于所选 tile。

每个像素在把 UV 转换为整数地址前，检查两个坐标均有限且位于 +/-2^40。使用到的非法坐标返回 `OperationFailed`；所选输出像素之外的坐标不被扫描。结构不兼容返回 `TypeMismatch`，非法 boundary 参数返回 `InvalidArgument`。每个像素前检查取消，取消后返回且不发布该 task 的输出。资源失败保留原类别。最终 RET 发布和 observer 所有权遵循固定执行器的有序退休契约。

独立参考 `oracle/ops/transform/geo06/reference.hpp` 定义边界映射和 straight-alpha 重采样，每个算术操作后显式舍入。临时 public/direct 检查逐位比较输出，覆盖透明颜色、constant 部分越界和上述舍入参考。这些检查不构成 GPU 执行或第三方 normalized STMap 格式兼容性证据。
