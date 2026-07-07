#!/usr/bin/env python3
"""Verify CLI cache_root_dir controls GraphModel disk cache placement."""

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
    sent_compute = False
    sent_exit = False
    sent_exit_answer = False
    while time.time() - start < timeout_s:
        output.extend(read_available(master_fd, 0.25))
        text = output.decode("utf-8", errors="replace")
        if not sent_compute and "ps>" in text:
            os.write(master_fd, b"compute 1 force m\n")
            sent_compute = True
        if sent_compute and not sent_exit and text.count("ps>") >= 2:
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


def rel(path, base):
    try:
        return str(path.relative_to(base))
    except ValueError:
        return str(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    binary = pathlib.Path(args.binary).resolve()
    out_dir = pathlib.Path(args.out).resolve()
    workspace = out_dir / "workspace"

    if workspace.exists():
        shutil.rmtree(workspace)
    workspace.mkdir(parents=True)

    config = workspace / "cache_root_config.yaml"
    config.write_text(
        "\n".join(
            [
                "cache_root_dir: configured_cache",
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

    graph = workspace / "cache_graph.yaml"
    graph.write_text(
        """- id: 1
  name: source
  type: image_generator
  subtype: constant
  parameters:
    width: 8
    height: 8
    value: 128
    channels: 3
  caches:
    - cache_type: image
      location: source.png
""",
        encoding="utf-8",
    )

    returncode, stdout = run_graph_cli(binary, config, graph, workspace, 20.0)
    clean_stdout = ANSI_RE.sub("", stdout).replace("\r", "")

    configured_cache_file = (
        workspace / "configured_cache" / "default" / "1" / "source.png"
    )
    legacy_session_cache_file = (
        workspace / "sessions" / "default" / "cache" / "1" / "source.png"
    )
    expected = {
        "configured_cache_file": rel(configured_cache_file, repo),
        "legacy_session_cache_file": rel(legacy_session_cache_file, repo),
        "configured_cache_exists": True,
        "legacy_session_cache_exists": False,
    }
    actual = {
        "returncode": returncode,
        "configured_cache_file": rel(configured_cache_file, repo),
        "legacy_session_cache_file": rel(legacy_session_cache_file, repo),
        "configured_cache_exists": configured_cache_file.exists(),
        "legacy_session_cache_exists": legacy_session_cache_file.exists(),
        "stdout_mentions_compute_failure": "Compute task failed" in clean_stdout,
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
        and actual["configured_cache_exists"] is True
        and actual["legacy_session_cache_exists"] is False
        and actual["stdout_mentions_compute_failure"] is False
    )
    compare = [
        f"returncode={returncode}",
        f"configured_cache_exists={actual['configured_cache_exists']}",
        f"legacy_session_cache_exists={actual['legacy_session_cache_exists']}",
        f"stdout_mentions_compute_failure={actual['stdout_mentions_compute_failure']}",
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
