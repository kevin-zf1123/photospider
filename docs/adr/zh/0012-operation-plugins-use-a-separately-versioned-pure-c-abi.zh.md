# ADR 0012：Operation 插件使用版本化 C 契约

- 状态：已接受
- 实现说明（2026-10-20）：纯 C 边界保持为已接受决策。Package 0.39.0 实现 operation module ABI 8 与 fixed invocation ABI 8。[ADR 0022](0022-freeze-operation-c-abi-v3.zh.md) 保持为历史 Proposed continuation 设计；其 staged record 和 prefix negotiation 提议不是当前 ABI。

## 1. 核心摘要

Operation 插件通过版本化 C 接口跨越 kernel 边界。Host 在发布 registry entry 前校验并复制声明。C module 与 C++ operation 使用相同的静态 lowering、物理 Plan 校验和固定 invocation 契约。Native plugin 仍处于 host 进程及其信任域内。

当前契约见 [Plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md) 与 [Compiler、Plan 和 Executor](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)。实现状态不改变完整 physical-execution 提议的正式决策状态。

## 2. 所有权与阶段

启动加载先校验 module table，再将 trait、schema 与参数契约复制到私有 registry candidate。发布是原子的；compiler 与 executor 使用前冻结 registry。

静态 preparation 特化 schema，捕获不可变 state 和声明的 resource。Lowering 构造固定 task、read、write、placement 和 scratch，不接触 sample payload。显式执行 preparation 链接 kernel 与 native pipeline，不改变拓扑。CPU invocation 结束全部访问后才返回；已接受的 GPU invocation 持有借用 view，直至恰好一次 terminal completion。

只要 definition、static preparation 或 prepared kernel 仍需要 module code，动态库就保持映射。Release callback 先于 module destroy callback 与 native unload。Prepared kernel 持有静态 state 与 resource；Run 在取消排空期间保留 preparation。Query/service record 仅在回调内借用，需要保留的数据必须复制到 provider 自己的 state。

## 3. 契约规约

```c
#include "photospider/plugin/operation_module_api.h"

PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void);
```

独立 C11 header 定义 exact-size module ABI 8，并包含 fixed invocation ABI 8。OperationTraits 为 30。Native shader preparation 使用独立版本化的 native kernel service ABI 2。当前没有 data-provider C table。

Module 声明 operation 契约，以及 static prepare、resource validation、lowering、kernel preparation 回调。Host 校验版本与 record size、alignment、pointer/count pair、有界字符串/数组、封闭 enum/flag、checked arithmetic、必要回调及清理所有权。先校验 module header，再接管 destroy hook 并读取嵌套记录。全部声明在私有 candidate 中验证；非法声明、重复 key 或 freeze 竞争不会发布部分 registry 更新。

Lowering 分别记录 schema-only 与 payload read、唯一输出 producer、私有 SSA 中间值、显式 COPY，以及每个 effectful call 恰好一个 terminal effect task。Kernel preparation 校验实际编译得到的 invocation view、参数、scratch 和 placement，随后返回 unchecked 直接入口。执行期间不进行 continuation polling、运行时依赖扩展、publication grant、数值认证或 backend retry。

异步提交被拒绝时，owner 留在 coordinator，不产生 callback；接受后保留 input/output/scratch、参数及 module owner，直到匹配 completion 确认停止访问与可见性，取消也遵循这一规则。Completion 可以 inline 发生。Callback 与 release function 遵守不抛异常契约；非法 completion key 不能结算其他 submission。

## 4. 边界

ABI 校验不提供 sandbox、签名校验、package 准入或 crash isolation。Native code 仍可能使 host 崩溃或挂起。Daemon IPC 不将 operation-plugin 加载暴露为能力。

当前 table 没有 ABI 1–7 getter、prefix-size adapter、staged continuation shim 或自动转换。外部 consumer 必须使用匹配 header 与 package 重建。

## 5. 后果

每个公共边界维护独立兼容版本。Module 不匹配时，在 registry 发布前拒绝加载。Compiler artifact 保存静态契约与物理表，不包含 native pointer；artifact import 与显式 device preparation 保持分离。内核开发阶段暂停安装消费测试，通过 focused C11 module、loader、preparation 和固定执行检查验证该边界。
