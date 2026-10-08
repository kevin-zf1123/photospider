# G4 依赖 workflow

此可执行程序通过公开 Result API 演示带依赖的 workflow。输入和算子输出都是 Result。`data` 场景使用逻辑长度为十亿、实际只发布两个样本的张量；其他场景覆盖渐进式 Need、共享执行、有序归约与扫描，以及块状态复用。generic `InputSnapshotStore` 检查中使用的小型 `Value` 只是该独立 API 演示的局部类型化 backing，不是 workflow 绑定。

程序有三种运行方式：

```sh
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow --radius-only
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow --scenario data
```

无参数时依次运行九个场景：`data`、`progressive`、`dynamic`、`demand`、`shared`、`radius retention`、`reductions`、`scan` 和 `blocks`。`--radius-only` 运行 `dynamic`、`demand` 与 `radius retention`。`--scenario` 可选择 `data`、`progressive`、`shared`、`reductions`、`scan` 或 `blocks`。

## 稀疏数据与依赖支持

`data` 场景声明一个包含十亿个逻辑样本的 Float64 张量，并绑定只在坐标 1 和 999999998 发布样本的 Result。一次 `execute_fragments` 调用仅请求这两个点。示例检查对应值，确认读取未物化的中间区域会失败，验证 `dependencies.source_support()` 与请求 footprint 相同，并检查 `potential_dirty()` 将输入样本变更映射到相同输出坐标。稀疏 Result 使用的 Root Payload 不超过 4096 字节。依赖关系表达逻辑支持范围，不代表存储读取次数或数据传输量。

## 渐进式 Control 与 Data Need

`progressive` 场景注册两个无输入 Result source operation 和一个分阶段的 `Follow` operation。Control source 在坐标 0、1、3 生成值；每个值选择下一处坐标，直到最后一个 Control 选择 payload 坐标 999999999。`Follow` 为 control 张量发出 Control role 2 的 ResultProgramNeed，为 payload 发出 Data role 1 的 Need，随后发布值 17.25，并同时记录历史 control 关系和最终 payload 关系。

这些 source operation 对请求样本总共生成 32 字节。示例将 Root Payload 上限设为 128 字节；实测峰值为 112 字节。生成字节数表示产生的 Result payload，不是物理读取流量。

## 动态编辑、按需请求与保留的 Result

`dynamic` 场景在绑定的 Result 上运行 `numeric.radius_scatter` 和 `numeric.radius_gather`。编辑 `radius[3]` 后，scatter 输出 0 从 1 变为 5，而 gather 输出 0 仍为 1。冻结的 execution 仍返回 scatter 值 1。依赖证据显示 radius 编辑使 scatter 输出 0 变脏、gather 输出 0 保持干净；新一代关系包含新选中的 data edge，旧冻结关系不包含该 edge。算子行为见[依赖采样契约](../../docs/kernel-architecture/Dependency-Sampling.md)。

`demand` 场景打开 context 所有的 handle，并请求稀疏 footprint `{0,4}`。它先后替换 radius 和 data 绑定，中间不重新计算，因此 dirty coverage 累积了两次替换的影响。最新结果为 `[10,14]`；冻结 bundle 仍返回 `[1,5]`。读取中间空洞会失败，示例还会释放对应的 query subscription。

`radius retention` 场景在保留首个 Result 时重复请求相同的冻结查询。第二次请求返回相同 Result ObjectId，且没有 operation timing。更换绑定代数后，即使 data 编辑位于查询范围之外且 dirty coverage 为空，执行仍会重新计算。之后编辑 radius 会改变 dirty footprint。清除 Result cache 后，保留的输出仍能报告精确的 data support。此场景演示冻结 Result 保留和依赖证据，不代表 radius operation 支持跨代内容缓存复用。

## 共享执行

`shared` 场景让两个精确 waiter 请求同一个不可变 Result bundle。注册的 identity operation 内有一个 barrier，确保第二个调用者加入正在执行的计算后，第一个调用者才被取消。第一个请求返回 `Cancelled`；第二个请求仍收到值 7 和完整依赖证据。即使关闭 Result retention，identity callback 也只运行一次。

## 有序归约

`reductions` 场景将无输入 Result source operation 接到 `numeric.mean` 和 `numeric.variance`。每次 source 调用生成 64 个 Float64 样本。192 个生成块共生成 98,304 字节，Root Payload 峰值为 624 字节，低于 1024 字节上限。对于循环值 `[0,1,2,3]`，独立检查得到均值 1.5、总体方差 1.25。两个输出都记录完整 4096 样本输入的 support。字节数是生成的输出 payload，不是物理读取量。标量归约使用有序输入累加器，不使用分块部分和。

## 有序扫描

`scan` 场景以 block size 16 计算 `[1,2,...,128]` 的全部 128 个 inclusive prefix。Result source operation 根据每次请求的连续 span 单调生成数据；示例检查每个样本只生成一次，并验证三角数结果以 8256 结束。

另一个绑定包含 `[1,+inf,...]`。请求 `{0}` 成功并返回 1；联合请求 `{0,1}` 则报告输入 1 的非有限值。这展示了按请求前缀边界执行的局部验证。

## 块状态复用

`blocks` 场景先对 `[0,1,2^54,4,5,6]` 运行 `numeric.ordered_scan`，再把首个绑定样本改为 1。首次执行有六次 block-cache miss。编辑后，前三个状态转移 miss，后三个因输入累加器状态重新汇合而 hit。到输入 1 的 prefix 值为 2，最终输出由 volatile binary64 左折叠独立检查。依赖 support 仍覆盖当前完整 source prefix。cache hit 会复用块结果，但算子仍须提供用于建立依赖的 Result Need。

## 构建与运行

构建仓库内可执行文件，然后运行全部演示或选择单个场景：

```sh
cmake --build build/kernel-dev --target photospider_dependency_workflow -j 8
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow
build/kernel-dev/examples/g4_workflow/photospider_dependency_workflow --scenario progressive
ctest --test-dir build/kernel-dev -R '^test_dependency_(workflow|radius_workflow)$' --output-on-failure
```

完整 workflow 测试运行无参数模式；radius 测试运行 `--radius-only`。

此程序也可作为已安装的 Photospider 0.30 包的 consumer 构建。consumer 测试项目还提供 `photospider_sampling_consumer`；dependency workflow 测试覆盖无参数模式和 radius-only 模式。

```sh
cmake --install build/kernel-dev --prefix "$PWD/build/kernel-dev/consumer-install"
cmake -S tests/consumer -B build/kernel-dev/consumer-build \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build \
  --target photospider_dependency_workflow photospider_sampling_consumer -j 8
ctest --test-dir build/kernel-dev/consumer-build \
  -R '^(installed_dependency_workflow|installed_dependency_radius_workflow)$' --output-on-failure
```

这些场景中的 Payload 峰值和上限只涵盖 Root Payload 计费，不代表进程 RSS，也不包含全部 metadata 或其他进程内存。本地 `test_dependency_workflow` 和 `test_dependency_radius_workflow` CTest 已通过；安装 consumer 的 `installed_dependency_workflow` 和 `installed_dependency_radius_workflow` CTest 也已通过。这些结果覆盖上述 CPU workflow 场景，不代表其他平台验证。
