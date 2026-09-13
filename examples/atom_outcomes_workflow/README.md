# Atom outcomes and numerical quality

This installed C++ example runs `RegionalSource → guarded reciprocal → scale →
AtomObservation sink` with source values `[2, 0, -1]` and fallback `[9, 8, 4]`.
It checks the independent scalar reference `2 / (primary < 0 ? fallback : primary)`:
outputs 1 and 0.5, with a separately observable DivideByZero at coordinate 1.
The failure identifies node 11 while the requested sink identifies node 22.

```sh
cmake --build build/issue257-shared --target photospider_atom_outcomes_workflow -j 6
./build/issue257-shared/examples/atom_outcomes_workflow/photospider_atom_outcomes_workflow
```

An isolated installed consumer uses only the public package:

```sh
cmake --install build/issue257-shared --prefix "$PWD/out/phase-a-install"
cmake -S examples/atom_outcomes_workflow -B out/atom-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/out/phase-a-install"
cmake --build out/atom-consumer -j 6
./out/atom-consumer/photospider_atom_outcomes_workflow
```

A second run makes the middle source read fail and checks Io/Group/input=1
without inventing an atom-domain error. Both runs retain successful values after
context destruction and verify last-owner resource release. Zero observation
capacity accepts empty demand; a capacity of two rejects a three-atom request.

The separate registered quality callback checks a fixed integer diagonal system
`a=[1,3,2], x=[16777217,1,2], b=[16777217,4,4]`. Independent integer arithmetic
gives residual R=1 and min|a|=1, hence solution error at most 1. Its reported bound
is conservatively rounded above 1. Measured evidence has no bound, an estimate
rounded through Float32 is rejected, and NotConverged retains its Measured
report through a downstream node. This evidence describes the stored integer
system, not arbitrary Float64 iteration or resource/RSS usage.

See [the runtime contract](../../docs/kernel-architecture/Atom-Errors-and-Quality.md).
