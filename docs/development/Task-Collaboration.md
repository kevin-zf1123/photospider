# Task Collaboration

These conventions govern work in this repository. Accepted public ADRs retain
product authority; this document does not alter CI or branch protection.

## Scope and completion

Start from the current user request or named live Issue. Read its acceptance,
direct dependencies, relevant code, tests, and affected contracts. Ordinary
implementation does not require an Issue, proposal, or a repository-wide read.
Check the current branch, HEAD, and existing edits before modifying files.

Complete implementation, run it, inspect results, and fix in-scope failures
until the agreed endpoint is reached. Default to local implementation and
validation. Reuse existing authorization; ask only for a missing material
decision or external action. Do not stop after the first implementation when
verification or repairs remain. An authorized Issue list or Project range can
be completed in dependency order without asking again after every item.

## Early kernel, operation, and workflow iteration

Develop kernel capabilities alongside useful operations and runnable workflows.
An operation using the existing ABI normally follows `rapid`. Describe its
name, input/output types and shapes, parameter types/ranges/defaults, numeric
or image semantics, and applicable Region demand near its existing docs or
example. Avoid creating a second operation catalog with duplicated schemas.

For a new composable operation or workflow execution capability, provide a
minimal workflow run through a public entrypoint and a checkable expected
result. Reuse existing examples or integration tests;
a bug fix may reuse the workflow and add a focused regression. Examples intended
for GPT must use implemented APIs. Accepted target contracts are explicitly
identified as such. Add deterministic behavioral coverage and error/boundary
cases according to the operation's actual risks. Do not require a whole-kernel
validation matrix for each new operation.

## Validation and review

Default to `rapid`: focused implementation, behavioral tests, scoped format/lint,
and diff review. Use `reviewed` for shared public API/ABI, installed-consumer,
ownership or persistent-format changes, or actual concurrency, memory, or
untrusted-input risk. Verify unresolved external behavior and substantive
alternatives before choosing. Add an independent relevant code/contract check
and widen tests only for the affected risk. `release` is explicitly requested.

Keep one code writer per task. Delegate only independent, bounded research,
review, or CI analysis when useful; no fixed agent count, nesting, fresh-owner
requirement, or mandatory review cycle. Reuse reviewers for follow-up checks.
Fix verified, in-scope `blocker` and `required` findings; report suggestions and
out-of-scope findings without automatically expanding the task.

Reuse valid checks until later changes invalidate them. CI failures start with
logs; retrieve relevant artifacts only when needed for diagnosis. Workflow,
score, migration, and tracking checks do not become product CI gates.

## Delivery and records

Commit/push, GitHub writes, merge, cleanup, and private overlay synchronization
follow the current task's authorization. Organize commits and PRs by coherent
behavior; related Issues may share a PR. Use the repository's actual required
checks for the current PR revision. Additional remote reviews and post-merge
validation are conditional on risk or explicit delivery requirements. Do not
assume admin merge, bypass protection, or create a separate CI branch for every
CI correction; split unrelated infrastructure work when it merits its own PR.

Issues own public delivery status and Projects reflect it. When writes are
authorized, update relevant status at delivery or a meaningful blocker change.
Keep the result, location, actual validation, remaining work, and next action
concise. Update the Current Development Program only for a milestone or key
dependency change. Do not duplicate progress in feedback, tracking, and reports.
Private OpenSpec is explicit-only and supplies no public delivery gate.

Public ADRs record decisions; architecture docs record implemented behavior.
Update affected English documents and their Chinese mirrors. Research does not
accept a design; an accepted design does not prove implementation. Completion
means the requested behavior is verified at the agreed delivery location.
Private recovery notes are optional when requested; uncommitted work requires
its original worktree unless an accessible commit or patch preserves it.
