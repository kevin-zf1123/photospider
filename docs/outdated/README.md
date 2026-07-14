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

## Current Sources of Truth

- `readme.md` and `manual.md` for product use;
- `docs/kernel-architecture/README.md` for current kernel behavior;
- `docs/adr/` for accepted long-lived decisions;
- `docs/roadmap/Kernel-Evolution.md` for accepted future architecture;
- `docs/development/Testing-and-Validation.md` for maintained development
  validation guidance.

Before using an archived statement, verify it against current code and the
maintained documents above.
