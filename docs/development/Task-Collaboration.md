# Task Collaboration

This public contract applies to kernel and daemon tasks. Product boundaries
remain governed by each repository's accepted ADRs. It does not change CI or
branch protection and does not require private assistant files for review.

## Sources and task scope

Use the named live Issue as the task source. Public ADRs record accepted
long-term decisions; architecture documents record implemented facts. An ADR
proposal or accepted target must be clearly distinguished from available API
behavior. Issues own delivery status; Projects are operational views. Update
the Current Development Program only for milestone, baseline, critical-path,
or blocker changes. Private OpenSpec does not define public acceptance.

At start identify task type (research, decision, implementation, delivery),
validation mode, acceptance criteria, dependencies, and authorized endpoint
(local draft, local implementation and validation, PR submission, or merged
settlement). Verify the branch, HEAD, existing changes, and relevant PRs.
These endpoints are collaboration agreements. Use existing authorization;
ask only for a missing material decision or permission. File editing, testing,
and review do not imply commit/push, metadata writes, merge, or cleanup.

## Execution and validation

Default to `rapid`: locate, make the focused change, validate its behavior,
and review the diff. Use `reviewed` for public API/ABI, package/consumer,
ownership, persistent-format, unresolved external-behavior or substantive
alternative decisions, concurrency, memory, or untrusted-input risk. Add an
independent relevant code/spec review and only risk-justified wider checks.
Use `release` only when explicitly requested.

Keep one code writer for a task and coordinate tasks sharing a worktree.
Independent read-only research/review can run concurrently. Review findings
are `blocker`, `required`, `suggestion`, or `invalid or unverified`; only
verified, in-scope blockers and required findings enter the repair loop.
Out-of-scope findings are reported for triage. Ordinary work does not create
OpenSpec, feedback, tracking, evidence, branches, worktrees, or full test
matrices automatically. Workflow-text checks do not become product CI gates.

## Status and completion

Maintain one concise Issue current-status block, where remote updates are
authorized, covering result/location, checked acceptance and validation,
remaining work, blockers, concrete maintainer decisions, and one next action
with owner, prerequisites, and stopping point. Comments retain discussion.
State the actual lifecycle: needs clarification, ready, in progress, awaiting
review, awaiting acceptance, or complete. Record blockers separately and
identify when nobody is executing. `ready-for-agent` describes readiness only.

Research delivers evidence, conclusions, unknowns, and next steps. A decision
requires explicit maintainer acceptance and its specified delivery. An
implementation reaches the agreed delivery location with applicable tests.
A parent capability requires a repeatable integrated acceptance scenario.
Cancelled work and issue counts do not establish delivered capability or a
project completion percentage. Stop at the authorized endpoint; a suggested
next Issue is not authorization to start it.

One task owner updates Issue status, then the Project mapping. Verify actual
remote changes. If updates are unauthorized or fail, report that GitHub was
not updated and provide the exact pending text when needed. Private recovery
notes are optional and require task authorization; never publish private
paths, credentials, configuration, or local reports. Fresh sessions recheck
references and Git state. Uncommitted work requires its original worktree
unless an authorized accessible commit or patch preserves it.
