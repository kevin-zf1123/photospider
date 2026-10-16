# ADR 0017：以有界 CPU 存储执行区域工作

- 状态：Accepted
- 英文权威文档：[English](../0017-cpu-regional-execution-and-storage.md)

## 1. 核心摘要

每个 Result 成员都有静态 tile 网格。Tile 是 payload 调度、授权、发布、缓存和失效的单位。Continuation 是执行实体，可以跨多次 poll、请求多个输入 tile，并通过共享 stage 生产多个结果 tile。完整覆盖是普通的紧凑 tile 选择。

## 2. 模型与边界

一个 Run 固定 plan、binding 和 generation。普通输出 actor 发布一个完整成员 tile。准备后的共享 stage 在 Run 内按原始节点/stage 执行一次全局算法；等待者暂停并释放执行 worker。状态、等待记录、终态和 backing 都计入 Root。活动 continuation 不跨 Run。

## 3. 正式契约与 API

`ExecutionContext::execute` 返回具名 Result；`execute_fragments` 和 demand handle 选择成员局部 tile 集合。`ExecutionOptions::result_publication` 观察已认证发布，并可保留其 owner。不提供基于 Tensor 的 streaming sink 接口。

Plan-wide tile extent 是正二次幂，默认 128。空间网格切分 H/W 并覆盖其余 cell 轴；非空间网格切分最后两个非 atomic cell 轴。显式 batch 轴和更前面的自由 cell 轴形成坐标，atomic 轴保持完整。见 [ADR 0023](0023-track-dependencies-between-fixed-tiles.zh.md)。

Tensor 提供统一的不可变 backing 和 view 映射。宿主默认 tiled planar 分配，使用实际边缘尺寸和稀疏目录，无需预留完整逻辑画布。拥有型读窗口和受限写窗口暴露实际 stride。负 stride、broadcast 和外部 buffer 经结构校验后仍合法。稠密收集及 native packing/transfer 都显式准入容量并计费。

普通 CPU callback 使用 range 服务 ABI 2，分阶段协调器使用 tile 服务 ABI 2。资格取决于 callback 角色和 worker 配额，不取决于覆盖大小。禁止嵌套提交，每次服务在返回前等待工作完成。Kernel guard 保存/恢复浮点状态，算子在实际执行线程建立数值模式。

## 4. 非目标与明确边界

不提供独立的整体结果模式、默认布局/straight-alpha 认证、自动稠密复制、动态 field extent 或活动 backing 逐出重算。资源上限约束受控容量和 work，不等于进程 RSS。Native module 仍是受信任的进程内代码。

## 5. 影响

空 payload 请求合法且不启动生产；descriptor-only 请求必须显式提出。越界 tile 索引失败。取消停止新准入并排空已进入 callback。失败 tile 事务不公开部分 payload 或依赖证据。全局算法可能因一个 tile 请求而需要完整输出工作和容量；准备后的共享 stage 明确这项成本并避免逐 tile 重复计算。返回 Result 和窗口保留 backing 租约，直到最后 owner 退役。
