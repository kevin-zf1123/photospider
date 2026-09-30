# 统一 Result 图像 workflow

本示例注册一个小型 test operation，并通过 public C++ workflow API 执行。该 operation 接收两个 structured image Results 和一个 numeric Control Value，并在同一节点发布 typed image Result 与独立的 numeric `count` 输出。

## 构建与运行

在仓库根目录使用已配置的测试构建来编译和运行示例：

```sh
cmake --build build/kernel-dev --target photospider_unified_result_workflow
build/kernel-dev/examples/unified_result_workflow/photospider_unified_result_workflow
```

示例源码和 test-defined operation 位于本目录。作为外部 consumer 构建时，workflow 只使用已安装的 public headers 和 kernel target；不会加载 production image operation。

运行成功时会输出：

```text
count=32 frame=1 layer=0 y=1 x=2 before=1012 after=6013
```

## 数据流

输入 schema `example.image` 包含一个 `pixels` image slot，有两个 frames 和两个 layers。每个 frame/layer 使用 Float32 `{height=2, width=4}` sample domain。Source fixture 为每个 sample 赋值 `1000 * frame + 100 * layer + 10 * y + x`；第二个 source 额外加 5000。

```text
Result A ─┐
Result B ─┼─ example.gather ── image : Result
Control ──┘                 └─ count : Int64 Value
```

`image` output 只读取 Control 选中的 samples。最低位选择 A 或 B，其余位用于平移 source x 坐标。Operation 将 Control 与选中 image samples 记录在 Result relation 中，因此后续编辑沿同一 dependency evidence 转置。未读取的 Control sample 不进入 read set。

Public workflow 声明两个具名 outputs。完整执行结果将 `image` 按 workflow output name 保存为 `ResultRef`，并将 `count` 保存为 numeric Value。图像通过 captured `ResultDescriptor` 和 `ResultRef::read_image` 读取；frame 与 layer 使用显式坐标。

`main.cpp` 运行完整示例：编译 graph、执行两个 outputs、为一个 image sample 打开 demand handle、读取该 sample、修改 selector、替换 bindings，再次请求相同 coverage。程序检查返回值和 dirty Region 后打印结果。

## Sparse 请求与编辑

`DemandQuery` 将 output name 映射到该 output 逻辑 shape 内的 `Footprint`。`ExecutionContext::execute_fragments` 只计算请求 coverage，holes 仍未授权。`DemandHandle::replace_bindings` 为新的不可变 binding generation 比较实际消费的 source observations；修改已消费的 Control 可以切换到新 source samples，修改未消费的 Control sample 则保持 clean。

本示例刻意保持精简，展示 Result image ownership、混合 outputs、具名选择、N/L 坐标、sparse demand、动态 support 和 dirty transpose。当前 production image 可用性见[图像 operations](../../docs/kernel-architecture/zh/Image-Operations.zh.md)。
