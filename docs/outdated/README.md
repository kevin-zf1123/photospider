# Outdated Documentation

This directory keeps historical documentation that is useful for context but no
longer describes the current branch as the source of truth.

## Removal Notes

- 2026-06-11: Historical references to legacy `Node::cached_output` in this
  directory describe removed content. The current branch uses
  `cached_output_high_precision` for reusable HP cache and
  `cached_output_real_time` for transient RT state, with no legacy cache
  fallback.

The active docs are:

- `readme.md`
- `manual.md`
- `docs/kernel-architecture/Overview.md`
- `docs/kernel-architecture/Dirty-Region-Propagation.md`

Use files in this directory as design history only. Before acting on one of
these documents, verify behavior against the code and current maintained docs.
