# Unified Result 图像 workflow

本示例注册一个小型 test operation，并通过 public C++ workflow API 执行。三个输入和两个具名输出全部使用 `ResultRef`。Operation 根据 Int64 Control Result，从两个图像 Result 中选择 samples，发布稀疏图像 Result，并在同一节点发布独立的常量 `count` Result。

## 构建与运行

在仓库根目录使用已配置的测试构建编译并运行示例：

```sh
cmake --build build/kernel-dev --target photospider_unified_result_workflow
build/kernel-dev/examples/unified_result_workflow/photospider_unified_result_workflow
```

在已配置的 installed consumer 构建目录中编译示例与 installed consumer executable，然后运行两个 installed tests：

```sh
cmake --build build/kernel-dev/consumer-build \
  --target photospider_unified_result_workflow photospider_unified_result_consumer
ctest --test-dir build/kernel-dev/consumer-build \
  -R '^installed_unified_result_(workflow|cpp)$' --output-on-failure
```

运行成功时会输出：

```text
count=32 frame=1 layer=0 y=1 x=2 before=1012 after=6013
```

## Result 输入与输出

图像 schema `example.image` 包含一个 Float32 `pixels` tensor。它的 cell shape 为 `{2,4}`，batch axes 为 `{2,2}`，按 `{N,L,Y,X}` 排列的 full sample shape 为 `{2,2,2,4}`。Fixture 为每个 sample 赋值 `1000 * N + 100 * L + 10 * Y + X`；第二个图像 source 额外加 5000。

`control` 输入使用 schema `example.control`，包含一个名为 `samples`、shape 为 `{2,2,2,4}` 的 Int64 tensor。32 个值在被读取时必须处于 0..7。最低位选择图像 A 或 B，其余位按 `(selector >> 1) % 4` 平移 source x 坐标，并在四个像素的行内循环。

Operation 声明两个 Result outputs。`image` 使用图像 schema，保存所选 samples。`count` 使用 schema `example.count`，包含一个 shape 为 `{1}` 的 Int64 `samples` tensor。它发布常量 32，并声明没有 input dependencies，因此只请求 `count` 不会读取图像或 Control payload。

```text
Result A ─┐
Result B ─┼─ example.gather ── image : Result
Control ──┘                 └─ count : Result
```

对于每个被请求的 image output sample，Operation 记录一个 Control dependency 和一个所选图像 Data dependency。Operation 先以 `Control | Validation` roles（`6`）请求 Control tensor support，只校验实际读取的 selector 值；随后以 Data role（`1`）请求所选图像 samples。其 `sample_rows` relation 记录逐 sample 的精确 support。已消费 selector 改变时，所选 branch 和 source coordinate 可能改变；请求 coverage 之外的 selector 不会进入 read set。

## 稀疏请求与绑定修改

`main.cpp` 先执行两个 outputs，用 `ResultRef::read_tensor` 读取 `count`，再通过 `DemandHandle` 请求一个 image sample。请求的 full-sample 坐标为 `{N=1,L=0,Y=1,X=2}`。初始 selector 为 0，sample 来自 A，值为 1012。示例将 Control sample 22 改为 selector 3，替换不可变 bindings，并再次请求相同 coverage。Selector 3 选择 B 并将 X 平移一位，因此返回值为 6013。示例检查 `image` 的 dirty coverage 精确等于该请求 sample。集成测试另行验证先前的 dependency evidence 保持不可变，且不相关的 selector 修改保持 clean。

`DemandQuery` 将 output name 映射到 output full sample shape 中的 `Footprint`。`ExecutionContext::execute_fragments` 只计算请求的 coverage，并保留未授权的 holes。任一 output 的空 footprint 都会发布 descriptor metadata，tensor coverage 为零，也不会读取 sample payload。只请求 `count` 不受无效或未请求 selector 影响。若实际读取的 selector 不在 0..7 范围内，执行会失败。Metadata specialization 会在执行前校验固定的 image 和 Control schemas。

集成测试还验证 `ExecutionContext` 退出后已发布的 Results 仍可读取。目前 runnable example 和 `test_unified_result_images` 的本地验证通过；安装 consumers `installed_unified_result_workflow` 与 `installed_unified_result_cpp` 通过 2/2。这些检查覆盖 CPU Result 行为，不代表 GPU 执行。示例展示 Result ownership、两个具名 Result outputs、N/L 坐标、稀疏 demand、动态 support 和 dirty transpose。Production image operation 的可用性见[图像 operations](../../docs/kernel-architecture/zh/Image-Operations.zh.md)。
