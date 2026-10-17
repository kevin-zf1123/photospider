# ADR 0012：Operation Plugin 使用版本化 C 契约

- 状态：已接受
- 提议中的后继：[ADR 0022](0022-freeze-operation-c-abi-v3.zh.md) 以前缀协商取代精确大小规则，并让内置算子使用同一条 C 导入路径。在该提案接受并实现前，当前接口为 exact-size Result ABI 6。

## 1. 核心摘要 (TL;DR)

Operation plugin 通过版本化 C 接口跨越 kernel 边界。Host 在发布 registry 记录前校验并复制 descriptor，使编译元数据不依赖 plugin C++ 对象布局。Native plugin 仍在 host 进程和信任域内执行。

## 2. 架构心智模型

宿主加载库、校验完整表、复制 traits/schema，并在 compiler 与 Run 使用前冻结 registry。Module 的 destroy callback 在卸载前执行。Host 拥有复制后的元数据，校验 Need 和 Result 发布。Callback 服务表只借用到返回；显式保留的窗口和状态 handle 遵循自己的 release 生命周期。

## 3. 正式契约与 API

```c
#define PS_RESULT_OPERATION_ABI_VERSION_6 6U
const ps_result_operation_plugin_api_v6 *
ps_result_operation_plugin_get_api_v6(void);
```

该表声明 Result 端口、静态 Tensor 成员、成员区间 Need、共享 stage callback 和事务发布。`publish_tensor_view` 必须提供非 NULL 的显式 transform；`publish_tensor_view_pieces` 接受 1..16 条精确大小的 piece envelope，禁止在 pieces 中 reshape，并将完整、互斥的目标分区原子提交。单源 view 的 `reshape=1, source_rank=0, axes=NULL` 仍合法。跨 Root 发布视图保留来源 Result，而不在目标 Root 重复计入来源 Payload。当前不提供 data-provider C table。详细布局、C metadata sink 和 callback 契约见 [Plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)。

Loader 校验精确 ABI 版本和结构大小、自然对齐、pointer/count 配对、数量与 key 长度、UTF-8、受检运算、封闭 enum/flag、必需 callback、schema、参数和恰好一次的 destroy 所有权。多记录注册使用 copy-then-swap，失败不发布前缀。库 guard 在拒绝路径调用可安全读取的 destroy 后再关闭库。

运行时检查成员 tile Need、输入投影、授权、发布覆盖、冻结依赖证据、资源、取消和后端同步。普通 tile 必须一次完整发布；共享 stage 可以跨 poll 提交完整 tile。GPU token 与拥有型窗口保留 backing 到显式释放或相应 callback 退役。服务失败保持 sticky，不能通过 callback 返回成功掩盖。

## 4. 非目标与明确边界

ABI 校验不提供沙箱、签名认证、插件隔离或崩溃防护。Daemon IPC 不属于 operation ABI。数值和领域解释由算子负责，kernel 不根据 schema 或通道数绑定默认合规 validator。

## 5. 影响

版本或布局不匹配时在注册发布前拒绝。Callback 异常受边界栅栏处理，但 native 崩溃或挂起仍可能影响 host。外部模块必须按匹配的 package 和头文件重新构建，不提供旧 ABI 兼容层。ABI 6 不兼容 ABI 5 的记录大小或 getter，也不会对缺失的 view transform 推断隐式映射；ADR 0022 仍是未实现的 Proposed 后继。
