#!/usr/bin/env python3
"""Verify real graph_cli option mode keeps Host bad_alloc exceptional."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


RESOURCE_EXHAUSTION_EXIT_CODE = 3
RESOURCE_EXHAUSTION_DIAGNOSTIC = "Fatal: graph_cli resource exhaustion."


def run_command(
    command: list[str], cwd: Path, timeout_seconds: float
) -> subprocess.CompletedProcess[str]:
    """@brief Runs one graph_cli process with captured text streams.

    @param command Exact executable and argument vector without shell parsing.
    @param cwd Isolated working directory used for config/session side effects.
    @param timeout_seconds Maximum wall time before the child is terminated.
    @return Completed process containing the real exit status and both streams.
    @throws OSError If the executable cannot be started.
    @throws subprocess.TimeoutExpired If option mode does not terminate in time.
    @note HOME is redirected to ``cwd``; all other environment entries are
      inherited unchanged.
    """

    environment = os.environ.copy()
    environment["HOME"] = str(cwd)
    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout_seconds,
        env=environment,
    )


def write_json(path: Path, value: Any) -> None:
    """@brief Writes one deterministic UTF-8 JSON evidence file.

    @param path Destination file replaced by this call.
    @param value JSON-serializable value derived from the real process run.
    @return Nothing.
    @throws OSError If the destination cannot be written.
    @throws TypeError If ``value`` is not JSON serializable.
    @note Keys are sorted and a final newline is always emitted.
    """

    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> int:
    """@brief Builds an isolated fixture and runs graph_cli option mode.

    @return Zero only when help succeeds and the real ``--read`` Host chain
      reaches process-level resource-exhaustion policy without generic CLI
      exception translation.
    @throws OSError If fixture, process, or evidence I/O fails.
    @throws subprocess.TimeoutExpired If either process fails to terminate.
    @note The YAML tag activates the existing BUILD_TESTING-only GraphIO probe
      inside ``Host::load_graph``. Production binaries intentionally fail this
      verification because they do not contain that private test probe.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    binary = Path(args.binary).resolve()
    out = Path(args.out).resolve()
    workspace = out / "workspace"
    if workspace.exists() or workspace.is_symlink():
        shutil.rmtree(workspace)
    workspace.mkdir(parents=True)

    config = workspace / "config.yaml"
    config.write_text(
        "\n".join(
            [
                "plugin_dirs: []",
                "policy_dirs: []",
                "policy_interactive_type: interactive",
                "policy_throughput_type: throughput",
                "execution_hp_type: serial_debug",
                "execution_rt_type: serial_debug",
                "execution_worker_count: 1",
                "",
            ]
        ),
        encoding="utf-8",
    )
    graph = workspace / "bad_alloc_probe.yaml"
    graph.write_text(
        "\n".join(
            [
                "- !photospider-test-reload-bad-alloc",
                "  id: 1",
                "  name: option_mode_resource_exhaustion_probe",
                "  type: image_generator",
                "  subtype: constant",
                "  parameters:",
                "    width: 1",
                "    height: 1",
                "",
            ]
        ),
        encoding="utf-8",
    )
    malformed_config = workspace / "malformed_transaction_config.yaml"
    malformed_config.write_text(
        "\n".join(
            [
                "switch_after_load: false",
                "execution_worker_count: invalid-integer",
                "",
            ]
        ),
        encoding="utf-8",
    )
    valid_graph = workspace / "transaction_graph.yaml"
    valid_graph.write_text(
        "\n".join(
            [
                "- id: 1",
                "  name: transaction_visible_node",
                "  type: image_generator",
                "  subtype: constant",
                "  parameters:",
                "    width: 1",
                "    height: 1",
                "    value: 0",
                "    channels: 3",
                "",
            ]
        ),
        encoding="utf-8",
    )

    help_command = [str(binary), "--help"]
    help_run = run_command(help_command, workspace, args.timeout)
    option_command = [
        str(binary),
        "--config",
        str(config),
        "--read",
        str(graph),
    ]
    option_run = run_command(option_command, workspace, args.timeout)
    transaction_command = [
        str(binary),
        "--config",
        str(malformed_config),
        "--read",
        str(valid_graph),
        "--print",
    ]
    transaction_run = run_command(transaction_command, workspace, args.timeout)

    actual = {
        "binary": str(binary),
        "binary_is_file": binary.is_file(),
        "help": {
            "command": help_command,
            "returncode": help_run.returncode,
            "stdout": help_run.stdout,
            "stderr": help_run.stderr,
        },
        "option_mode": {
            "command": option_command,
            "returncode": option_run.returncode,
            "stdout": option_run.stdout,
            "stderr": option_run.stderr,
            "config_loaded_before_action": "Loaded configuration from"
            in option_run.stdout,
            "resource_exhaustion_diagnostic": RESOURCE_EXHAUSTION_DIAGNOSTIC
            in option_run.stderr,
            "generic_exception_translation_absent": "Error:" not in option_run.stderr,
            "ordinary_load_failure_absent": "Failed to load graph"
            not in option_run.stderr,
        },
        "transactional_config": {
            "command": transaction_command,
            "returncode": transaction_run.returncode,
            "stdout": transaction_run.stdout,
            "stderr": transaction_run.stderr,
            "parse_warning_observed": "Could not parse config file"
            in transaction_run.stderr,
            "graph_loaded": "Loaded graph from" in transaction_run.stdout,
            "default_switch_state_retained": "transaction_visible_node"
            in transaction_run.stdout
            and "No graph loaded; use -r first." not in transaction_run.stderr,
        },
        "repo": str(repo),
    }
    expected = {
        "binary_is_file": True,
        "help_returncode": 0,
        "option_mode_returncode": RESOURCE_EXHAUSTION_EXIT_CODE,
        "config_loaded_before_action": True,
        "resource_exhaustion_diagnostic": RESOURCE_EXHAUSTION_DIAGNOSTIC,
        "generic_exception_translation_absent": True,
        "ordinary_load_failure_absent": True,
        "malformed_config_returncode": 0,
        "malformed_config_parse_warning_observed": True,
        "malformed_config_leaves_preexisting_switch_state_unchanged": True,
        "failure_origin": "BUILD_TESTING GraphIO probe through Host::load_graph",
    }
    checks = {
        "graph_cli binary exists": actual["binary_is_file"],
        "help fast path exits successfully": help_run.returncode == 0,
        "option mode reaches config then Host action": actual["option_mode"][
            "config_loaded_before_action"
        ],
        "Host bad_alloc reaches process resource policy": option_run.returncode
        == RESOURCE_EXHAUSTION_EXIT_CODE,
        "process emits dedicated resource diagnostic": actual["option_mode"][
            "resource_exhaustion_diagnostic"
        ],
        "run boundary does not apply generic exception translation": actual[
            "option_mode"
        ]["generic_exception_translation_absent"],
        "Host bad_alloc is not converted to an ordinary load status": actual[
            "option_mode"
        ]["ordinary_load_failure_absent"],
        "malformed config remains a recoverable parse warning": (
            transaction_run.returncode == 0
            and actual["transactional_config"]["parse_warning_observed"]
        ),
        "failed temporary parse commits no earlier switch field": actual[
            "transactional_config"
        ]["graph_loaded"]
        and actual["transactional_config"]["default_switch_state_retained"],
    }
    passed = all(checks.values())
    compare_lines = ["graph_cli_option_bad_alloc"]
    compare_lines.extend(
        f"{'PASS' if ok else 'FAIL'} {name}" for name, ok in checks.items()
    )
    compare_lines.append(f"overall={'PASS' if passed else 'FAIL'}")

    write_json(out / "expected.json", expected)
    write_json(out / "actual.json", actual)
    (out / "help.stdout.log").write_text(help_run.stdout, encoding="utf-8")
    (out / "help.stderr.log").write_text(help_run.stderr, encoding="utf-8")
    (out / "option.stdout.log").write_text(option_run.stdout, encoding="utf-8")
    (out / "option.stderr.log").write_text(option_run.stderr, encoding="utf-8")
    (out / "transaction.stdout.log").write_text(
        transaction_run.stdout, encoding="utf-8"
    )
    (out / "transaction.stderr.log").write_text(
        transaction_run.stderr, encoding="utf-8"
    )
    (out / "compare.log").write_text("\n".join(compare_lines) + "\n", encoding="utf-8")
    print("\n".join(compare_lines), file=sys.stdout if passed else sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
