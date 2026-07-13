#!/usr/bin/env python3
"""Run graph_cli through external operation/scheduler plugin compute flow."""

from __future__ import annotations

import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time


def parse_args() -> argparse.Namespace:
    """Parse paths injected by the configured CMake build."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--operation-plugin-dir", required=True, type=Path)
    parser.add_argument("--scheduler-plugin", required=True, type=Path)
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


def release_compute_fifo(fifo: Path, stop: threading.Event, failure: list[str]) -> None:
    """Release the external scheduler after it opens the one-shot FIFO."""

    deadline = time.monotonic() + 20.0
    while not stop.is_set() and time.monotonic() < deadline:
        try:
            descriptor = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as error:
            if error.errno == errno.ENXIO:
                time.sleep(0.01)
                continue
            failure.append(f"scheduler FIFO open failed: {error}")
            return
        try:
            os.write(descriptor, b"c")
        finally:
            os.close(descriptor)
        return
    if not stop.is_set():
        failure.append("external scheduler never entered the compute FIFO")


def main() -> int:
    """Create isolated inputs, run the real CLI, and validate the full flow."""

    args = parse_args()
    binary = args.binary.resolve()
    operation_plugin_dir = args.operation_plugin_dir.resolve()
    scheduler_plugin = args.scheduler_plugin.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"graph_cli binary does not exist: {binary}")
    if not operation_plugin_dir.is_dir():
        raise FileNotFoundError(
            "operation plugin directory does not exist: " f"{operation_plugin_dir}"
        )
    if not scheduler_plugin.is_file():
        raise FileNotFoundError(f"scheduler plugin does not exist: {scheduler_plugin}")

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
    scheduler_plugin_dir = work / "scheduler_plugins"
    scheduler_plugin_dir.mkdir()
    shutil.copy2(
        scheduler_plugin,
        scheduler_plugin_dir / scheduler_plugin.name,
    )
    config = work / "config.yaml"
    config.write_text(
        "cache_root_dir: " + yaml_string(cache) + "\n"
        "plugin_dirs:\n"
        "  - " + yaml_string(operation_plugin_dir) + "\n"
        "scheduler_dirs:\n"
        "  - " + yaml_string(scheduler_plugin_dir) + "\n"
        "exit_prompt_sync: false\n"
        "session_warning: false\n"
        "ops_plugin_path_mode: name_only\n"
        "scheduler_hp_type: destroy_count_test\n"
        "scheduler_rt_type: serial_debug\n"
        "scheduler_worker_count: 1\n",
        encoding="utf-8",
    )
    repl_input = (
        f"load cli_plugin_flow {graph}\n"
        "ops plugins\n"
        "scheduler plugins\n"
        "scheduler get all\n"
        "compute all force parallel nosave m\n"
        "inspect 1\n"
        "exit\n"
    )
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    trace = work / "scheduler.trace"
    gate = work / "compute.fifo"
    gate_stop = threading.Event()
    gate_failure: list[str] = []
    gate_thread: threading.Thread | None = None
    if os.name != "nt":
        os.mkfifo(gate, 0o600)
        environment["PS_DESTROY_COUNT_SCHEDULER_TRACE"] = str(trace)
        environment["PS_DESTROY_COUNT_SCHEDULER_COMPUTE_GATE"] = str(gate)

    process = subprocess.Popen(
        [str(binary), "--config", str(config), "--repl"],
        cwd=work,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if os.name != "nt":
        gate_thread = threading.Thread(
            target=release_compute_fifo,
            args=(gate, gate_stop, gate_failure),
            daemon=True,
        )
        gate_thread.start()
    try:
        stdout, stderr = process.communicate(input=repl_input, timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        raise RuntimeError(f"graph_cli timed out\n{stdout}{stderr}") from None
    finally:
        gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=1)

    transcript = stdout + stderr
    if gate_failure:
        raise RuntimeError(f"{gate_failure[0]}\n{transcript}")
    completed_returncode = process.returncode
    if completed_returncode != 0:
        raise RuntimeError(
            f"graph_cli exited with {completed_returncode}\n{transcript}"
        )

    require_output(transcript, "Loaded session 'cli_plugin_flow'")
    require_output(transcript, "Type: plugin_lifecycle")
    require_output(transcript, "Loaded scheduler plugins:")
    require_output(transcript, "destroy_count_test")
    require_output(transcript, "HP (GlobalHighPrecision) Scheduler:")
    require_output(transcript, "Type: destroy_count_test")
    require_output(transcript, "Computation finished.")
    require_output(transcript, "ROI:       (0, 0, 11x7)")
    if os.name != "nt":
        trace_events = trace.read_text(encoding="utf-8").splitlines()
        if "compute_gate_wait" not in trace_events:
            raise RuntimeError(
                "external scheduler trace lacks compute_gate_wait\n" + transcript
            )
        if "compute_gate_release" not in trace_events:
            raise RuntimeError(
                "external scheduler trace lacks compute_gate_release\n" + transcript
            )
    if "Computation failed." in transcript:
        raise RuntimeError(f"graph_cli reported failed compute\n{transcript}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - test entry reports full failure.
        print(error, file=sys.stderr)
        raise SystemExit(1) from error
