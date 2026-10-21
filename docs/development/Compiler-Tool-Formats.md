# Compiler tool file formats

The driver reads and writes strict UTF-8 JSON for DAGs and manifests, with `format` and integer `version: 1` tags. Binary Tensor files use little-endian scalar representations. `.psir` and `.psxp` use the independent version-5 text codec. See [tool usage and bounds](Compiler-Tools.md).

## DAG JSON

This complete document describes one external operation from the CLI test fixture. The operation must be registered and its resource supplied before compilation.

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

Input references are either `{"input": declaration_id}` or `{"node": call_id, "port": output_key}`. IDs are exact unsigned 64-bit integers; library validation requires nonzero, unique definitions and valid dependencies. Input-array order defines operation port order. Output names are unique. The driver accepts at most 65536 nodes and 4096 inputs/outputs; the frontend applies its additional call-reference and chain limits.

A schema requires `id`, `version` and `tensors`. Each Tensor requires `key`, `dtype`, positive `shape`. Optional `batch` defaults to `[]`, `atomic` to 0, and `layout` to nonspatial. Full sample shape is `batch + shape`, rank 1..8. Optional layout is `{"spatial": true, "height_axis": 0, "width_axis": 1, "channel_axis": 2}`; axes refer to the cell shape. A channel axis may be omitted or null. A nonspatial layout is `{"spatial": false}` and rejects axis fields. The schema may include `publication_scopes: [{"policy": "complete_bundle", "members": [0,1]}]`; other policies are `independent_chunks` and `stable_prefix`. The kernel validates structural and scope constraints.

Supported dtypes are `u8`, `i8`, `u16`, `i16`, `i64`, `f32`, `f64`, with widths 1, 1, 2, 2, 8, 4, 8 bytes. There is no implicit endian, shape, dtype, image or color conversion.

Parameters can be strings, booleans, signed-64 integer literals or floating literals. Integral JSON values outside signed-64 range are rejected as untyped parameters; values outside signed/unsigned-64 parsing ranges are rejected everywhere. Explicit forms preserve the requested type:

```json
{
  "exact_integer": {"type": "i64", "value": "9007199254740993"},
  "real": {"type": "f64", "value": 1.0},
  "negative_zero": {"type": "f64_bits", "value": "8000000000000000"},
  "label": {"type": "string", "value": "example"},
  "enabled": {"type": "bool", "value": true}
}
```

Typed f64 rejects integer tokens that cannot be represented exactly; use an explicit floating literal for rounded floating input. `f64_bits` requires exactly 16 hex digits and preserves any binary64 bit pattern, subject to operation parameter constraints. JSON NaN/Infinity tokens are forbidden. Unknown fields, duplicate keys and invalid Unicode are errors rather than ignored metadata.

## Resources

```json
{"format":"photospider.resources","version":1,"resources":[
  {"path":"scale.bin","byte_length":1,
   "sha256":"084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"}
]}
```

Here `scale.bin` contains the byte `03`. Relative paths resolve against the manifest directory. Hashes use 64 lowercase hexadecimal characters. The driver verifies actual size and SHA-256, then makes immutable resource owners available for static preparation. Artifacts retain selected identities, never local paths or bytes. Resource bytes must be supplied again when loading the artifact.

## Requests

```json
{"format":"photospider.requests","version":1,"outputs":[
  {"name":"result","members":[{"slot":0,"intervals":[[1,2]]}]}
]}
```

Intervals are half-open member-local tile indices, not sample coordinates. `slot` is the zero-based Tensor ordinal. Output names and slots must be unique within their respective objects. Overlapping/adjacent intervals normalize to a compact set; reversed/out-of-grid intervals fail. An empty member set or interval list selects no payload. At most 65536 raw intervals are accepted across a manifest. With shape `[4]`, no atomic axes and tile extent 2, `[1,2]` selects sample offsets 2 and 3. `psc` computes the selected dependency closure; `psx` rejects a request outside saved coverage.

## Runtime bindings

```json
{"format":"photospider.bindings","version":1,"inputs":[
  {"name":"x","tensors":[
    {"slot":0,"path":"input.bin","encoding":"raw-le","dtype":"f64","shape":[4]}
  ]}
]}
```

For example, Python `struct.pack('<4d', 1, 2, 3, 4)` creates the 32-byte input. Paths are manifest-relative; `-` is not a raw-file path. There must be exactly one entry for every Plan input and every declared Tensor slot. Byte length is exactly the product of the full sample shape and dtype width, with overflow checks. Data is dense logical row-major order. The tool reads and validates the complete file, then uses the execution context's Root to create bindings according to the saved Plan contract. `DirectTiles` allocates each required dense Host tile separately, with zero byte offset and matching compiled capacity; unrequested tiles are not retained. `DenseHost` retains one complete dense member allocation when any of its tiles is requested; the Plan's explicit COPY tasks materialize independent selected tiles during execution. Incompatible physical layouts fail before Run submission, including in check mode. The C++ `PlanningOptions::input_layouts` API selects this contract; `psc` uses the default DirectTiles contract.

## Results

A successful `psx` run creates a new directory with `manifest.json` and generated fragment filenames. Example for the partial request above and scale resource `03`:

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

The binary fragment holds 9.0 and 12.0. `shape` on the Tensor is its static full shape; `offset` and `shape` on each fragment are actual covered logical coordinates. Fragment ordering and splitting are implementation details. Fragments are disjoint, collectively cover only published samples, and each stores contiguous row-major little-endian bytes for its region. Empty coverage produces no binary file. Consumers must use the manifest rather than infer coverage from filenames or full schema shape. Results are output files, not resumable execution state.

## IR and Plan text v5

The version-5 `.psir` prefix is:

```text
psir 5
stage ir
parent "..."
producer "optimize" "1"
parameters { }
```

`parent` is provenance and may be empty for initial frontend/validation output. The producer's resolved options use the typed parameter grammar. Subsequent sections store operation-contract/resource manifests, input declarations/bindings, call declarations, return signature, entry, `form dependency_paths|control_flow` and blocks. Both forms are valid for raw and optimizer-produced IR. Each import yields `ValidatedIR` and recomputes content identity; provenance does not contribute to it.

`.psxp` begins with `psxp 5` and `parent` equal to the actual input IR digest, then execution mode, tile extent, lowered CFG declarations and selected output requests. Its `physical` section stores entry, input IDs, return-schema IDs and these ordered tables:

| Table | Encoded fields |
| --- | --- |
| `schemas` | Structural Result schemas |
| `layouts` | Element type, rank, byte offset and eight shape/origin/signed-stride entries |
| `tiles` | Value, member, layout, producer, domain, tile index, capacity and alignment |
| `values` | Source IR value, schema, owner, kind, import-source ID, input-layout contract and ordered tile IDs |
| `placements` | Backend, domain, profile and queue |
| `kernels` | Operation, symbol, contract and ABI version |
| `tasks` | Kind, call/kernel/placement IDs, reads, writes, schema reads, parameter bytes, explicit dependencies, predecessors, successors and scratch |
| `calls` | Source call, block, retirement ordinal, effect task, task IDs, exports and input/output binding IDs |
| `blocks` | Source block, terminator, condition, task/call/argument/live-in/return IDs, edges and reservation vectors |

Value kinds are Input (0), Argument (1), Produced (2) and Imported (3). Input-layout contracts are DirectTiles (0) and DenseHost (1); non-input values use 0. An Imported value has zero source IR value, a block owner and a canonical Input ID in `import_source`; other kinds use the absent ID there. Its tiles have explicit COPY producers. Boundary import COPYs use an absent call ID and execute after all real calls retire. The loader checks provenance and rejects raw DenseHost payload use that bypasses an import.

Table IDs are consecutive from zero; `4294967295` represents an absent ID. Task order does not constrain dependency order. The loader preserves IDs and independently validates adjacency, liveness, reservations, layout bounds and correspondence to IR. Saved derived tables must match their definitions; loading does not repair them. Operation preparation is reconstructed from validated IR and the supplied registry/resources, without calling a lowerer, optimizer or planner. The saved IR has ControlFlow form and may differ from the parent's dependency-path form.

Artifact text is bounded to 64 MiB. Physical tables share a 4194304-record bound and physical references share a 16777216-entry bound. Opaque task parameter bytes consume the separate validation work budget of 268435456 units, alongside structural work; they are not dependency edges. Import and export use the same physical validation contract.

Text supports semicolon line comments and escaped strings. Parameters use `i64`, `f64`, `f64_bits`, `bool`, `string`; encoding preserves IEEE-754 bits. See `src/lib/compiler/artifact_text.cpp` for the exact bounded grammar and `tests/unit/kernel/test_physical_compiler.cpp` for physical round trips. v1–v4 fail with a re-export diagnostic. Artifact identities detect content changes but do not authenticate code or files. Package, document schema, C ABI and artifact versions are independent axes.
