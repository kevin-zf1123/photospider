# 基准 Spike

这些基准 spike 必须在收紧 Metal context 访问策略或改变可移植 CPU 行对齐要求之前完成。

## Metal 适配器访问

Metal buffer adapter 是已实现但当前未启用的候选路径。将它作为生产运行边界启用之前，需要比较它的 API、当前后端特定 Metal operation 路径，以及直接解释后端 `context` 在以下场景中的表现：

- 16x16 和 64x64 tile
- copy、add、curve transform 等轻量操作
- 上传/下载密集工作负载
- 适配器开销可能影响 RT 延迟的混合 CPU/GPU 调度路径

在 adapter 接入构建且该基准产生数据之前，不要把 adapter API 描述为生产 Metal 路径。直接访问 `context` 仍是后端特定且实现敏感的行为。

## ARM Mac 对齐

在 Apple Silicon 上比较必需的 64 字节行对齐和可选 128 字节对齐模式：

- 按行 SIMD 内核
- 分块 HP 操作
- RT tile 更新
- 内存使用和 padding 开销

目前尚未记录基准结果。后续 OpenSpec change 应捕获实现细节、脚本、原始结果，以及 128 字节对齐是变为可配置项还是仅保留为实验的决策。
