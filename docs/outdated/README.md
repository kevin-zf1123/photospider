# Outdated Documentation

This directory preserves development history that no longer defines current
software behavior. Files here may contain obsolete names, APIs, intermediate
designs, or incomplete experiments. They are evidence of past decisions only.

## Archived Kernel Material

`kernel-architecture/` contains historical reports and migration artifacts.
The following files were moved out of the maintained kernel architecture set
on 2026-07-14:

- `kernel-architecture/Compute-Service-Split.md`: completed compute-service
  restructuring plan;
- `kernel-architecture/Benchmark-Spikes.md`: proposed experiments without a
  stable architecture result.

Their Chinese reader copies are preserved under
`kernel-architecture/zh/`. Historical documents without a Chinese source copy
remain historical-only and are not retroactively treated as maintained docs.

## Archive-only boundary

Files below this directory are historical context only. Active indexes do not
link them as product authority, and they must not be used to restore a removed
domain. The current sources of truth are:

- `readme.md` for the product surface;
- `docs/kernel-architecture/README.md` for current kernel behavior;
- `docs/adr/0015-breaking-product-boundary-scope-reset.md` for the highest
  product-boundary decision; and
- `docs/development/Testing-and-Validation.md` for maintained validation.

Before quoting an archived statement, verify it against current code and those
maintained documents. Job/service, worker-process, policy, trust, isolation,
durable-result, and evidence material here remains archive-only.
