# 可编辑缓存 workflow

`InputSnapshotStore` 提供不可变输入快照与 patch；`ExecutionContext::freeze`
将编译计划与绑定固定，用于执行和缓存复用。当前公开行为由
`test_input_snapshot`、`test_frozen_execution` 与 `test_planar_image_workflow`
覆盖。

`examples/s3_image_workflow` 源码保留为迁移 fixture，仍引用已退役的内建
`image.gaussian_blur`，因此其图像链不是当前默认 registry 可执行示例。新滤镜流程
需先接受 05-filter 契约并注册新的实现；拟议行为见
[05-filter](../../built-in_ops/05-filter/spatial.md)。
