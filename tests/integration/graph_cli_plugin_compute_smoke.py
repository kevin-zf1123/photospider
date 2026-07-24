#!/usr/bin/env python3
"""Run graph_cli through external operation/policy plugin compute flow."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    """Parse paths injected by the configured CMake build."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--operation-plugin-dir", required=True, type=Path)
    parser.add_argument("--policy-plugin", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    return parser.parse_args()


def yaml_string(value: Path | str) -> str:
    """Return one JSON-quoted scalar, which is also valid YAML."""

    return json.dumps(str(value))


def require_output(output: str, fragment: str) -> None:
    """Fail with the complete process transcript when a marker is absent."""

    if fragment not in output:
        raise RuntimeError(
            f"graph_cli output lacks required marker {fragment!r}\n{output}"
        )


def main() -> int:
    """Create isolated inputs, run the real CLI, and validate the full flow."""

    args = parse_args()
    binary = args.binary.resolve()
    operation_plugin_dir = args.operation_plugin_dir.resolve()
    policy_plugin = args.policy_plugin.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"graph_cli binary does not exist: {binary}")
    if not operation_plugin_dir.is_dir():
        raise FileNotFoundError(
            "operation plugin directory does not exist: " f"{operation_plugin_dir}"
        )
    if not policy_plugin.is_file():
        raise FileNotFoundError(f"policy plugin does not exist: {policy_plugin}")

    work = args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    home = work / "home"
    home.mkdir()
    graph = work / "external_plugins.yaml"
    graph.write_text(
        "- id: 1\n"
        "  name: external_plugin_source\n"
        "  type: plugin_lifecycle\n"
        "  subtype: op\n",
        encoding="utf-8",
    )
    cache = work / "cache"
    policy_plugin_dir = work / "policy_plugins"
    policy_plugin_dir.mkdir()
    shutil.copy2(
        policy_plugin,
        policy_plugin_dir / policy_plugin.name,
    )
    config = work / "config.yaml"
    config.write_text(
        "cache_root_dir: " + yaml_string(cache) + "\n"
        "plugin_dirs:\n"
        "  - " + yaml_string(operation_plugin_dir) + "\n"
        "policy_dirs:\n"
        "  - " + yaml_string(policy_plugin_dir) + "\n"
        "exit_prompt_sync: false\n"
        "session_warning: false\n"
        "ops_plugin_path_mode: name_only\n"
        "policy_interactive_type: fixture_policy\n"
        "policy_throughput_type: fixture_policy\n"
        "execution_hp_type: cpu\n"
        "execution_rt_type: cpu\n"
        "execution_worker_count: 1\n",
        encoding="utf-8",
    )
    repl_input = (
        f"load cli_plugin_flow {graph}\n"
        "ops plugins\n"
        "policy plugins\n"
        "policy get all\n"
        "execution list\n"
        "execution get all\n"
        "compute all force parallel nosave m\n"
        "inspect 1\n"
        "exit\n"
    )
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    process = subprocess.Popen(
        [str(binary), "--config", str(config), "--repl"],
        cwd=work,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        stdout, stderr = process.communicate(input=repl_input, timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        raise RuntimeError(f"graph_cli timed out\n{stdout}{stderr}") from None
    transcript = stdout + stderr
    completed_returncode = process.returncode
    if completed_returncode != 0:
        raise RuntimeError(
            f"graph_cli exited with {completed_returncode}\n{transcript}"
        )

    require_output(transcript, "Loaded session 'cli_plugin_flow'")
    require_output(transcript, "Type: plugin_lifecycle")
    require_output(transcript, "Loaded policy plugins:")
    require_output(transcript, "fixture_policy")
    require_output(transcript, "interactive: fixture_policy (generation ")
    require_output(transcript, "throughput: fixture_policy (generation ")
    require_output(transcript, "Available execution types:")
    require_output(transcript, "hp: cpu - ")
    require_output(transcript, "rt: cpu - ")
    require_output(transcript, "Computation finished.")
    require_output(transcript, "ROI:       (0, 0, 11x7)")
    if "Computation failed." in transcript:
        raise RuntimeError(f"graph_cli reported failed compute\n{transcript}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - test entry reports full failure.
        print(error, file=sys.stderr)
        raise SystemExit(1) from error
