#!/usr/bin/env python3
"""Run one protected CI command under a portable process-group deadline.

The helper uses only Python's POSIX process APIs available on maintained
Darwin and Linux runners. The child receives a fresh process group so a
deadline terminates configure/build tools and their descendants together. A
timeout exits with status 124; ordinary child statuses pass through unchanged.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import signal
import subprocess
import sys
from collections.abc import Sequence


TIMEOUT_EXIT_STATUS = 124
TERMINATION_GRACE_SECONDS = 2


def _positive_integer(value: str) -> int:
    """Parse a canonical positive decimal CLI value.

    Args:
        value: User-provided timeout text.

    Returns:
        The strictly positive integer represented by ``value``.

    Raises:
        argparse.ArgumentTypeError: If the value is zero, signed, padded, or
            otherwise non-canonical.
    """
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise argparse.ArgumentTypeError("timeout must be a canonical positive integer")
    return int(value)


def _terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    """Terminate a timed-out child process group without leaking descendants.

    Args:
        process: Group-leading child created with ``start_new_session=True``.

    Returns:
        Nothing. The child has exited when the function returns.

    Raises:
        subprocess.TimeoutExpired: Never; escalation to ``SIGKILL`` handles a
            child that ignores the grace-period ``SIGTERM``.

    Note:
        A concurrent natural child exit is accepted through
        ``ProcessLookupError`` and the final ``wait``.
    """
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=TERMINATION_GRACE_SECONDS)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_with_timeout(command: Sequence[str], timeout_seconds: int, label: str) -> int:
    """Execute a command and impose one wall-clock deadline on its process tree.

    Args:
        command: Exact argv sequence; no shell parsing is performed.
        timeout_seconds: Positive total wall-clock bound.
        label: Safe diagnostic identity for the bounded operation.

    Returns:
        The child's exit status, or 124 after terminating the complete process
        group on timeout.

    Raises:
        OSError: If the command cannot be started.
        ValueError: If the caller supplies an empty command or invalid bound.

    Note:
        Output streams are inherited so existing CI logging remains intact.
    """
    if not command:
        raise ValueError("bounded command is empty")
    if timeout_seconds <= 0:
        raise ValueError("timeout must be positive")
    process = subprocess.Popen(list(command), start_new_session=True)
    try:
        return process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        _terminate_process_group(process)
        print(
            f"{label} exceeded its declared {timeout_seconds}-second job timeout; "
            "terminated the process group.",
            file=sys.stderr,
        )
        return TIMEOUT_EXIT_STATUS
    except OverflowError as error:
        _terminate_process_group(process)
        raise ValueError("timeout exceeds the maintained platform clock range") from error


def build_parser() -> argparse.ArgumentParser:
    """Build the explicit Darwin/Linux timeout-wrapper command line.

    Returns:
        An argument parser requiring one timeout unit, a diagnostic label, and
        an exact command following ``--``.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    timeout = parser.add_mutually_exclusive_group(required=True)
    timeout.add_argument("--timeout-seconds", type=_positive_integer)
    timeout.add_argument("--timeout-minutes", type=_positive_integer)
    parser.add_argument("--label", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    return parser


def main() -> int:
    """Validate the maintained platform boundary and run the bounded command.

    Returns:
        The exact child status, 124 on deadline, or a normalized setup failure.
    """
    parser = build_parser()
    arguments = parser.parse_args()
    if os.name != "posix" or platform.system() not in {"Darwin", "Linux"}:
        parser.error("the protected timeout wrapper supports Darwin/Linux only")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]+", arguments.label):
        parser.error("label must be a canonical diagnostic identifier")
    command = list(arguments.command)
    if command[:1] == ["--"]:
        command.pop(0)
    if not command:
        parser.error("an exact command is required after --")
    seconds = arguments.timeout_seconds
    if seconds is None:
        seconds = arguments.timeout_minutes * 60
    try:
        return run_with_timeout(command, seconds, arguments.label)
    except (OSError, ValueError) as error:
        print(f"protected command timeout failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
