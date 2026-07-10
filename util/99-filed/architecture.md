```mermaid
graph TD
    subgraph UserInterface [用户交互层]
        direction LR
        CLI["<b>graph_cli (REPL)</b><br><i>cli/graph_cli.cpp</i>"]
        TUI["<b>TUI Editors</b><br><i>ftxui, e.g., cli/node_editor.cpp</i>"]
    end

    subgraph ServiceFacade [公共 Host 与嵌入式适配层]
        Host["<b>ps::Host</b><br><i>photospider/host/host.hpp</i><br>frontend/CLI/TUI 的公共接口"]
        EmbeddedHostAdapter["<b>embedded Host adapter</b><br><i>src/host/embedded_host.cpp</i><br>公共请求到内部后端的适配器"]
        InteractionService["<b>InteractionService</b><br><i>src/kernel/interaction.hpp</i><br>仅供 adapter/backend 使用的内部 facade"]
    end

    subgraph CoreKernel [核心内核]
        Kernel["<b>Kernel</b><br><i>kernel/kernel.hpp</i><br>管理多个图实例 (GraphRuntime)"]
        PluginManager["<b>PluginManager</b><br><i>kernel/plugin_manager.hpp</i><br>加载和管理插件"]
        GraphRuntime["<b>GraphRuntime</b><br><i>kernel/graph_runtime.hpp</i><br>单图运行时：GraphStateExecutor、事件、<br>scheduler 注册/生命周期与平台资源；<br><b>不拥有通用线程池或 compute queue</b>"]
        SchedulerRuntime["<b>IScheduler / SchedulerTaskRuntime</b><br><i>kernel/scheduler/*</i><br>接收 ready task callback；具体 scheduler<br>拥有策略相关 worker 与 queue"]
    end

    subgraph GraphServices [图服务层]
        direction TB
        ComputeService["<b>ComputeService</b><br><i>kernel/services/compute_service.hpp</i><br>执行节点计算逻辑"]
        GraphIOService["<b>GraphIOService</b><br><i>kernel/services/graph_io_service.hpp</i><br>YAML加载/保存"]
        GraphCacheService["<b>GraphCacheService</b><br><i>kernel/services/graph_cache_service.hpp</i><br>磁盘与内存缓存管理"]
        GraphTraversalService["<b>GraphTraversalService</b><br><i>kernel/services/graph_traversal_service.hpp</i><br>仅负责拓扑排序与邻接查询"]
    end

    subgraph DataModel [数据模型]
        GraphModel["<b>GraphModel</b><br><i>graph_model.hpp</i><br>图的数据容器 (节点集合)"]
        Node["<b>Node</b><br><i>node.hpp</i><br>单个操作节点；只持有<br><b>formal HP cache 与版本</b>"]
        RealtimeProxyGraph["<b>RealtimeProxyGraph</b><br><i>compute-service/realtime_proxy_graph.hpp</i><br>持有 transient RT proxy/cache state；<br><b>不是 formal reusable cache</b>"]
        DataStructs["<i>ps_types.hpp / image_buffer.hpp</i><br><b>ImageBuffer</b>, <b>NodeOutput</b>, <b>TileTask</b>"]
    end
    
    subgraph Operations [操作层]
        OpRegistry["<b>OpRegistry (Singleton)</b><br><i>ps_types.hpp</i><br>注册中心, 支持多种实现<br><i>(Monolithic, Tiled HP/RT)</i><br>和<b>脏区传播函数</b>"]
        BuiltinOps["<b>Built-in Operations</b><br><i>src/ops.cpp</i>"]
        PluginOps["<b>Plugin Operations</b><br><i>custom_ops/*.cpp, src/metal/*.mm</i>"]
    end

    subgraph AdaptersAndDependencies [适配器与依赖]
        direction LR
        OpenCV["<b>OpenCV</b>"]
        Metal["<b>Metal (Apple)</b>"]
        FTXUI["<b>FTXUI</b>"]
        YAMLCPP["<b>yaml-cpp</b>"]
        BufferAdapter["<b>BufferAdapter</b><br><i>adapter/*.hpp</i><br>ImageBuffer <=> cv::Mat/MTLTexture"]
    end

    %% 连接关系
    CLI -- "调用公共 API" --> Host
    TUI -- "调用公共 API" --> Host
    Host -- "由进程内实现承接" --> EmbeddedHostAdapter
    EmbeddedHostAdapter -- "委托内部请求" --> InteractionService

    InteractionService -- "委托调用" --> Kernel
    
    Kernel -- "创建/销毁" --> GraphRuntime
    Kernel -- "使用" --> PluginManager
    Kernel -- "构造/调用内部服务" --> ComputeService

    PluginManager -- "加载" --> PluginOps
    
    GraphRuntime -- "持有" --> GraphModel
    GraphRuntime -- "持有 transient RT state" --> RealtimeProxyGraph
    GraphRuntime -- "注册并管理生命周期" --> SchedulerRuntime
    ComputeService -- "委派 ready tasks" --> SchedulerRuntime
    ComputeService -- "使用事件/RT proxy/调度器注册" --> GraphRuntime
    
    ComputeService -- "操作" --> GraphModel
    GraphIOService -- "操作" --> GraphModel
    GraphCacheService -- "操作" --> GraphModel
    GraphTraversalService -- "操作" --> GraphModel

    ComputeService -- "查找算子" --> OpRegistry
    
    GraphModel -- "包含多个" --> Node
    Node -- "使用" --> DataStructs
    RealtimeProxyGraph -- "使用" --> DataStructs
    
    BuiltinOps -- "注册" --> OpRegistry
    PluginOps -- "注册" --> OpRegistry
        
    ComputeService -- "通过Adapter使用" --> BufferAdapter
    BufferAdapter -- "封装" --> OpenCV
    BufferAdapter -- "封装" --> Metal
    
    GraphIOService -- "读/写" --> YAMLCPP
    TUI -- "构建UI" --> FTXUI

    %% 样式
    style UserInterface fill:#e6f3ff,stroke:#333,stroke-width:2px
    style ServiceFacade fill:#d4edda,stroke:#333,stroke-width:2px
    style CoreKernel fill:#fff3cd,stroke:#333,stroke-width:2px
    style GraphServices fill:#fce8d5,stroke:#333,stroke-width:2px
    style DataModel fill:#f8d7da,stroke:#333,stroke-width:2px
    style Operations fill:#d1ecf1,stroke:#333,stroke-width:2px
    style AdaptersAndDependencies fill:#e2e3e5,stroke:#333,stroke-width:2px
```
