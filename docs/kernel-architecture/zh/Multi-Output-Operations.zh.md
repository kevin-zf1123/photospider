# 多输出算子

命名多输出接口提供编译时确定的命名结果。Workflow 边通过
`WorkflowNodeOutput{node, port}` 选择端口，根使用 `WorkflowOutput{name, node, port}`。
这些公开 API 已实现。未请求的纯端口不会执行。`ExecutionOptions::enable_joint`
控制可选 CPU 物理优化，不改变数值语义。

## 当前支持边界

包 0.20.0 移除 `color.rgb_to_ycbcr420`。FMT-16 已退休，外部色度子采样／重建
归独立 input/output codec 边界。
下方 image 算子及 `test_multi_output_ops.cpp` 保留作待迁移源码说明；旧 typed
图像路径受 planar 门禁约束，该测试目前不在活动 CTest 清单中。这些段落不构成
新 planar 图像执行支持声明。通用命名输出 API 本身继续保留。

## `image.split_horizontal`

输入为 Float32 HWC Image，必填 Int64 参数 `split_x` 满足 `0 < split_x < W`，
没有默认切分位置。输出保留源颜色、alpha 和通道解释：

| 端口 | shape | 源映射 |
| --- | --- | --- |
| `full` | `{H,W,C}` | `(y,x,c)` |
| `left` | `{H,split_x,C}` | `(y,x,c)` |
| `right` | `{H,W-split_x,C}` | `(y,x+split_x,c)` |

各输出在自己的坐标中接受独立的完整像素 Region。分阶段读取精确声明映射后的源
像素。发布的 Value 是不可变视图，具有检查过的 origin-relative layout 并保留源
storage owner；收集成稠密结果时可能复制视图。联合成员在需求相等时共享源传输，
同时保留独立偏移证据。参数与尺寸减法在读取样本前验证。

集成测试请求三个不同偏移 ROI，在 joint 开关两种模式下检查源值、facet 和 owner
身份，验证 dirty 映射，并拒绝负数、零及超出宽度的切分位置。

## `image.convolve_channels` 与区域化 `field.convolve`

`image.convolve_channels` 按顺序接收 Float32 RGB Image 和 R、G、B 三个普通
Float32 HW kernel。RGB 角色可以在存储中重新排列。各存储 RGB 通道独立滤波，
不输出 alpha，也不隐式去预乘。`r/g/b` 输出为 Float32 HW ScalarField，保留对应
relative 通道角色。每通道必填 Int64 `{r,g,b}_anchor_y`、`{r,g,b}_anchor_x` 和
String `{r,g,b}_boundary`（`zero` 或 `clamp`），没有默认值。Anchor 必须位于
自己的 kernel 内。支持奇数、偶数及非对称 kernel，不添加归一化或 bias。

`field.convolve` 保留公开输入顺序、同 dtype Float32/Float64 HW kernel，以及
必填 `anchor_y`、`anchor_x`、`boundary` 参数；支持 generic field 的分阶段区域请求，
旧 ImagePlane 路径仍需 planar 迁移。输出为普通 HW field。`field.correlate` 保留现有 Whole 实现。

两条卷积路径均按固定 kernel 行优先顺序、binary64 累加计算
`sum K[ky,kx] * I[y+anchor_y-ky,x+anchor_x-kx]`。Zero 在外部补零，clamp 重复最近
有效边缘。输入、系数、中间值和最终 dtype 转换必须有限且可表示。每输出声明自己
完整的 kernel 及精确裁剪后的源邻域；image-v2 仍读取完整源像素。Data/Validation
角色明确，不读取兄弟 kernel 样本，因此修改 G kernel 只使 G 失效，R/B 保留缓存。
独立请求某通道时，不相关 kernel 的非法样本不会使该请求失败。

集成测试使用独立奇偶非对称 kernel 和 anchor，对照标量 oracle 检查两种执行模式，
仅改变 G kernel 并验证 3×4 图像有 24 个 R/B cache hit，同时验证有限 field ROI
不会读取远处 NaN。既有基本卷积结果由 `test_basic_operations` 覆盖。

## `image.gaussian_blur_with_kernel`

输入为 Float32 HWC Image，输出 `image` 保持原描述与 facets，`kernel` 为
通用 Float32 HW。必填 Float64 `radius` 和 `sigma` 必须有限且位于 `[0,64]`。
可选 String `boundary` 默认为 `clamp`，也可为 `zero`。
令 `R=ceil(radius)`，kernel 尺寸为 `{2R+1,2R+1}`，sigma 为零时尺寸也不变。
整数坐标权重为 `exp(-(x*x+y*y)/(2*sigma*sigma))*a(x)*a(y)`，其中
`a(d)=clamp(radius+1-abs(d),0,1)`，最终归一化；sigma 为零使用中心冲激。
radius=1.25 生成 5×5，最外层每轴覆盖因子为 0.25。实现保留整数上侧相邻
浮点 radius 的正边界权重，极小正 sigma 不产生中心除零错误。

系数仅转换一次为 Float32。图像路径使用同一系数、核行优先顺序和 binary64
累加，示例显式绑定独立的 R 平面供 `field.convolve` 复算，不再调用已退休的通道提取。
输入样本与输出须有限且可表示。图像观察读取裁剪后的半径邻域，并记录 Data
和 Validation；kernel 只依赖静态参数和描述元数据，不读取图像样本。
当前缓存身份仍包含完整参数集合。

Singleton 在读取图像前检查点保存宿主拥有的核表；joint 共享一次核生成，
通过共享 work 服务计费。发布的 kernel view 独立持有 backing owner。
共享 work 错误具有粘性，C ABI 同样执行；上游失败成员先退休，再继续其他成员。
既有 Gaussian 算子语义保持不变。

`test_multi_output_ops` 检查 radius 为 0、0.25、1、1.25、2、64 的完整矩阵，
整数两侧相邻浮点值、零与极小 sigma、非法参数、kernel-only 零图像读取，
以及两种 boundary 和 joint 开关下公开 `field.convolve` 的精确复算。

保留的三个旧图像场景及迁移边界见
[示例说明](../../../examples/multi_output_workflow/README.zh.md)。

卷积和高斯核生成建立默认最近舍入/渐进下溢环境，结束后恢复调用者原环境。
调用线程使用向上或向下舍入时，直接 generic field/kernel 输出仍与 typed image
及 joint 路径一致；两种公开直接调用均有环境恢复回归。
