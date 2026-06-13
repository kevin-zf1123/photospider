#!/usr/bin/env python3
"""Verify CLI config scheduler fields reach Kernel before graph load."""

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


def read_available(fd: int, timeout_s: float) -> bytes:
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


def run_graph_cli(binary: pathlib.Path, config: pathlib.Path,
                  graph: pathlib.Path, workspace: pathlib.Path,
                  timeout_s: float):
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
    sent_get = False
    sent_exit = False
    sent_exit_answer = False
    while time.time() - start < timeout_s:
        output.extend(read_available(master_fd, 0.25))
        text = output.decode("utf-8", errors="replace")
        if not sent_get and "ps>" in text:
            os.write(master_fd, b"scheduler get\n")
            sent_get = True
        if sent_get and not sent_exit and "RT (RealTimeUpdate) Scheduler:" in text:
            if text.count("ps>") >= 2:
                os.write(master_fd, b"exit\n")
                sent_exit = True
        if sent_exit and not sent_exit_answer and "before exiting?" in text:
            os.write(master_fd, b"n\n")
            sent_exit_answer = True
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


def parse_scheduler_types(stdout: str) -> dict:
    clean = ANSI_RE.sub("", stdout).replace("\r", "")
    hp_match = re.search(
        r"HP \(GlobalHighPrecision\) Scheduler:\n\s*Type:\s*([^\n]+)",
        clean,
    )
    rt_match = re.search(
        r"RT \(RealTimeUpdate\) Scheduler:\n\s*Type:\s*([^\n]+)",
        clean,
    )
    return {
        "hp_type": hp_match.group(1).strip() if hp_match else None,
        "rt_type": rt_match.group(1).strip() if rt_match else None,
    }


def main() -> int:
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

    config = workspace / "scheduler_config.yaml"
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

    expected = {
        "hp_type": "serial_debug",
        "rt_type": "serial_debug",
        "source": "config loaded before graph load",
    }

    returncode, stdout = run_graph_cli(binary, config, graph, workspace, 20.0)
    actual_types = parse_scheduler_types(stdout)
    actual = {
        **actual_types,
        "returncode": returncode,
        "config": str(config.relative_to(repo)) if config.is_relative_to(repo) else str(config),
        "graph": str(graph.relative_to(repo)) if graph.is_relative_to(repo) else str(graph),
    }

    (out_dir / "stdout.log").write_text(stdout, encoding="utf-8")
    (out_dir / "expected.json").write_text(
        json.dumps(expected, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (out_dir / "actual.json").write_text(
        json.dumps(actual, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    passed = (
        returncode == 0
        and actual_types["hp_type"] == expected["hp_type"]
        and actual_types["rt_type"] == expected["rt_type"]
    )
    compare = [
        f"returncode={returncode}",
        f"expected.hp_type={expected['hp_type']} actual.hp_type={actual_types['hp_type']}",
        f"expected.rt_type={expected['rt_type']} actual.rt_type={actual_types['rt_type']}",
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
