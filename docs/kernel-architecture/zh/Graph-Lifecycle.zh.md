# 图生命周期与变更语义

本文档定义图/运行时所有权，以及图加载、reload、编辑和 clear 操作的失败行为。

## 运行时所有权

`Kernel` 拥有从图名称到 `GraphRuntime` 实例的映射。每个 `GraphRuntime` 只拥有一个 `GraphModel`、事件服务、调度器映射、worker 状态和平台 context。

```text
Kernel
  graph name -> GraphRuntime
                  -> GraphModel
```

图和运行时应被视为一对一的所有权单元。

## 新图加载

加载新图应创建新的 `GraphRuntime`，并且只有 YAML 验证成功后才通过 `Kernel` 暴露它。

如果 YAML 中任意节点非法、缺少必需字段或产生环，加载必须返回错误，并且不得暴露部分加载的图。

期望行为：

```text
parse YAML -> validate all nodes/topology -> create/commit runtime
                                      \-> on failure: return error, expose none
```

在失败节点之前部分保留有效节点不是期望行为。

## 现有图 Reload

Reload 现有图更敏感，因为它操作的图名称可能已经可见。目标方向是在 reload 失败时避免半清空或部分重建的模型状态。

选择的行为：失败的 reload 保留之前的图。Reload 会在提交到可见 `GraphModel` 之前验证替换模型。

## 节点 YAML 替换

节点 YAML 替换应在验证失败时保留旧节点和图。

至少，替换必须解析新节点，并在解析或字段验证失败时保留旧节点。拓扑验证也应在提交前发生，使替换不能引入环或断裂依赖。

替换会在提交前验证候选拓扑。如果解析、依赖验证或环验证失败，之前的节点和图保持可见。

## GraphModel Clear

`GraphModel::clear()` 应重置模型级运行时状态，而不只是删除 `nodes`。

Clear 应重置：

- 节点映射
- 计时结果
- 累计 IO 时间
- skip-save 状态
- 其他可能影响后续加载或计算的单次运行模型状态

这让 reload 和 clear 行为更容易推理，并避免陈旧元数据附着在空图上。

## 错误表面

图加载、reload 和编辑失败应通过内核和交互层 API 可见。前端不应需要从部分变化的图中推断失败。

