# Result Representation Workflows

This example turns a compact byte wire into typed Result records, checks the records through a separate Result consumer, and exercises representation helpers for spectra, wavelet bands, paths, points, connected components, YCbCr 4:2:0, causal brush state, and iterative systems. The workflow uses Result inputs and outputs throughout.

## Build and run

The example requires Photospider 0.30 or newer. From the repository root, build and run the registered target:

```sh
cmake --build build/kernel-dev --target photospider_representations_workflow -j8
./build/kernel-dev/photospider_representations_workflow
./build/kernel-dev/photospider_representations_workflow spectrum-nyquist-column
```

The optional arguments are `[fixture [root_bytes]]`. With no fixture selected, the program runs the complete fixture set. A fixture name selects all cases registered under that name, including its backing-layout variants and intentional rejection cases. The optional byte count must be an unsigned decimal integer and sets both the execution Root's Host and Metadata capacities; when omitted, each capacity defaults to 512 KiB. Unknown fixture names and malformed capacities are rejected. For example, `spectrum-nyquist-column 524288` runs the dense, negative-stride, split-wire, and invalid-Hermitian cases with 512 KiB for each capacity.

## Result flow and ownership

The input is one owning `ResultRef` with schema `example.representation_wire` and one UInt8 tensor, `samples`, whose rank-one extent contains a 128-byte field-count header followed by the packed field rows. The `example.pack` operation requests that tensor as Data, Control, Validation, and Descriptor, acquires an owning read window, and walks the wire through bounded row runs. For each run it honors the reported sample stride, charges Run work, and propagates typed read or resource failures. The valid `spectrum-nyquist-column` fixture is also supplied through a negative-stride backing and through two backing regions split at byte 64, so the header itself crosses a physical boundary. `points-empty` uses a 128-byte logical wire backed by one byte with zero stride. These cases exercise the same parser without assuming a single contiguous span.

The packer validates field row counts and payload length, then publishes a Result using the fixture's declared schema. Fields are appended in pages bounded by the execution's `maximum_result_window_bytes`; the fixtures use 128- or 256-byte windows. The source relation and output field relations record conservative support.

A second operation reads the packed Result descriptor and publishes a Float64 `{1}` tensor containing the total field-row count. The harness independently reads each published field and compares its bytes with the fixture data. It verifies that the wire is read once, checks the summary row count, and checks the ordered source ObjectIds in the Result associations.

Association is identity history, not a payload lease. In the iteration fixture, a child Result names its predecessor, but the child owns its published storage: after the predecessor Result is released, the child remains readable. Releasing the last Result releases its backing storage; the harness checks that Root Disk usage returns to zero.

## Coverage

The fixtures cover exact and measured spectrum policies, packed-spectrum schema rejection, odd Haar membership and explicit zero bands, path and primitive records, empty and reordered point sets, component limits, odd-edge chroma support, causal brush partitioning, and diagonal iteration validation. Invalid fixtures are expected to fail before the publication observer runs. The iteration fixture also checks generation, convergence state, and the independently known diagonal solution.

The executable reports actual Root Host and Metadata peaks and wire bytes read for each successful fixture. These are observations from the selected run and capacity configuration, not fixed cross-platform values. This example does not establish native GPU behavior or a platform-wide performance result.
