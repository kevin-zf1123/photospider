# Editable cached workflows

`InputSnapshotStore` provides immutable input snapshots and patches.
`ExecutionContext::freeze` pins a compiled plan to bindings for execution and
cache reuse. The current public behavior is exercised by `test_input_snapshot`,
`test_frozen_execution`, and `test_planar_image_workflow`.

The source under `examples/s3_image_workflow` remains a migration fixture. It
references the retired built-in `image.gaussian_blur`, so its image chain is not
an executable default-registry example. A filter workflow requires an accepted
05-filter contract and a new registered implementation. Current proposed
filter behavior is in [05-filter](../built-in_ops/05-filter/spatial.md).
