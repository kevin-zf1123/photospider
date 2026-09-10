# 阈值与连通组件

默认 registry 通过公开 WorkflowDocument/compile/execute 提供五个 CPU Whole 算子。
已接受契约见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)，
[英文文档](../Component-Operations.md)为权威实现说明。

| Key | 输入 | 输出 | 必填静态参数 |
| --- | --- | --- | --- |
| `mask.threshold` | 有限 Float32 `[H,W]` typed ScalarField | Float32 `[H,W]` canonical coverage mask，精确 0/1 | 有限 Float64 `threshold`，构造端默认 `.5` |
| `mask.components` | Float32 `[H,W]` canonical coverage mask，精确 0/1 | Int64 `[H,W]` typed labels | Int64 `capacity` |
| `component.count` | Int64 `[H,W]` typed labels | generic Int64 `[1]` | Int64 `capacity` |
| `component.area` | 同上 | generic Int64 `[capacity+1]` | Int64 `capacity` |
| `component.bbox` | 同上 | generic Int64 `[capacity+1,4]` | Int64 `capacity` |

全部轴非零。各节点 capacity 独立，范围 `[1,2^53-1]`，使用既有 exact bounded Int64
参数范围。构造端显式写入，registry 不补默认。共享输出 axes 对 capacity 加 checked
常量偏移一；IR/callback 发布前检查 dense 输出尺寸，执行预算先于分配接纳。

Threshold 比较 `sample>=threshold`，支持 signed 有限样本，threshold 采用输入字段
样本单位。它建立 dimensionless canonical coverage mask，移除原字段解释；比较使用
nearest/gradual-underflow 环境。Generic array/image 不因 shape 相同自动成为 scalar field。

Components 使用四邻域。Row-major 扫描为每个未访问前景组件启动队列，邻居入队时
立即标记，按首像素顺序从一编号，背景零。容量约束**最终连通组件数**，因此
`[[1,0,1],[1,1,1]]` 在 capacity=1 成功。Whole 需求覆盖完整 mask，规划 tile 不切断
连通性；首版无 connectivity 参数或八连通模式。

Label 描述为 canonical ScalarField、Int64 `[H,W]`，通道 name/role 均
`component_label`，整体/通道单位 `dimensionless`，无 capacity facet。属性端口要求
精确 facets。各消费者检查每个 label 位于自身 `[0,capacity]`；producer capacity=10、
实际只有 1/2，可接 consumer capacity=5。允许编号空洞，count 统计 distinct 非零 ID，
不是最大编号；相同 ID 的不连通像素不会被重新编号或拆分。

Area 按 ID 计像素数。Bbox 顺序为逻辑像素坐标
`x_min,y_min,x_max_exclusive,y_max_exclusive`。背景零号及未用记录全零；无前景时
属性表仍保持静态非零容量。Area/bbox 为 generic 整数数组，不复制 label facet。
保持每节点单输出，使用独立节点和命名输出取得多个属性。

读取通过 memcpy 与 logical coordinate，支持 byte offset、storage origin、signed/
zero stride 和未对齐视图。组件队列计量 `8*S` bytes；count 去重 hash 最多 `32*S`
bytes，均按输入 S 样本数，与 capacity 无关。Area/bbox 无独立 workspace。输出清零、
扫描、队列与 hash probing 检查取消；错误释放未发布输出/scratch，无部分成功结果。
负值/越容量 labels、非二值 coverage、area/坐标溢出为附样本索引的
OperationFailed；参数错误为 InvalidArgument，元数据不匹配为 TypeMismatch，取消/
资源错误保持自身代码；直接非法 typed 绑定遵循既有 preflight 错误契约。

## 公开 workflow 与检查

[test_component_operations.cpp](../../../tests/integration/test_component_operations.cpp)
的 run()/threshold_cases()/component_cases()/attribute_cases() 使用公开入口。
run() 的不可变 producer 支持合法 strided Value；普通 dense 输入可改成
WorkflowDocument 声明与每 Run ExecutionBindings。绑定 mask input 1 时：

```cpp
ps::WorkflowNode labels{
    1, "mask.components", {ps::WorkflowInputReference{1}},
    {{"capacity", std::int64_t{5}}}};
ps::WorkflowNode area{
    2, "component.area", {ps::WorkflowNodeOutput{1, "value"}},
    {{"capacity", std::int64_t{5}}}};
```

为 Float32 mask 使用 encode_semantic(coverage_semantics())；count/bbox 连接相同
labels，或在前方接收 typed Float32 ScalarField 的 threshold。静态 capacity/threshold
变化重编译；同 descriptor 的值变化使用每 Run 绑定。Region 依赖全部 Whole。

```sh
cmake --build build/issue257-static --target test_component_operations -j 8
ctest --test-dir build/issue257-static -R '^test_component_operations$' --output-on-failure
```

退出零检查无前景 labels/count 零及非空零表；bridge 为一个组件、area=5、bbox
`[0,0,3,2]`；3x3 棋盘按四连通稳定编号 1..5。稀疏 labels `[[0,2,2],[5,0,5]]`
得到 count=2、area `[0,0,2,0,0,2]`，ID2 bbox `[1,0,3,1]`、ID5 `[0,1,3,2]`；
capacity=4 拒绝 5。另覆盖独立容量、subnormal 非二值、巨大精确 ID、未对齐负/零
stride、取消和小预算。Static/shared 安装 consumer 用已安装公共 target 编译同一源码。
