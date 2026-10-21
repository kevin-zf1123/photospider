# 编译工具文件格式

驱动使用严格 UTF-8 JSON 读取和写出 DAG 及 manifest，要求 `format` 和整数 `version: 1`。Tensor 二进制文件使用小端标量表示。`.psir`、`.psxp` 使用独立的 v5 文本格式。参见[工具用法与边界](Compiler-Tools.zh.md)。

## DAG JSON

下面是 CLI 测试 fixture 中外部算子的完整文档。编译前需要注册算子并提供其资源。

```json
{
  "format": "photospider.dag", "version": 1, "schema_version": 7,
  "inputs": [{"id": 1, "name": "x", "schema": {
    "id": "fixture.vector", "version": 1,
    "tensors": [{"key": "samples", "dtype": "f64", "shape": [4]}]
  }}],
  "nodes": [{"id": 10, "operation": "fixture.resource_scale",
    "inputs": [{"input": 1}],
    "parameters": {"sha256": "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"}
  }],
  "outputs": [{"name": "result", "node": 10, "port": "value"}]
}
```

输入引用是 `{"input": declaration_id}` 或 `{"node": call_id, "port": output_key}`。ID 使用精确 uint64，库要求非零、定义唯一且依赖合法。输入数组顺序定义端口顺序，输出名称唯一。驱动最多接受 65536 nodes 和 4096 inputs/outputs；前端继续执行调用引用与 chain 数量限制。

Schema 必须有 `id`、`version`、`tensors`。每个 Tensor 必须有 `key`、`dtype`、正 `shape`。可选 `batch` 默认 `[]`、`atomic` 默认 0、`layout` 默认非空间。完整 sample shape 为 `batch + shape`，rank 1..8。空间布局示例为 `{"spatial":true,"height_axis":0,"width_axis":1,"channel_axis":2}`，轴索引针对 cell shape；channel 可省略或为 null。非空间布局为 `{"spatial":false}`，拒绝轴字段。可选 `publication_scopes` 示例为 `[{"policy":"complete_bundle","members":[0,1]}]`；其他 policy 为 `independent_chunks`、`stable_prefix`。内核继续校验结构和 scope 约束。

支持 dtype `u8`、`i8`、`u16`、`i16`、`i64`、`f32`、`f64`，字节宽度依次为 1、1、2、2、8、4、8。不隐式转换端序、shape、dtype、图像或颜色。

参数可以直接使用字符串、布尔、int64 整数字面量或浮点字面量。未标类型的整数参数超过 int64 时拒绝；超出 int64/uint64 解析范围的整数在任何位置均拒绝。显式类型形式如下：

```json
{
  "exact_integer": {"type": "i64", "value": "9007199254740993"},
  "real": {"type": "f64", "value": 1.0},
  "negative_zero": {"type": "f64_bits", "value": "8000000000000000"},
  "label": {"type": "string", "value": "example"},
  "enabled": {"type": "bool", "value": true}
}
```

Typed f64 拒绝无法精确表示的整数 token；需要浮点舍入时使用显式浮点字面量。`f64_bits` 必须恰好 16 个十六进制字符，可保留任意 binary64 位模式，仍受算子参数契约约束。禁止 JSON NaN/Infinity token。未知字段、重复键与非法 Unicode 均报错。

## 静态资源

```json
{"format":"photospider.resources","version":1,"resources":[
  {"path":"scale.bin","byte_length":1,
   "sha256":"084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"}
]}
```

此例 `scale.bin` 包含单字节 `03`。相对路径以 manifest 目录为基准。SHA-256 为 64 位小写十六进制文本。驱动校验实际长度与哈希，再向静态 preparation 提供不可变资源 owner。Artifact 只保留选中的 identity，不保存本地路径或字节；再次加载时需要重新提供资源。

## 请求

```json
{"format":"photospider.requests","version":1,"outputs":[
  {"name":"result","members":[{"slot":0,"intervals":[[1,2]]}]}
]}
```

区间是成员局部 tile index 的半开区间，不是 sample 坐标。`slot` 为从零开始的 Tensor 序号。输出名称以及各输出内的 slot 必须唯一。重叠/相邻区间归一化，反向或超出网格的区间失败。空成员集合或空区间列表选择零 payload。整个 manifest 最多 65536 个原始区间。对于 shape `[4]`、无 atomic 轴、tile extent 2，`[1,2]` 选择 sample offset 2、3。`psc` 计算依赖闭包；`psx` 拒绝超出保存覆盖的请求。

## 运行绑定

```json
{"format":"photospider.bindings","version":1,"inputs":[
  {"name":"x","tensors":[
    {"slot":0,"path":"input.bin","encoding":"raw-le","dtype":"f64","shape":[4]}
  ]}
]}
```

例如 Python `struct.pack('<4d', 1, 2, 3, 4)` 产生 32 字节输入。路径以 manifest 目录为基准，raw 文件路径不能为 `-`。每个 Plan 输入和每个 Tensor slot 必须恰好出现一次。字节长度严格等于完整 sample shape 乘积与 dtype 宽度之积，并检查溢出。数据是逻辑 row-major 连续排列。工具读取并验证完整文件，然后使用执行 context 的 Root 按保存的 Plan 契约建立绑定。`DirectTiles` 分别分配每个所需 dense Host tile，要求零 byte offset 和匹配的编译容量；未请求的 tile 不保留。`DenseHost` 在 member 有任何所需 tile 时保留一份完整的 dense member allocation；Plan 的显式 COPY task 在执行期间物化独立的选定 tile。不兼容的物理 layout 在 Run 提交前失败，检查模式同样拒绝。C++ `PlanningOptions::input_layouts` API 选择该契约；`psc` 使用默认的 DirectTiles 契约。

## Results

成功的 `psx` 创建新目录，包含 `manifest.json` 和生成的片段文件名。前述部分请求与资源 `03` 的结果示例：

```json
{"format":"photospider.results","version":1,"outputs":[
  {"name":"result","schema":{
    "id":"fixture.vector","version":1,"tensors":[
      {"key":"samples","dtype":"f64","shape":[4],"batch":[],"atomic":0,"layout":{"spatial":false}}
    ],"publication_scopes":[]},
   "tensors":[{"slot":0,"encoding":"raw-le","dtype":"f64","shape":[4],"fragments":[
     {"path":"output-0-tensor-0-fragment-0.bin","byte_length":16,"offset":[2],"shape":[2]}
   ]}]}
]}
```

二进制片段保存 9.0、12.0。Tensor 的 `shape` 是静态全形状，fragment 的 `offset`、`shape` 是真实覆盖坐标。片段顺序和拆分是实现细节；片段互不重叠，只覆盖已发布 samples，每片按其区域保存 row-major 小端连续字节。空覆盖不生成二进制文件。消费者必须读取 manifest，不应通过文件名或完整 schema 推断覆盖。Results 是输出文件，不是可恢复执行状态。

## IR 与 Plan 文本 v5

`.psir` v5 的前缀如下：

```text
psir 5
stage ir
parent "..."
producer "optimize" "1"
parameters { }
```

`parent` 为来源信息，初始前端/校验输出可为空。生产者的已解析选项使用类型化 parameter 语法。后续保存算子契约与资源 manifest、输入声明/绑定、calls、返回签名、entry、`form dependency_paths|control_flow` 和 blocks。原始及优化 IR 都允许两种形式。导入产生 `ValidatedIR` 并重新计算内容 identity；来源不参与计算。

`.psxp` 以 `psxp 5` 和实际输入 IR digest 的 `parent` 开头，随后保存执行模式、tile extent、lowering 后的 CFG 声明和选定输出请求。`physical` 节保存 entry、输入 ID、返回 schema ID 及以下有序表：

| 表 | 编码字段 |
| --- | --- |
| `schemas` | 结构化 Result schema |
| `layouts` | 元素类型、rank、字节偏移及八项 shape/origin/有符号 stride |
| `tiles` | Value、member、layout、producer、domain、tile index、容量与对齐 |
| `values` | 来源 IR value、schema、owner、kind、import 来源 ID、输入 layout 契约及有序 tile ID |
| `placements` | Backend、domain、profile 与 queue |
| `kernels` | Operation、symbol、contract 与 ABI 版本 |
| `tasks` | Kind、call/kernel/placement ID、读写、schema read、参数字节、显式依赖、前驱、后继与 scratch |
| `calls` | 来源 call、block、退休序号、effect task、task ID、export 及输入/输出绑定 ID |
| `blocks` | 来源 block、终止方式、condition、task/call/argument/live-in/return ID、边与资源预留向量 |

Value kind 为 Input（0）、Argument（1）、Produced（2）和 Imported（3）。输入 layout 契约为 DirectTiles（0）和 DenseHost（1）；非 Input value 使用 0。Imported value 的来源 IR value 为零，owner 为 block，`import_source` 为规范 Input ID；其他 kind 的该字段使用无 ID。Imported tile 具有显式 COPY producer。边界 import COPY 使用无 call ID，在全部真实 call 退休后执行。Loader 检查来源关系，并拒绝绕过 import 的 raw DenseHost payload 使用。

表 ID 从零连续编号，`4294967295` 表示无 ID。Task 表顺序不约束依赖顺序。Loader 保留 ID，独立校验邻接表、存活值、资源预留、布局边界及 IR 对应关系。保存的派生表必须与定义一致，加载时不修复。算子 preparation 从已校验 IR 和给定 registry/resources 重建，不调用 lowerer、optimizer 或 planner。保存的 IR 为 ControlFlow，允许不同于父 IR 的依赖路径形式。

产物文本上限为 64 MiB。物理表共享 4194304 条记录上限，物理引用共享 16777216 项上限。Task 参数字节与结构校验共同消耗独立的 268435456 单位 work 预算，不计为依赖边。导入与导出使用同一物理校验契约。

文本支持分号行注释和转义字符串，参数类型为 `i64`、`f64`、`f64_bits`、`bool`、`string`，编码保留 IEEE-754 位。精确有界语法见 `src/lib/compiler/artifact_text.cpp`，往返测试见 `tests/unit/kernel/test_physical_compiler.cpp`。v1–v4 拒绝并提示重新导出。Identity 检测内容变化，不认证代码或文件。Package、document schema、C ABI、artifact 版本是独立版本轴。
