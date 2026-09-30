# ADR 0012：Operation Plugin 使用版本化 C 契约

- 状态：已接受

## 1. 核心摘要 (TL;DR)
Operation plugin 和 data provider 通过独立版本化的 C 接口跨越 kernel 边界。Host 在发布 registry 记录前校验并复制 descriptor。这样 compiler 元数据不依赖 plugin 的 C++ 对象布局；native plugin 仍在 host 进程和信任域中运行。

## 2. 架构心智模型

```text
startup configuration --> load library --> validate exact table --> copy traits/schemas
                                                   |
                                      registry frozen for compiler and runs
                                                   |
                                          synchronous callback
                                                   |
                                      host validates/copies output
```

Library 自有的表在 destroy callback 执行时仍保持映射。Host 拥有复制后的元数据，并校验通用 `Value` callback 输出。Dependency program 可以保留经授权的输入 owner handle，直到显式释放或状态销毁；每次 poll 的输入/输出指针仍是借用。Planar callback 通过 host 所有的 row buffer 写入，GPU token 会将 allocation 保留到释放或 callback 结束。不同生命周期的完整说明见 [Plugin ABI](../../kernel-architecture/Plugin-ABI.md)。

## 3. 契约规约与接口

```c
#define PS_OPERATION_ABI_VERSION_11 11U
uint32_t ps_operation_plugin_get_abi_version(void);
const ps_operation_plugin_api_v11 *ps_operation_plugin_get_api_v11(void);
#define PS_DATA_PROVIDER_ABI_VERSION_1 1U
#define PS_PLANAR_OPERATION_ABI_VERSION_3 3U
uint32_t ps_data_provider_get_abi_version(void);
const ps_data_provider_api_v1 *ps_data_provider_get_api_v1(void);
```

Operation ABI v11 声明 operation trait、参数和端口 schema、callback、输出契约及 native GPU 服务。独立的 provider ABI v1 发布有界 data-schema 记录。完整结构与执行细节见 [Plugin ABI](../../kernel-architecture/Plugin-ABI.md)。

专用 planar operation 接口单独使用 ABI v3，与 operation ABI v11 一样拥有独立记录和执行契约，详见 [Plugin ABI](../../kernel-architecture/Plugin-ABI.md)。Staged dependency program 使用 host 分配的零初始化 continuation state，并在 `destroy` 中释放关联资源；definition/library lease 会覆盖该状态的生命周期。

Loader 会核对准确的 ABI 版本和结构尺寸、自然对齐、指针与计数配对、记录数量和 key 长度上限、严格 UTF-8 key、算术溢出、封闭 enum/flag 组合、必需 callback、定长输出的稠密可表示性、callback 返回值、输出字节/facet 上限，以及恰好一次的 destroy 所有权。它拒绝尾随结构字节。多记录更新采用 copy-then-swap，因此分配失败或后续记录无效时不会发布部分前缀。刚打开库时取得的 guard 会在可安全读取 destroy callback 后接管它，并在任何拒绝路径关闭库。Unload 前先销毁已发布的 plugin 表。

通用 operation callback 同步执行。输入 view 和输出 sink 仅在调用期间借用；接受的通用 Value 输出会在返回前复制或冻结。Sink 第一次发布尝试就占用该 sink，即使校验失败也一样；第二次尝试会留下 sticky violation。Host 先检查 cancellation，然后依次检查 sink 分配失败、malformed image output 和重复发布。若 sink 已经尝试发布后 callback 又返回 backend unavailable，则不会触发 fallback：已接受的输出变成 `OperationFailed`，第一次发布被拒绝时则返回 sink 保留的类型化错误。未知非零返回值变为 `OperationFailed`。只有 GPU attempt 在尚未尝试发布 sink 时明确报告 backend unavailable，且复制的 trait 允许时，才可请求 CPU fallback。

Planar callback 通过 host 所有的 row buffer 与 invocation-local scratch service 工作；它不会通过 operation sink 返回通用 Value。GPU allocation token 会将底层 owner 保留到显式释放或 callback 结束。

Dependency program 可以把授权的输入 handle 保留到释放或 continuation state 销毁；普通 service 指针只在同步 poll 调用期间有效。Registry 在 compiler 与 executor 使用前冻结。

## 4. 非目标与明确边界
- ABI 校验不是 sandbox、签名验证、包准入或崩溃隔离。
- Daemon 不把 operation plugin 作为 IPC 能力加载。
- Provider 记录描述语义 schema，不读取文件，也不创建持久 Value。

## 5. 后果与代价
版本或布局不匹配会在发布前拒绝加载/注册。格式错误的 descriptor 不会留下 registry 前缀。Callback 异常会在边界处拦截，但 native code 的崩溃或挂起仍会影响 host 进程。ABI 改动要求外部 consumer 使用匹配的 header 与 package 重新构建。
