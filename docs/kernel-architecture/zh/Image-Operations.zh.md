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

## 可复用算子包与可执行示例

[`plugins/ops/rgba32f`](../../../plugins/ops/rgba32f/CMakeLists.txt) 仅通过
`Photospider::operation_sdk` 构建受维护的 ABI4 C module `photospider_rgba32f_ops`。
它实现上述两个算子和相同 profile，使用严格浮点编译选项。ABI4 host 在进入 callback
之前验证 port 并建立 nearest/gradual-underflow 浮点环境。Callback 持有临时输出
buffer，等同步 sink 复制后释放；成功、拒绝和取消路径均释放。将可信包加载到空
registry，随后 freeze 再编译；default registry 已有相同 operation key。

[`examples/image_vertical/image_fixture.hpp`](../../../examples/image_vertical/image_fixture.hpp)
是测试、安装消费者和配套 daemon vertical 共用的公开 fixture contract，固定
[ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md#named-fixtures-and-image-oracle)
中的声明、算子链、A/B 值、shape/layout/facet、请求像素 (0,1) 和输出表。
有界 CPU oracle `s1-rgba32f-exposure-opacity-v1` 从每份 binding snapshot 独立计算
16 个 channel，每阶段舍入到 binary32，先核对冻结输出表，再精确比较具名 `result`
的完整 descriptor 和全部 64 字节。Oracle 不调用算子 callback。

[`photospider_image_vertical`](../../../examples/image_vertical/main.cpp) 编译一次，
以同一个 plan 执行 A/B。每次要求两个成功 CPU callback（node 10、20）、相同 plan
identity、预期的不同 result digest、零 transfer/byte/fallback 和 128 modeled byte
峰值。两个 image input demand 和 step output demand 均为 offsets {0,1,0}/extents
{1,1,4}；scalar demand 为 whole {1}，结果仍是完整 {2,2,4} 图像。程序分行输出具名
输入/输出 Value、descriptor/Region/layout/facet、plan/result digest、编译/执行/算子
耗时、选用 backend、传输/资源观测及 correctness。耗时可以为零。Digest 用于诊断，
correctness 比较实际 byte。

随后每份 payload 执行两个 raw benchmark sample，分别捕获匹配的 CPU oracle。
这些 sample 保留 `RawBenchmarkRunner` 每次独立编译的语义，与编译一次的直接执行
分开报告。任何不匹配返回非零。无参数时使用内置算子，可选参数为可信 native module
的精确路径。

```sh
cmake --build build/issue257-static --target photospider_image_vertical test_bindings -j 8
build/issue257-static/examples/image_vertical/photospider_image_vertical
ctest --test-dir build/issue257-static -R '^test_(image_vertical|image_vertical_plugin|bindings|installed_consumer)$' --output-on-failure
```

`test_image_vertical_plugin` 向同一程序传入 generator 解析的包路径。`test_bindings`
保留精确 binding/output-demand 负例、独立并发快照、数值/浮点环境边界、取消和资源
检查，正常 DSO 路径改用受维护的算子包。故意发布错误输出的 DSO 仅留在测试中。

安装内核后，两个源码目录也可以独立构建：

```sh
cmake -S plugins/ops/rgba32f -B build/rgba32f-package -DCMAKE_PREFIX_PATH=/absolute/kernel-prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/rgba32f-package --target photospider_rgba32f_ops -j 8
cmake -S examples/image_vertical -B build/image-example -DCMAKE_PREFIX_PATH=/absolute/kernel-prefix -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/image-example --target photospider_image_vertical -j 8
build/image-example/photospider_image_vertical /absolute/path/to/native-module
```

隔离安装消费者通过 installed SDK 构建同一算子源码包，在 shared bridge 中运行 A/B，
并以默认算子和 module 分别运行相同示例。Static/shared 内核均验证此路径、package
0.3 消费及 0.2 拒绝，参见[测试与验证](../../development/zh/Testing-and-Validation.zh.md)。

## S2 存储与 liveness 修订

#264/#210 已迁移 ABI4 宿主输出/scratch。每个图像 step 预留输出字节，不需要第二份
sink copy；完整 Run 预留包含保留中间结果和 scratch。调用方已有输入及进程 RSS 不计入
受控预算。当前 S1 整图两步场景实际分配峰值为 128 bytes，每步输出容量 64 bytes；
旧 2B 回调估算说明由这些实际存储语义替换。区域执行尚由 #265 追踪。
