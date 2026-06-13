#!/usr/bin/env python3
"""Verify CLI exposes read-only dirty snapshot inspection."""

import argparse
import json
import os
import pathlib
import pty
import re
import select
import shutil
import subprocess
import sys
import time


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def read_available(fd, timeout_s):
    chunks = []
    end = time.time() + timeout_s
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            data = os.read(fd, 4096)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks)


def run_graph_cli(binary, config, graph, workspace, timeout_s):
    env = os.environ.copy()
    env["HOME"] = str(workspace)
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [
            str(binary),
            "--config",
            str(config),
            "--read",
            str(graph),
            "--repl",
        ],
        cwd=workspace,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        env=env,
        close_fds=True,
    )
    os.close(slave_fd)

    output = bytearray()
    start = time.time()
    sent_inspect = False
    sent_exit = False
    sent_exit_answer = False
    while time.time() - start < timeout_s:
        output.extend(read_available(master_fd, 0.25))
        text = output.decode("utf-8", errors="replace")
        if not sent_inspect and "ps>" in text:
            os.write(master_fd, b"inspect dirty\n")
            sent_inspect = True
        if sent_inspect and not sent_exit and "(No dirty snapshot recorded.)" in text:
            if text.count("ps>") >= 2:
                os.write(master_fd, b"exit\n")
                sent_exit = True
        if sent_exit and not sent_exit_answer and "before exiting?" in text:
            os.write(master_fd, b"n\n")
            sent_exit_answer = True
        if sent_exit and proc.poll() is not None:
            output.extend(read_available(master_fd, 0.25))
            break
        if sent_exit_answer and proc.poll() is not None:
            output.extend(read_available(master_fd, 0.25))
            break

    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
    os.close(master_fd)
    return proc.returncode or 0, output.decode("utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--graph", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    binary = pathlib.Path(args.binary).resolve()
    graph = pathlib.Path(args.graph).resolve()
    out_dir = pathlib.Path(args.out).resolve()
    workspace = out_dir / "workspace"

    if workspace.exists():
        shutil.rmtree(workspace)
    workspace.mkdir(parents=True)

    config = workspace / "cli_config.yaml"
    config.write_text(
        "\n".join(
            [
                "cache_root_dir: cache",
                "plugin_dirs: []",
                "scheduler_dirs: []",
                "cache_precision: int8",
                "history_size: 10",
                "exit_prompt_sync: false",
                "scheduler_hp_type: serial_debug",
                "scheduler_rt_type: serial_debug",
                "scheduler_worker_count: 1",
                "",
            ]
        ),
        encoding="utf-8",
    )

    returncode, stdout = run_graph_cli(binary, config, graph, workspace, 20.0)
    clean = ANSI_RE.sub("", stdout).replace("\r", "")
    expected = {
        "contains": "(No dirty snapshot recorded.)",
        "does_not_contain": [
            "Invalid node id 'dirty'.",
            "Usage: inspect <node_id>|all",
            "No current graph.",
        ],
    }
    actual = {
        "returncode": returncode,
        "contains_empty_dirty_snapshot_message": expected["contains"] in clean,
        "contains_invalid_node_id": "Invalid node id 'dirty'." in clean,
        "contains_old_usage": "Usage: inspect <node_id>|all" in clean,
        "contains_no_current_graph": "No current graph." in clean,
        "graph": str(graph.relative_to(repo)) if graph.is_relative_to(repo) else str(graph),
    }

    passed = (
        returncode == 0
        and actual["contains_empty_dirty_snapshot_message"]
        and not actual["contains_invalid_node_id"]
        and not actual["contains_old_usage"]
        and not actual["contains_no_current_graph"]
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "stdout.log").write_text(stdout, encoding="utf-8")
    (out_dir / "expected.json").write_text(
        json.dumps(expected, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (out_dir / "actual.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    compare = [
        f"returncode={returncode}",
        "contains_empty_dirty_snapshot_message="
        + str(actual["contains_empty_dirty_snapshot_message"]),
        "contains_invalid_node_id=" + str(actual["contains_invalid_node_id"]),
        "contains_old_usage=" + str(actual["contains_old_usage"]),
        "contains_no_current_graph=" + str(actual["contains_no_current_graph"]),
        f"overall={'PASS' if passed else 'FAIL'}",
    ]
    (out_dir / "compare.log").write_text("\n".join(compare) + "\n",
                                          encoding="utf-8")
    if not passed:
        print("\n".join(compare), file=sys.stderr)
        return 1
    print("\n".join(compare))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
