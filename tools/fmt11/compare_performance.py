#!/usr/bin/env python3
"""Paired, warmed FMT-11 benchmark; retain failures and all raw observations.

Both binaries must be built from the same benchmark main.cpp and flags, against
baseline/candidate implementations respectively. No third-party Python package
is needed. This script does not detect unrelated system workloads.
"""
import argparse
import csv
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time


def matrix(accelerated_profile="x86"):
    result = []
    def add(name, **values):
        params = dict(member="M", dtype="f32", profile="strict", algorithm="auto",
                      mode="math", width=17, height=1, workers=1, tile=128,
                      roi_width=0, work=1000000000000)
        params.update(values)
        result.append((name, params))
    for member in ("M", "E"):
        for dtype in ("f32", "f64"):
            for algorithm in ("auto", "reference"):
                add(f"{member}-{dtype}-math-{algorithm}", member=member,
                    dtype=dtype, algorithm=algorithm)
    for member in ("M", "E"):
        for mode in ("generic", "planar"):
            for profile in ("strict", "x86"):
                add(f"{member}-f32-{mode}-{profile}-full", member=member,
                    mode=mode, profile=profile, width=1024, height=64)
    for member in ("M", "E"):
        for profile in ("strict", "x86"):
            add(f"{member}-f32-planar-{profile}-roi7", member=member,
                mode="planar", profile=profile, width=1024, height=64, roi_width=7)
    for workers in (2, 4):
        add(f"M-f32-planar-x86-workers{workers}", mode="planar", profile="x86",
            width=1024, height=64, workers=workers)
    for tile in (64, 256):
        add(f"M-f32-planar-x86-tile{tile}", mode="planar", profile="x86",
            width=1024, height=64, tile=tile)
    add("M-f32-planar-x86-scalar", mode="planar", profile="x86", algorithm="scalar",
        width=1024, height=64)
    for dtype in ("f32", "f64"):
        for algorithm in ("auto", "scalar"):
            add(f"M-{dtype}-planar-x86-tail133-{algorithm}", mode="planar",
                profile="x86", dtype=dtype, algorithm=algorithm, width=133, height=2)
    for mode in ("generic", "planar"):
        add(f"Q-f32-{mode}-copy-control", member="Q", mode=mode,
            width=1024, height=64)
    return [(name.replace("-x86-", "-" + accelerated_profile + "-"),
             dict(params, profile=accelerated_profile) if params["profile"] == "x86" else params)
            for name, params in result]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def comparable_counters(rows):
    modes = {row["mode"] for row in rows}
    if len(modes) != 1:
        raise RuntimeError("cannot compare different benchmark modes")
    return ("checksum", "accepted", "reference", "refinements", "math_work") if modes == {"math"} else ("checksum",)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--repeats", type=int, default=9)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--case", action="append", help="exact case name; may repeat")
    parser.add_argument("--accelerated-profile", choices=("x86", "apple"),
                        default="apple" if platform.system() == "Darwin" and platform.machine() == "arm64" else "x86")
    parser.add_argument("--cpus", help="comma-separated allowed CPU IDs; Linux only")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.pairs < 3 or args.repeats < 1 or args.warmup < 0 or args.timeout <= 0:
        parser.error("require pairs>=3, repeats>=1, warmup>=0, timeout>0")
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    for path in binaries.values():
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f"not executable: {path}")
    # Fail before a long series when an EXCLUDE_FROM_ALL benchmark target is stale.
    preflight = {}
    for variant, path in binaries.items():
        command = [str(path), "--member", "M", "--width", "17", "--height", "1",
                   "--warmup", "0", "--repeats", "1"]
        try:
            check = subprocess.run(command, capture_output=True, text=True,
                                   timeout=args.timeout, check=True)
            rows = list(csv.DictReader(io.StringIO(check.stdout)))
            if (len(rows) != 1 or rows[0].get("warmup") != "0"
                    or not rows[0].get("timings_us")):
                raise ValueError("missing warmup/per-iteration CSV fields")
            preflight[variant] = dict(command=command, stdout=check.stdout)
        except (OSError, subprocess.SubprocessError, ValueError) as error:
            parser.error(f"{variant} harness preflight failed; explicitly rebuild "
                         f"photospider_model_conversion_performance: {error}")
    cases = matrix(args.accelerated_profile)
    if args.case:
        missing = set(args.case) - {name for name, _ in cases}
        if missing:
            parser.error(f"unknown cases: {sorted(missing)}")
        cases = [(name, params) for name, params in cases if name in args.case]
    allowed = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    cpus = [int(x) for x in args.cpus.split(",")] if args.cpus else allowed[:4]
    if cpus and (not allowed or not set(cpus) <= set(allowed)):
        parser.error("selected CPUs not in the process affinity")
    args.output.mkdir(parents=True, exist_ok=True)
    raw_path = args.output / "raw.jsonl"
    # Do not silently overwrite an earlier measurement series.
    if raw_path.exists():
        parser.error(f"measurement series already exists: {raw_path}")
    env = dict(platform=platform.platform(), machine=platform.machine(),
               python=sys.version, allowed_cpus=allowed, benchmark_cpus=cpus,
               pairs=args.pairs, repeats=args.repeats, warmup=args.warmup,
               harness_requirement="same main.cpp and build flags",
               preflight=preflight,
               binary_sha256={key: sha256(path) for key, path in binaries.items()},
               start_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    for path in ("/proc/cpuinfo", "/proc/meminfo", "/sys/fs/cgroup/cpu.max",
                 "/sys/fs/cgroup/memory.max"):
        try:
            env[path] = Path(path).read_text()
        except OSError:
            pass
    (args.output / "environment.json").write_text(json.dumps(env, indent=2))
    rng = random.Random(0x464D543131)
    observations = {}
    failures = []
    with open("/tmp/photospider-performance.lock", "w") as lock, raw_path.open("w") as raw:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # Randomize the case order too, rather than always measuring slow paths last.
        rng.shuffle(cases)
        for name, params in cases:
            observations[name] = {"baseline": [], "candidate": []}
            for pair in range(args.pairs):
                order = ["baseline", "candidate"]
                if rng.randrange(2):
                    order.reverse()
                for variant in order:
                    command = [str(binaries[variant])]
                    values = dict(params, warmup=args.warmup, repeats=args.repeats)
                    for key, value in values.items():
                        command += ["--" + key.replace("_", "-"), str(value)]
                    affinity = cpus[:min(len(cpus), params["workers"])]
                    preexec = (lambda: os.sched_setaffinity(0, affinity)) if affinity else None
                    started = time.time()
                    record = dict(case=name, variant=variant, pair=pair, command=command,
                                  cpus=affinity, started_unix=started)
                    try:
                        completed = subprocess.run(command, capture_output=True, text=True,
                                                   timeout=args.timeout, preexec_fn=preexec)
                        record.update(returncode=completed.returncode, stdout=completed.stdout,
                                      stderr=completed.stderr, elapsed_s=time.time()-started)
                        if completed.returncode:
                            raise RuntimeError(f"exit {completed.returncode}: {completed.stderr}")
                        rows = list(csv.DictReader(io.StringIO(completed.stdout)))
                        if len(rows) != 1:
                            raise RuntimeError("expected exactly one CSV row")
                        row = rows[0]
                        timings = [float(x) for x in row["timings_us"].split(";")]
                        if len(timings) != args.repeats or any(x <= 0 for x in timings):
                            raise RuntimeError("invalid per-iteration timings")
                        record["row"] = row
                        observations[name][variant].append(row)
                    except (OSError, subprocess.TimeoutExpired, RuntimeError, ValueError, KeyError) as error:
                        record["error"] = str(error)
                        failures.append(f"{name}/{variant}/{pair}: {error}")
                    raw.write(json.dumps(record) + "\n")
                    raw.flush()
            print(name, "completed", flush=True)
    summary = []
    for name, variants in sorted(observations.items()):
        if any(len(rows) != args.pairs for rows in variants.values()):
            continue
        # All runs, not just each matched pair, must agree on output and work.
        for key in comparable_counters([row for rows in variants.values() for row in rows]):
            values = {row[key] for rows in variants.values() for row in rows}
            if len(values) != 1:
                failures.append(f"{name}: nonidentical {key}: {sorted(values)}")
        item = {"case": name, "pairs": args.pairs, "repeats": args.repeats, "warmup": args.warmup}
        medians = {}
        for variant, rows in variants.items():
            samples = [float(row["median_us"]) for row in rows]
            medians[variant] = statistics.median(samples)
            item[variant + "_median_us"] = medians[variant]
            item[variant + "_process_min_us"] = min(samples)
            item[variant + "_process_max_us"] = max(samples)
            item[variant + "_process_medians_us"] = ";".join(map(str, samples))
        item["speedup"] = medians["baseline"] / medians["candidate"]
        item["time_reduction_percent"] = (1 - medians["candidate"] / medians["baseline"]) * 100
        item["pair_speedups"] = ";".join(str(float(a["median_us"]) / float(b["median_us"]))
                                          for a, b in zip(variants["baseline"], variants["candidate"]))
        for key in ("checksum", "accepted", "reference", "refinements", "math_work",
                    "root_peak_host_bytes", "run_live_payload_bytes", "run_live_metadata_bytes",
                    "source_payload_bytes", "source_logical_bytes", "issued_work",
                    "numeric_evaluated", "numeric_copied", "numeric_views",
                    "strict_math_calls", "strict_fallbacks",
                    "peak_buffer_bytes", "copy_bytes", "tiles"):
            item[key] = variants["candidate"][0].get(key, "")
        summary.append(item)
    if summary:
        with (args.output / "summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(summary[0]))
            writer.writeheader()
            writer.writerows(summary)
    verdict = dict(cases=len(cases), completed_cases=len(summary), expected_processes=len(cases)*args.pairs*2,
                   failures=failures, passed=not failures,
                   finished_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    (args.output / "verdict.json").write_text(json.dumps(verdict, indent=2))
    print(json.dumps(verdict, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
