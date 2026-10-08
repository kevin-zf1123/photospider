# Result atom outcomes and numerical quality

This example builds Float64 `{3}` Result tensors for `primary` and `fallback`, then runs `example.guarded_reciprocal` followed by `example.scale`. At each requested coordinate, the reciprocal callback requests only the needed input sample. It selects `primary` when the value is nonnegative, otherwise it requests `fallback`; zero returns a local `DivideByZero` Atom failure. The scale node doubles successful reciprocals.

The input tensors are `[2, 0, -1]` and `[9, 8, 4]`. The independent scalar oracle is `primary >= 0 ? 2 / primary : 2 / fallback`, with zero reported as failure. Coordinates 0 and 2 therefore produce `1` and `0.5`; coordinate 1 retains the divide-by-zero outcome from node 11. Three requested observations use two joint groups and no singleton fallback. The Result values are read after the execution context is destroyed, then releasing the observations returns the managed Root's live resource counts to zero.

Build and run the in-tree example from the repository root:

```sh
cmake --build build/kernel-dev --target photospider_atom_outcomes_workflow -j 8
./build/kernel-dev/examples/atom_outcomes_workflow/photospider_atom_outcomes_workflow
```

The same source can be configured as a standalone consumer of an installed SDK:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/atom-outcomes-install
cmake -S examples/atom_outcomes_workflow -B build/kernel-dev/atom-outcomes-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/atom-outcomes-install" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/atom-outcomes-consumer -j 8
./build/kernel-dev/atom-outcomes-consumer/photospider_atom_outcomes_workflow
```

The transport-error case inserts `example.transport` at node 10. That separate Atomic Result continuation returns a simulated `ShortIo` with `Io/Group` scope at coordinate 1; it is not a disk or device I/O measurement. Because the callback uses request-level failure delivery, the error remains a source/transport failure rather than being recast as a contract-2 Atom failure. The ordinary divide-by-zero case remains the local Atom failure from node 11.

The quality example attaches either a certified integer-diagonal report or a Measured residual report to contract-2 outcomes. For `a=[1,3,2]`, `x=[16777217,1,2]`, and `b=[16777217,4,4]`, the independent integer calculation gives `max |a*x-b|=1` and `min |a|=1`, so the solution error is at most 1; the reported bound rounds upward. Measured evidence carries no error bound. A `NotConverged` Atom or ValidationDomain failure preserves its Measured report through the downstream scale node.

The example also checks that `maximum_atom_observations=0` accepts an Empty query, while a limit of 2 rejects a three-observation request. See [Atom errors and quality](../../docs/kernel-architecture/Atom-Errors-and-Quality.md) for the contract.
