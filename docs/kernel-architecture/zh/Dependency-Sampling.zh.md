# 依赖采样算子

## 范围与执行路径

算子负责算法和样本解释。每个算子还拥有一个静态 mapper（`OperationDefinition::map_dependencies`），planner 对每个输出 tile 或共享 stage 目标调用一次。Mapper 接收固定的输出查询、schema、tile grid、参数、资源 identity 和 publication 目标，返回该 tile 可以读取的输入 tile 与共享 stage tile。Planner 把这些读取合并、验证为指令的固定授权；VM 在第一次 poll 前绑定授权并提交不可变输出。数据相关的读取（例如地址来自另一个输入的 gather）必须声明有限的静态超集：mapper 返回数据可能寻址的每个 tile，callback 在运行时于该授权内选择 sample。需要根据运行时数据跳过整个分支的前端，用显式 IR conditional jump 表达这一选择。内核协议不编码 data/control 角色或样本级 relation。

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

### 发布与并行算术

`image.stmap` 的 mapper 为每个输出 tile 授权覆盖该 tile 输出像素的 map tile，以及 source 成员的全部 tile。UV 值只有在运行时才知道，因此完整 source 就是有限超集；callback 在同一次 poll 内读取其 map tile、计算 bilinear tap，并只从该授权中读取所需 tap。所有存在的 tap（包括零权重 tap）都从 source 授权中读取。数学 tap 精确不意味着样本级授权或失效。

协调器准备有界像素 span，通过 Result 写窗口写入。足够大的已准入工作可使用 CPU staged tile 服务；work-item 几何独立于 plan payload tile。Tile worker 读取保留的不可变输入并写入互不相交的准备后 span。分配、输入读取、work 准入和发布由协调器完成，并在提交前等待 worker 结束。算子在每个实际 callback 内建立数值环境；kernel 只保存和恢复线程状态。

### Map 坐标存储

输入窗口保留实际仿射/分片映射。可表示的 map row 可以保留 owner 和 stride；需要的 row copy 显式计费。完整输入覆盖或默认 tiled 存储都不意味着连续画布，也不引入隐式全量复制。

### 错误与资源

State、窗口、workspace、输出和 work 都计入 Root。非法 UV 返回 OperationFailed，结构不兼容返回 TypeMismatch，非法 boundary 返回 InvalidArgument。取消与资源耗尽保留各自状态。失败 tile 事务不公开部分 payload 或不完整证据。

## 通用 tuple observation 与数值诊断

Ops SDK `NumericDiagnostics` 通过有界通用诊断服务报告算术尝试和实现相关计数。内核只传输和合并报告，不赋予像素语义。`computed_elements` 统计新提交的 Tensor 样本，不统计动态行；缓存命中不增加计算量。饱和计数和计时观测不构成数值精度或原生设备执行证明。
