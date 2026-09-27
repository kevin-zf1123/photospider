# 原生 Metal workflow

`ExecutionMode::MetalFp32` 和原生 GPU 服务仍可供明确支持的算子使用。
当前宿主路径由 `test_native_execution`、`test_native_gpu` 与
[G4 GPU workflow](../../../examples/g4_gpu_workflow/README.md) 覆盖。
后端支持逐个已注册算子核对，不自动扩展到拟议的 05-filter 成员。

`examples/s4_gpu_workflow` 源码保留为迁移 fixture，其旧内建 Gaussian 场景
在滤镜退役后没有默认 registry 实现。独立构建的 `rgba32f` C 算子模块有自己的
注册和资源边界；加载它不表示新 FIL 规格已经注册。
