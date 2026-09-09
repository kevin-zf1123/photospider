# Compute Flow

1. The caller creates a `WorkflowDocument` and an independent `GraphContext`.
2. A coherent `GraphSnapshot` captures source plus revision.
3. `Compiler::analyze` validates and produces typed `SemanticGraphIR`.
4. `Compiler::optimize` produces a distinct conservative
   `OptimizedGraphIR`.
5. `Compiler::plan` produces dependency-ordered local `ExecutionPlan` steps.
6. `ExecutionContext::execute` verifies the exact frozen operation-registry
   identity and creates one private `ExecutionRun`.
7. Dependency-ready steps enter a bounded CPU queue or optional local GPU
   callback queue under per-Run parallelism and one context-wide waiting
   admission.
8. Explicit native upload actions pack demanded input Regions into shared
   buffers when no valid retained copy exists. GPU successors reuse buffers;
   host access follows completion without a fabricated D2H copy.
9. Before a scheduler/admission failure becomes the first failure, and again
   when an operation callback completes, the Run selects cancellation before
   graph staleness before the original failure. It then checks type/shape and
   dependency identity before any Value publication.
10. Complete named outputs become one in-memory `ExecutionResult`; temporary Values retire after their last reader, while returned Values
    retain allocation leases until their final owner releases them.

Cancellation is cooperative, not preemption. Running callbacks may return
late, but their Values cannot publish after cancellation or graph replacement.
Exceptions are fenced into typed failure and do not stop unrelated contexts.
The first-failure selector is allocation-free and is also used when failure
diagnostic construction itself fails, so an already observed stop cannot be
downgraded to queue, admission, or backend failure.

GPU selection names the optional native Metal implementation under explicit
MetalFp32 policy. The existing single GPU lane drains submitted commands before
callback retirement, including cancellation. Traits permit output-free CPU
fallback; submitted device execution errors terminate the Run. Both attempts,
actual dispatches/copies and cache reuse remain visible in raw diagnostics.
