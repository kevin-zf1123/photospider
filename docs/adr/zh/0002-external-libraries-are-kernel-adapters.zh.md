# ADR 0002：外部库留在 Kernel 契约之后

- 状态：已接受

## 1. 核心摘要 (TL;DR)
Kernel 通过编译器、数据、执行和插件契约提供接口，不暴露第三方库类型。Operation 实现可以在契约之后使用私有依赖。这使安装后的 kernel 无需可选库即可使用，也允许针对具体能力进行集成。

## 2. 架构心智模型

```text
consumer document / values
          |
          v
 Photospider public contracts <----> operation/provider ABI
                                          |
                                          v
                              implementation-private libraries
```

Kernel 拥有类型化 workflow 和执行边界。Plugin 拥有内部 adapter，并在 ABI 边界转换数据与错误。

## 3. 契约规约与接口

```cpp
struct WorkflowDocument;
class Value;
class Region;
class OperationRegistry;
struct ExecutionContextConfig;
```

这些公共 C++ 类型定义 kernel 面向调用方的数据模型。Result operation 与 data-provider 的 C 接口分别声明在 `photospider/plugin/result_operation_plugin_api.h` 和 `photospider/plugin/data_provider_api.h`。Operation plugin 通过定宽 Result 记录和借用缓冲区传递数据；第三方对象与异常留在 plugin 内部。安装包要求 C++ 运行时和 Threads；可选 native backend 与集成由构建选项控制。

## 4. 非目标与明确边界
- Kernel 不定义文件发现、文档解析、持久化、编解码、UI、网络或加密服务。
- ABI 校验互操作正确性，不隔离 native code，也不证明 plugin 可信。
- Plugin 不得通过公共 ABI 转移第三方 allocator 或库对象的所有权。

## 5. 后果与代价
Plugin 作者需要在边界处转换数据、错误和生命周期，并管理库级线程设置。这增加 adapter 工作，但避免库 ABI 和 allocator 决定 kernel 的兼容要求。启用可选集成的构建配置需要相应依赖。
