# Float32 图像算子

默认 registry 包含两个 CPU 算子，实现位于
[`plugins/ops/image_operations.cpp`](../../../plugins/ops/image_operations.cpp)。
两者都有两个有序 runtime Value input，无 compile-time parameter 或隐式默认值，
输出一个由 workflow 命名的完整图像。

| Operation | Input 0 | Input 1 | Output |
| --- | --- | --- | --- |
| `image.exposure_gain` | 图像 | Float32 gain scalar，闭区间 [0,16] | RGB 乘 gain，alpha 逐 bit 复制 |
| `image.opacity` | 图像 | Float32 opacity scalar，闭区间 [0,1] | 全部 RGBA channel 乘 opacity |

图像必须为 dense Float32 {H,W,4}，H/W 为正，whole Region，零 offset，canonical
row-major stride；精确包含一个 facet：key `photospider.image`，version 1，payload
`rgba;linear-srgb;premultiplied;hwc`，共 34 个 ASCII byte，不含 NUL。
RGB 必须 finite 且非负，alpha 必须 finite 且位于 [0,1]，alpha 为零时 RGB 必须全零。
HDR RGB 可以超过 1 或 alpha，接受 signed zero。Caller 提供已转换为 linear-sRGB
premultiplied 的值；不执行 color conversion、gamma、clamp 或 unpremultiplication。

Scalar input 必须直接引用 workflow declaration：Float32 {1}、whole Region、零 offset、
stride {4}、四字节且无 facet。每次运行的 gain/opacity byte 不属于 source parameter，
也不改变 compiler identity。

两者为 deterministic、side-effect-free、cacheable、PreserveFirstInput 和 Elementwise。
Image input demand 为请求的空间 output demand，完整包含四个 channel；scalar demand
始终为 whole {1}。小范围 demand 仍返回完整 dense image。每个 image step 至少按 output
byte count 的两倍建模 callback output 和 host copy，不表示 input/intermediate/process
总内存上限。

每个运算阶段按 IEEE binary32 nearest、ties-to-even 舍入，保留 gradual underflow。
Host schema/numeric validation 和 image callback scope 保存并恢复 thread 浮点环境，
避免继承的 rounding 或 flush-to-zero 模式改变结果。计算出的 non-finite pixel、错误
profile 或 alpha-zero/nonzero-RGB output 返回 OperationFailed。绑定 pixel/scalar
数值域错误在所有 callback 前返回 InvalidArgument。

## 可执行 public API 示例

[`tests/consumer/image_fixture.hpp`](../../../tests/consumer/image_fixture.hpp)
构造 [ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md) 的精确三项声明、
两个节点和 A/B binding snapshot。[`test_bindings.cpp`](../../../tests/integration/test_bindings.cpp)
编译一次，分别顺序和并发执行两个快照，将具名 `result` descriptor 与全部 64 字节
output 对照 `s1-rgba32f-exposure-opacity-v1`。它也覆盖各 input 单独变化、opacity 为零、
preflight failure、schema、Halo、停止和资源边界。

```sh
cmake --build build/issue257-static --target test_bindings -j 8
ctest --test-dir build/issue257-static -R '^test_bindings$' --output-on-failure
```

隔离 installed consumer 通过默认 C++ 算子和独立编译的 ABI3 C plugin 执行相同 oracle，
检查 package 0.3 消费成功并拒绝 0.2 consumer。分别使用 BUILD_SHARED_LIBS OFF/ON
验证两种 package，参见[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。
