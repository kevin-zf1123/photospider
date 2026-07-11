#!/usr/bin/env python3
"""Audit built-in scheduler Doxygen through Clang's real comment AST."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterator


REQUIRED_COMMANDS = {"brief", "return", "throws", "note"}
CALLABLE_KINDS = {
    "CXXConstructorDecl",
    "CXXConversionDecl",
    "CXXDestructorDecl",
    "CXXMethodDecl",
}

SCHEDULERS = {
    "CpuWorkStealingScheduler": {
        "header": "src/lib/scheduler/cpu_work_stealing_scheduler.hpp",
        "source": "src/lib/scheduler/cpu_work_stealing_scheduler.cpp",
        "methods": {
            "CpuWorkStealingScheduler",
            "~CpuWorkStealingScheduler",
            "attach",
            "detach",
            "start",
            "shutdown",
            "name",
            "get_stats",
            "is_running",
            "task_runtime_running",
            "submit_initial_tasks",
            "submit_initial_task_handles",
            "submit_ready_task_from_worker",
            "submit_ready_task_handle_from_worker",
            "submit_ready_task_handles_from_worker",
            "submit_ready_task_any_thread",
            "submit_ready_task_handle_any_thread",
            "submit_ready_task_handles_any_thread",
            "wait_for_completion",
            "dec_tasks_to_complete",
            "inc_tasks_to_complete",
            "set_exception",
            "log_event",
            "active_epoch",
            "begin_new_epoch",
            "should_cancel_epoch",
            "this_task_epoch",
            "this_worker_id",
            "run_loop",
            "steal_task",
            "reset_exception_state",
            "finish_in_flight_task",
        },
        "nested": {
            "ScheduledTask": {
                "fields": {"epoch", "task", "handle", "use_handle"},
                "methods": {"ScheduledTask", "operator bool", "run"},
            }
        },
        "important_fields": {
            "runtime_",
            "workers_",
            "num_workers_",
            "configured_workers_",
            "running_",
            "worker_loop_active_",
            "local_task_queues_",
            "local_queue_mutexes_",
            "high_priority_queue_",
            "normal_priority_queue_",
            "global_queues_mutex_",
            "ready_task_count_",
            "tasks_to_complete_",
            "in_flight_tasks_",
            "first_exception_",
            "exception_claimed_",
            "has_exception_",
            "exception_epoch_",
            "exception_cleanup_complete_",
            "epoch_counter_",
            "active_epoch_",
        },
    },
    "GpuPipelineScheduler": {
        "header": "src/lib/scheduler/gpu_pipeline_scheduler.hpp",
        "source": "src/lib/scheduler/gpu_pipeline_scheduler.cpp",
        "methods": {
            "GpuPipelineScheduler",
            "~GpuPipelineScheduler",
            "attach",
            "detach",
            "start",
            "shutdown",
            "name",
            "get_stats",
            "is_running",
            "task_runtime_running",
            "submit_rt_task",
            "submit_rt_task_handle",
            "submit_hp_task",
            "submit_hp_task_handle",
            "submit_gpu_task",
            "submit_gpu_task_handle",
            "submit_initial_tasks",
            "submit_initial_task_handles",
            "submit_ready_task_from_worker",
            "submit_ready_task_handle_from_worker",
            "submit_ready_task_handles_from_worker",
            "submit_ready_task_any_thread",
            "submit_ready_task_handle_any_thread",
            "submit_ready_task_handles_any_thread",
            "wait_for_completion",
            "dec_tasks_to_complete",
            "inc_tasks_to_complete",
            "set_exception",
            "log_event",
            "active_epoch",
            "begin_new_epoch",
            "should_cancel_epoch",
            "this_task_epoch",
            "this_worker_id",
            "is_gpu_available",
            "get_available_devices",
            "available_devices",
            "cpu_run_loop",
            "gpu_run_loop",
            "start_gpu_workers_if_available",
            "can_dispatch_hp_to_gpu",
            "reset_exception_state",
            "finish_in_flight_task",
        },
        "nested": {
            "Config": {
                "fields": {"gpu_workers", "cpu_workers", "prefer_gpu_for_hp"},
                "methods": {"Config"},
            },
            "ScheduledTask": {
                "fields": {"epoch", "task", "handle", "use_handle", "intent"},
                "methods": {"ScheduledTask", "operator bool", "run"},
            },
        },
        "important_fields": {
            "runtime_",
            "config_",
            "cpu_workers_",
            "num_cpu_workers_",
            "gpu_workers_",
            "num_gpu_workers_",
            "running_",
            "worker_loop_active_",
            "rt_queue_",
            "hp_cpu_queue_",
            "gpu_queue_",
            "rt_ready_count_",
            "hp_cpu_ready_count_",
            "gpu_ready_count_",
            "tasks_to_complete_",
            "in_flight_tasks_",
            "epoch_counter_",
            "active_epoch_",
            "first_exception_",
            "exception_claimed_",
            "has_exception_",
            "exception_epoch_",
            "exception_cleanup_complete_",
        },
    },
}

HOOK_TYPES = {
    "SchedulerFailurePoint",
    "SchedulerFailureInjectionHook",
    "SchedulerTransactionalStateSnapshot",
    "SchedulerExceptionPublicationHook",
    "SchedulerExceptionPublicationSnapshot",
    "SchedulerStartPublicationHook",
    "SchedulerCpuWaitHook",
    "SchedulerCpuLocalReadyHook",
}

HOOK_FUNCTIONS = {
    "set_cpu_scheduler_exception_publication_hook",
    "set_cpu_scheduler_start_publication_hook",
    "set_cpu_scheduler_local_ready_hook",
    "set_cpu_scheduler_failure_injection_hook",
    "cpu_scheduler_transactional_snapshot",
    "cpu_scheduler_exception_publication_snapshot",
    "set_gpu_scheduler_exception_publication_hook",
    "set_gpu_scheduler_start_publication_hook",
    "set_gpu_scheduler_cpu_wait_hook",
    "set_gpu_scheduler_failure_injection_hook",
    "set_gpu_scheduler_force_gpu_route",
    "gpu_scheduler_transactional_snapshot",
    "gpu_scheduler_exception_publication_snapshot",
}


def walk(value: Any) -> Iterator[dict[str, Any]]:
    """Yield every decoded mapping node in depth-first order."""

    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def full_comment(node: dict[str, Any]) -> dict[str, Any] | None:
    """Return the declaration's direct compiler-associated FullComment."""

    return next(
        (
            child
            for child in node.get("inner", [])
            if isinstance(child, dict) and child.get("kind") == "FullComment"
        ),
        None,
    )


def comment_contract(node: dict[str, Any]) -> dict[str, Any]:
    """Derive structured commands and parameter coverage from Clang AST."""

    comment = full_comment(node)
    nodes = list(walk(comment)) if comment else []
    commands = sorted(
        {
            child.get("name", "")
            for child in nodes
            if child.get("kind") == "BlockCommandComment"
        }
    )
    parameters = sorted(
        child.get("name", "")
        for child in node.get("inner", [])
        if isinstance(child, dict)
        and child.get("kind") == "ParmVarDecl"
        and child.get("name")
    )
    documented = sorted(
        {
            child.get("param", "")
            for child in nodes
            if child.get("kind") == "ParamCommandComment"
        }
    )
    passed = (
        comment is not None
        and REQUIRED_COMMANDS.issubset(commands)
        and parameters == documented
    )
    return {
        "commands": commands,
        "parameters": parameters,
        "documented_parameters": documented,
        "passes": passed,
    }


def parse_json_stream(text: str) -> list[Any]:
    """Decode adjacent JSON roots emitted by filtered Clang AST dumps."""

    decoder = json.JSONDecoder()
    offset = 0
    values: list[Any] = []
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset >= len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        values.append(value)
    return values


def compile_arguments(database: Path, source: Path) -> list[str]:
    """Return real BUILD_TESTING compiler arguments for one scheduler TU."""

    rows = json.loads(database.read_text(encoding="utf-8"))
    candidates = [
        row for row in rows if Path(row["file"]).resolve() == source.resolve()
    ]
    if not candidates:
        raise ValueError(f"compile_commands lacks {source}")
    entry = next(
        (
            row
            for row in candidates
            if "PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING"
            in (row.get("command") or " ".join(row["arguments"]))
        ),
        candidates[0],
    )
    arguments = (
        shlex.split(entry["command"])
        if "command" in entry
        else list(entry["arguments"])
    )
    normalized = [arguments[0]]
    skip = False
    for argument in arguments[1:]:
        if skip:
            skip = False
            continue
        if argument == "-o":
            skip = True
            continue
        if argument == "-c":
            continue
        try:
            is_source = Path(argument).resolve() == source.resolve()
        except OSError:
            is_source = False
        if not is_source:
            normalized.append(argument)
    return normalized


def run_ast(
    arguments: list[str], repo: Path, source: Path, name_filter: str
) -> tuple[list[Any], dict[str, Any]]:
    """Run one real configured Clang comment-AST query."""

    command = arguments + [
        "-Xclang",
        "-fparse-all-comments",
        "-Xclang",
        "-ast-dump=json",
        "-Xclang",
        "-ast-dump-filter",
        "-Xclang",
        name_filter,
        "-fsyntax-only",
        str(source),
    ]
    completed = subprocess.run(
        command, cwd=repo, check=True, capture_output=True, text=True
    )
    return parse_json_stream(completed.stdout), {
        "filter": name_filter,
        "returncode": completed.returncode,
        "stderr_empty": not completed.stderr.strip(),
    }


def record_by_file(roots: list[Any], name: str, header: Path) -> dict[str, Any] | None:
    """Locate one explicit record root declared in the requested header."""

    return next(
        (
            node
            for node in walk(roots)
            if node.get("kind") in {"CXXRecordDecl", "EnumDecl"}
            and node.get("name") == name
            and Path(node.get("loc", {}).get("file", "")).resolve() == header.resolve()
        ),
        None,
    )


def inspect_scheduler(
    repo: Path, database: Path, class_name: str, specification: dict[str, Any]
) -> dict[str, Any]:
    """Audit one scheduler class declaration, nested records, and definitions."""

    source = repo / specification["source"]
    header = repo / specification["header"]
    arguments = compile_arguments(database, source)
    roots, query = run_ast(arguments, repo, source, class_name)
    record = record_by_file(roots, class_name, header)
    if record is None:
        return {"passes": False, "error": "class record missing", "query": query}

    direct = [child for child in record.get("inner", []) if isinstance(child, dict)]
    method_rows: dict[str, Any] = {}
    for name in sorted(specification["methods"]):
        candidates = [
            child
            for child in direct
            if child.get("kind") in CALLABLE_KINDS
            and child.get("name") == name
            and not child.get("isImplicit")
            and not child.get("explicitlyDeleted")
        ]
        observations = [comment_contract(candidate) for candidate in candidates]
        method_rows[name] = {
            "candidate_count": len(candidates),
            "observations": observations,
            "passes": bool(observations) and all(row["passes"] for row in observations),
        }

    field_rows: dict[str, Any] = {}
    for name in sorted(specification["important_fields"]):
        candidate = next(
            (
                child
                for child in direct
                if child.get("kind") == "FieldDecl" and child.get("name") == name
            ),
            None,
        )
        field_rows[name] = {
            "observed": candidate is not None,
            "has_full_comment": candidate is not None
            and full_comment(candidate) is not None,
        }

    nested_rows: dict[str, Any] = {}
    for nested_name, nested_spec in specification["nested"].items():
        nested = next(
            (
                child
                for child in direct
                if child.get("kind") == "CXXRecordDecl"
                and child.get("name") == nested_name
                and child.get("completeDefinition")
            ),
            None,
        )
        nested_direct = (
            [child for child in nested.get("inner", []) if isinstance(child, dict)]
            if nested
            else []
        )
        nested_fields = {
            name: any(
                child.get("kind") == "FieldDecl"
                and child.get("name") == name
                and full_comment(child) is not None
                for child in nested_direct
            )
            for name in sorted(nested_spec["fields"])
        }
        nested_methods = {}
        for name in sorted(nested_spec["methods"]):
            candidates = [
                child
                for child in nested_direct
                if child.get("kind") in CALLABLE_KINDS
                and child.get("name") == name
                and not child.get("isImplicit")
            ]
            nested_methods[name] = bool(candidates) and all(
                comment_contract(candidate)["passes"] for candidate in candidates
            )
        nested_rows[nested_name] = {
            "record_comment": nested is not None and full_comment(nested) is not None,
            "fields": nested_fields,
            "methods": nested_methods,
        }

    definition_rows: dict[str, Any] = {}
    for name in sorted(specification["methods"]):
        candidates = [
            node
            for node in walk(roots)
            if node.get("kind") in CALLABLE_KINDS
            and node.get("name") == name
            and Path(node.get("loc", {}).get("file", "")).resolve() == source.resolve()
            and any(
                isinstance(child, dict) and child.get("kind") == "CompoundStmt"
                for child in node.get("inner", [])
            )
        ]
        definition_rows[name] = {
            "candidate_count": len(candidates),
            "has_full_comment": bool(candidates)
            and all(full_comment(candidate) is not None for candidate in candidates),
        }

    passes = (
        comment_contract(record)["passes"]
        and all(row["passes"] for row in method_rows.values())
        and all(
            row["observed"] and row["has_full_comment"] for row in field_rows.values()
        )
        and all(
            row["record_comment"]
            and all(row["fields"].values())
            and all(row["methods"].values())
            for row in nested_rows.values()
        )
        and all(
            row["candidate_count"] == 1 and row["has_full_comment"]
            for row in definition_rows.values()
        )
    )
    return {
        "class_comment": comment_contract(record),
        "methods": method_rows,
        "important_fields": field_rows,
        "nested_records": nested_rows,
        "definitions": definition_rows,
        "query": query,
        "passes": passes,
    }


def inspect_hooks(repo: Path, database: Path) -> dict[str, Any]:
    """Audit BUILD_TESTING hook types/functions through real Clang AST roots."""

    source = repo / SCHEDULERS["CpuWorkStealingScheduler"]["source"]
    header = repo / "src/lib/scheduler/scheduler_exception_test_hooks.hpp"
    arguments = compile_arguments(database, source)
    roots: list[Any] = []
    queries: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="photospider-scheduler-doc-") as temp:
        translation_unit = Path(temp) / "hooks.cpp"
        translation_unit.write_text(
            '#include "scheduler/scheduler_exception_test_hooks.hpp"\n',
            encoding="utf-8",
        )
        for name_filter in (
            "SchedulerFailure",
            "SchedulerTransactional",
            "SchedulerExceptionPublication",
            "SchedulerStartPublication",
            "SchedulerCpu",
            "set_cpu_scheduler",
            "set_gpu_scheduler",
            "scheduler_transactional_snapshot",
            "scheduler_exception_publication_snapshot",
        ):
            values, query = run_ast(arguments, repo, translation_unit, name_filter)
            roots.extend(values)
            queries.append(query)

    type_rows = {}
    for name in sorted(HOOK_TYPES):
        candidates = [
            node
            for node in walk(roots)
            if node.get("kind") in {"CXXRecordDecl", "EnumDecl"}
            and node.get("name") == name
            and Path(node.get("loc", {}).get("file", "")).resolve() == header.resolve()
        ]
        candidate = candidates[0] if candidates else None
        children = candidate.get("inner", []) if candidate else []
        members = [
            child
            for child in children
            if isinstance(child, dict)
            and child.get("kind") in {"FieldDecl", "EnumConstantDecl"}
        ]
        type_rows[name] = {
            "candidate_count": len(candidates),
            "has_full_comment": candidate is not None
            and full_comment(candidate) is not None,
            "member_count": len(members),
            "members_documented": bool(members)
            and all(full_comment(member) is not None for member in members),
        }

    function_rows = {}
    for name in sorted(HOOK_FUNCTIONS):
        candidates = [
            node
            for node in walk(roots)
            if node.get("kind") == "FunctionDecl"
            and node.get("name") == name
            and full_comment(node) is not None
        ]
        function_rows[name] = {
            "documented_candidate_count": len(candidates),
            "passes": bool(candidates),
        }

    passes = all(
        row["candidate_count"] >= 1
        and row["has_full_comment"]
        and row["members_documented"]
        for row in type_rows.values()
    ) and all(row["passes"] for row in function_rows.values())
    return {
        "types": type_rows,
        "functions": function_rows,
        "queries": queries,
        "passes": passes,
    }


def main() -> int:
    """Run the audit and persist replayable expected/actual/compare evidence."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    database = args.compile_commands.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    schedulers = {
        name: inspect_scheduler(repo, database, name, specification)
        for name, specification in SCHEDULERS.items()
    }
    hooks = inspect_hooks(repo, database)
    actual = {
        "compile_commands_exists": database.is_file(),
        "schedulers": schedulers,
        "test_hooks": hooks,
    }
    expected = {
        "compiler_comment_ast": True,
        "scheduler_classes": sorted(SCHEDULERS),
        "scheduler_method_counts": {
            name: len(specification["methods"])
            for name, specification in SCHEDULERS.items()
        },
        "hook_type_count": len(HOOK_TYPES),
        "hook_function_count": len(HOOK_FUNCTIONS),
    }
    checks = {
        "configured compilation database exists": actual["compile_commands_exists"],
        "CPU scheduler declaration/definition Doxygen passes Clang AST": schedulers[
            "CpuWorkStealingScheduler"
        ]["passes"],
        "GPU scheduler declaration/definition Doxygen passes Clang AST": schedulers[
            "GpuPipelineScheduler"
        ]["passes"],
        "BUILD_TESTING hook types/functions are compiler-documented": hooks["passes"],
    }
    passed = all(checks.values())
    compare = "\n".join(
        ["scheduler_doxygen_ast"]
        + [f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()]
        + [f"overall={'PASS' if passed else 'FAIL'}", ""]
    )
    summary = "\n".join(
        [
            "# Scheduler Doxygen Clang AST audit",
            "",
            f"Overall: {'PASS' if passed else 'FAIL'}",
            "",
            "The audit uses the configured compiler command, parses FullComment "
            "nodes for real scheduler declarations/definitions and BUILD_TESTING "
            "hooks, and checks structured commands plus parameter coverage.",
            "",
        ]
    )
    (out / "actual.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (out / "expected.json").write_text(
        json.dumps(expected, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (out / "compare.log").write_text(compare, encoding="utf-8")
    (out / "summary.md").write_text(summary, encoding="utf-8")
    print(compare, end="")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
