# 多输出算子

命名多输出接口提供编译时确定的命名结果。Workflow 边通过
`WorkflowNodeOutput{node, port}` 选择端口，根使用 `WorkflowOutput{name, node, port}`。
这些公开 API 已实现。未请求的纯端口不会执行。`ExecutionOptions::enable_joint`
控制可选 CPU 物理优化，不改变数值语义。

## 当前支持边界

默认 registry 当前在本页的图像多输出算子中提供 `image.split_horizontal`；
其旧 typed 图像路径仍受 planar 存储门禁约束。命名多输出宿主 API 可独立使用。

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

维护中的宿主契约与执行测试验证命名多输出基础设施。切分示例不构成当前 planar
运行时验收。

## 滤镜状态

默认 registry 已移除 `image.convolve_channels`、`image.gaussian_blur_with_kernel`
及其 `field.convolve` 依赖。拟议替代行为见
[05-filter](../../built-in_ops/05-filter/spatial.md)。命名多输出宿主 API 与
`image.split_horizontal` 保留。[示例](../../../examples/multi_output_workflow/README.zh.md)
只保留切分场景，当前不作为 planar 运行验收。
