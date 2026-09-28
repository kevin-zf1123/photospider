# 原生 GPU workflow

`ExecutionMode::NativeGpu` 允许放置声明了原生实现的算子；`ExecutionContext`
选择已配置的 Metal 或 Vulkan 设备。该模式不定义算术，数值契约由各算子和 profile
负责。当前宿主路径由 `test_native_execution`、`test_native_gpu` 与
[G4 GPU workflow](../../../examples/g4_gpu_workflow/README.md) 覆盖。支持情况需按已注册
算子和已配置设备逐项确认。

`examples/s4_gpu_workflow` 源码保留为迁移 fixture，其旧内建 Gaussian 场景
在滤镜退役后没有默认 registry 实现。独立构建的 `rgba32f` C 算子模块有自己的
注册和资源边界；加载它不表示新 FIL 规格已经注册。
