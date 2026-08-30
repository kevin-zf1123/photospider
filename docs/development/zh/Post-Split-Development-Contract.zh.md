# 拆仓后开发契约

## 目的

本文是 IPC v2 daemon 拆仓后持续维护的 repository、version、preset 与 CI contract。
它定义 K0 开发基线；它不声称未来 typed-compiler capability 已实现。

路线排序以[拆仓后路线图 v3](../../roadmap/zh/Next-Stage-Execution-Plan.zh.md)为权威。
当前 runtime behavior 仍以 `docs/kernel-architecture/` 为权威。

## 仓库 ownership

| 关注点 | Photospider kernel | photospider-daemon |
| --- | --- | --- |
| Embedded Host/runtime | owner | 只做 installed consumer |
| Operation runtime、plugin SDKs、package | owner | 消费必要 components |
| WorkflowDocument/compiler IR/planner | future owner | 永不是 internal-schema authority |
| IPC v2 client/protocol/transport/router/registries | 非 owner | compatible-maintenance owner |
| `photospiderd` lifecycle 与 daemon tests/docs | 非 owner | owner |
| Job/worker/policy/trust/isolation/evidence | owner，保留 | 非 owner |

拆仓已完成。新的 daemon-owned client/protocol/transport/router/registry/lifecycle 工作在
daemon 仓提 Issue。Mixed 工作拆为相互链接的 focused Issues，不保留一个跨仓 authority。

## 独立版本轴

| 版本轴 | K0 值 | Generated compatibility | Consumer pin |
| --- | --- | --- | --- |
| Photospider CMake package | 0.1.0 | `SameMinorVersion` | daemon 要求 `0.1.0 EXACT` |
| PhotospiderDaemon CMake package | 0.1.0 | `SameMinorVersion` | installed client 要求 exact Photospider 0.1.0 runtime |
| Local IPC wire | v2 | exact protocol-v2 surface | exact v2 admission/method inventory |

0.x 开发的 same-minor package compatibility 刻意窄于 same-major。Daemon exact dependency 更窄，并且
fail closed。Package match 不代表 wire match，wire match 也不使 compiler schema 兼容。

未来 WorkflowDocument、IR、planner、digest、plan-cache 和 operation-trait 版本在 #245 中独立决定。
它们不是 package 或 IPC 版本。

## Kernel configure presets

运行 `cmake --list-presets` 查看持续维护的 presets。

| Preset | 用途 | 默认 closure |
| --- | --- | --- |
| `kernel-dev` | embedded kernel/runtime 和长期 dependency-neutral tests | Job、CLI、optional providers/plugins、OpenEXR、fuzzers 关闭 |
| `op-dev` | operation runtime/SDK 迭代 | tests 和 optional large products 关闭 |
| `legacy-full` | 历史 full developer/product validation | tests、CLI、OpenCV/YAML provider/plugin surface、Job 显式开启 |

普通 CMake 默认也把 single-tenant Job 设为 `OFF`。它的 option、implementation 与长期 tests
仍可在显式启用时使用。本契约不创建 `heavy-evidence` architecture 或 placeholder option。

典型命令：

```bash
cmake --preset kernel-dev
cmake --build --preset kernel-dev

cmake --preset op-dev
cmake --build --preset op-dev

cmake --preset legacy-full
cmake --build --preset legacy-full
ctest --preset legacy-full --output-on-failure
```

## Kernel CI contract

Kernel PR 与维护分支 push 验证：

- whitespace 与 exact checkout health；
- 三个持续维护的 preset configure path 及比例适当的 builds；
- 一个显式 legacy-full Job-enabled producer build；
- 六个长期 build/package smoke consumers；
- `unit`、`integration`、`verification` CTest labels。

Kernel CI 不 checkout `photospider-daemon`，不使用 private overlay content，不把 migration residue
注册为 CTest。它拥有 kernel、operation、未来 compiler、package 与 installed-consumer signals。

只有 kernel PR 修改 installed public API、component/export/package contract、package version tuple，
或 release gate 显式要求时，才请求 daemon downstream validation。Internal compiler-only change 不会把
daemon 变成每个 PR 的 gate。

## Daemon downstream contract

Daemon PR 与维护分支 push 使用 exact supported post-split kernel revision，并单独保留 archived
full-stack revision 作为 four-cell 旧侧。它们验证 daemon、client、server、install、installed consumer、
layout/RPATH、lifecycle、ownership 与 old/new interoperability。

每周/manual Ubuntu job 从 isolated installed prefix 检查 current kernel `main`。它是 maintenance drift signal；
既不更改 pinned PR tuple，也不阻塞每个 kernel PR。详细 daemon support matrix 位于该仓
`docs/Version-and-CI-Compatibility.md`。

## Typed-compiler 交接约束

K0 把未来开发交给 #245 -> #199 -> #200 -> #201/#202。在这些 Issues 落地前：

- operation ABI v1 保持 exact-size pure-C contract；
- traits 将来可使用 engine-owned registry 或 versioned sidecar；
- 无 traits 的 ABI-v1 plugin 为 `Unknown`，只允许 conservative no-opt lowering；
- trait v1 只包含 purity/side effects、determinism、cacheability、shape inference、Region/halo、
  static/dynamic inputs、supported candidates 和 fail-closed unknown；
- time/media/numeric/fusion/in-place/materialization 只保留 extensible identity；
- 第一版 WorkflowDocument 只包含一个 function、一个 region、一个 block，且无环；
- internal WorkflowDocument/IR/planner state 永不暴露给 daemon。

#194 Host 工作与无关 peripheral cleanup 与 #199 保持分离。K0 不实现 #199、#200、#201 或 #202。

## 维护流程

任何基线变更都要同时更新：

1. CMake option/package/preset source；
2. 受影响的 kernel 与 daemon CI；
3. 本英文 authority 与中文 mirror；
4. Roadmap/OpenSpec/tracking tasks；
5. live Issue dependencies 与 Project fields；
6. scoped tests，随后执行仓库测试政策允许的唯一一次 final clean verification。

事实声称必须命名 exact revision、command、environment 和 observed result。Future targets 在出现
implementation evidence 前保持未来时态。
