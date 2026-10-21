"""Behavior of the actual compiler/executor processes and their file contracts."""
import copy
import errno
import hashlib
import json
import os
import signal
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

psc, psx, plugin, builtins = sys.argv[1:]
checks = 0


def run(tool, *args, code=0, data=None):
    global checks
    result = subprocess.run([tool, *(a if isinstance(a, bytes) else str(a) for a in args)], input=data, capture_output=True, timeout=30)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    checks += 1
    return result


def write(path, value):
    Path(path).write_text(json.dumps(value), encoding="utf8")


# Compiler diagnostics are accepted only by the compiler driver.
for tool in [psc, psx]:
    assert b"fixed invocation ABI 8; operation module ABI 8" in run(tool, "--version").stdout
warning_flags = ["-Wall", "-Wextra", "-Werror", "-Wno-error",
                 "-Wunused-optimizer", "-Wno-unused-optimizer",
                 "-Werror=unused-optimizer", "-Wno-error=unused-optimizer"]
for args in [["-v"], ["--verbose"], ["--error-limit", "0"],
             ["--error-limit=1"], *[[flag] for flag in warning_flags]]:
    rejected = run(psx, "--help", *args, "--diagnostic-format=json", code=2)
    assert "unknown option" in json.loads(rejected.stderr)["message"]
    run(psc, "--help", *args)
for flag in ["-Wunknown", "-Werror=unknown"]:
    assert b"unknown warning group" in run(psc, "--help", flag, code=2).stderr
    assert b"unknown option" in run(psx, "--help", flag, code=2).stderr
for value in ["-1", "invalid"]:
    run(psc, "--help", "--error-limit", value, code=2)
# Diagnostic pre-scanning must use each driver's actual value-taking options.
compiler_limit = run(psc, "--error-limit", "--diagnostic-format=json", code=2)
assert not compiler_limit.stderr.lstrip().startswith(b"{")
executor_limit = run(psx, "--error-limit", "--diagnostic-format=json", code=2)
assert "unknown option" in json.loads(executor_limit.stderr)["message"]

with tempfile.TemporaryDirectory(prefix="photospider-cli-") as directory:
    os.chdir(directory)
    sha = hashlib.sha256(b"\x03").hexdigest()
    Path("scale.bin").write_bytes(b"\x03")
    resources = {"format": "photospider.resources", "version": 1,
                 "resources": [{"path": "scale.bin", "byte_length": 1, "sha256": sha}]}
    write("resources.json", resources)
    schema = {"id": "fixture.vector", "version": 1,
              "tensors": [{"key": "samples", "dtype": "f64", "shape": [4]}]}
    dag = {"format": "photospider.dag", "version": 1, "schema_version": 7,
           "inputs": [{"id": 1, "name": "x", "schema": schema}],
           "nodes": [{"id": 10, "operation": "fixture.resource_scale", "inputs": [{"input": 1}],
                      "parameters": {"sha256": sha}},
                     {"id": 20, "operation": "fixture.resource_scale", "inputs": [{"node": 10, "port": "value"}],
                      "parameters": {"sha256": sha}}],
           "outputs": [{"name": "first", "node": 10, "port": "value"},
                       {"name": "second", "node": 20, "port": "value"}]}
    write("input.json", dag)
    Path("input.bin").write_bytes(struct.pack("<4d", 1, 2, 3, 4))
    bindings = {"format": "photospider.bindings", "version": 1,
                "inputs": [{"name": "x", "tensors": [{"slot": 0, "path": "input.bin",
                 "encoding": "raw-le", "dtype": "f64", "shape": [4]}]}]}
    write("bindings.json", bindings)
    common = ["--load-ops", plugin, "--resources", "resources.json"]
    # Nonspatial layouts must reject axis fields instead of ignoring bad types.
    malformed_layout = copy.deepcopy(dag)
    malformed_layout["inputs"][0]["schema"]["tensors"][0]["layout"] = {
        "spatial": False, "height_axis": "wrong"}
    write("malformed-layout.json", malformed_layout)
    rejected = run(psc, "malformed-layout.json", *common, "--check", code=1)
    assert b"unknown JSON field: height_axis" in rejected.stderr
    runtime = common + ["--bindings", "bindings.json"]
    run(psc, "input.json", *common, "--tile-extent", 2, "--save-ir", "0=raw.psir", "-o", "plain.psxp")
    assert Path("raw.psir").read_text().startswith("psir 5")
    run(psc, "input.json", *common, "--planner", "static-tiles", "--tile-extent", 2,
        "-o", "explicit.psxp")
    assert Path("explicit.psxp").read_bytes() == Path("plain.psxp").read_bytes()
    pipeline = run(psc, "input.json", "--planner=static-tiles", "--print-pipeline")
    assert b"planner: static-tiles@2 -> validate Plan" in pipeline.stdout
    assert b"--planner NAME" in run(psc, "--help").stdout
    for query in [["--list-planners"], ["--help-planner", "static-tiles"]]:
        info = run(psc, *query, "--load-ops", "absent-library")
        assert b"static-tiles@2" in info.stdout
        assert b"dependency_paths" in info.stdout and b"control_flow" in info.stdout
    assert b"--max-tasks" in run(psc, "--help-planner=static-tiles").stdout
    assert b"unknown planner" in run(psc, "--help-planner", "missing", code=2).stderr
    run(psc, "--help-planner", code=2)

    for name in ["missing", "cpu"]:
        rejected = run(psc, "input.json", "--planner=" + name,
                       "--load-ops", "absent-library", "-o", "unknown.psxp", code=2)
        assert b"unknown planner" in rejected.stderr
        assert not Path("unknown.psxp").exists()
    run(psc, "input.json", "--planner=", code=2)
    run(psc, "input.json", "--planner", code=2)
    run(psc, "input.json", "--planner", "static-tiles", "--planner", "static-tiles", code=2)
    for endpoint in [["--check"], ["--emit", "ir"], ["--emit", "ir:0"]]:
        run(psc, "input.json", "--planner", "static-tiles", *endpoint, code=2)
    run(psx, "plain.psxp", "--planner", "static-tiles", code=2)

    run(psx, "plain.psxp", *runtime, "-o", "plain.results", "--threads", 2, "--parallelism", 2)

    def values(directory):
        manifest = json.loads((Path(directory) / "manifest.json").read_text())
        result = {}
        for output in manifest["outputs"]:
            items = {}
            for fragment in output["tensors"][0]["fragments"]:
                raw = (Path(directory) / fragment["path"]).read_bytes()
                assert len(raw) == fragment["byte_length"] == fragment["shape"][0] * 8
                for i, value in enumerate(struct.unpack("<" + "d" * (len(raw) // 8), raw)):
                    offset = fragment["offset"][0] + i
                    assert offset not in items
                    items[offset] = value
            result[output["name"]] = items
        return result

    expected = {"first": dict(enumerate([3., 6., 9., 12.])), "second": dict(enumerate([9., 18., 27., 36.]))}
    assert values("plain.results") == expected
    run(psc, "raw.psir", *common, "-O", "optimize", "-O", "optimize", "--emit", "ir",
        "--save-ir", "0=again.psir", "--save-ir", "1=one.psir", "--release-ir-after-save", 0, "-o", "two.psir")
    assert Path("again.psir").read_bytes() == Path("raw.psir").read_bytes()
    for name in ["raw", "one", "two"]:
        run(psc, name + ".psir", *common, "--planner", "static-tiles", "--tile-extent", 2, "-o", name + ".psxp")
        run(psx, name + ".psxp", *runtime, "-o", name + ".results")
        assert values(name + ".results") == expected
    requests = {"format": "photospider.requests", "version": 1,
                "outputs": [{"name": "second", "members": [{"slot": 0, "intervals": [[1, 2]]}]}]}
    write("requests.json", requests)
    run(psx, "plain.psxp", *runtime, "--requests", "requests.json", "-o", "partial.results")
    assert values("partial.results") == {"second": {2: 27., 3: 36.}}
    run(psc, "input.json", *common, "--tile-extent", 2, "--requests", "requests.json", "-o", "partial.psxp")
    run(psx, "partial.psxp", *runtime, "-o", "compiled-partial.results")
    assert values("compiled-partial.results") == {"second": {2: 27., 3: 36.}}
    requests["outputs"][0]["members"][0]["intervals"] = [[0, 2]]
    write("requests.json", requests)
    run(psx, "partial.psxp", *runtime, "--requests", "requests.json", "-o", "invalid.results", code=1)
    assert not Path("invalid.results").exists()
    run(psx, "plain.psxp", *runtime, "-o", "plain.results", code=4)
    run(psx, "plain.psxp", *runtime, "--check", "--max-live-bytes", 0, code=2)
    run(psx, "plain.psxp", *runtime, "--check", "--max-live-bytes", 1, code=1)
    run(psx, "plain.psxp", *common, "--check")
    run(psx, "plain.psxp", *common, code=1)
    run(psx, "plain.psxp", *common, "-O", "optimize", code=2)
    run(psc, "input.json", *common, "--bindings", "bindings.json", code=2)
    run(psc, "input.json", *common, "--check")
    text_mode = run(psc, "--diagnostic-format", "text", "--",
                    "--diagnostic-format=json", code=4)
    assert not text_mode.stderr.lstrip().startswith(b"{")
    text_mode = run(psc, "-o", "--diagnostic-format=json", "missing.psir", code=4)
    assert not text_mode.stderr.lstrip().startswith(b"{")
    for tool, value_option in [(psc, "-o"), (psx, "-o"), (psc, "--emit"),
                               (psc, "-O"), (psx, "--bindings")]:
        text_mode = run(tool, value_option, "--diagnostic-format=json", "--unknown", code=2)
        assert not text_mode.stderr.lstrip().startswith(b"{")
    for tool in [psc, psx]:
        malformed = run(tool, "--unknown", "--diagnostic-format=json", code=2)
        assert json.loads(malformed.stderr)["severity"] == "error"
        malformed = run(tool, "-o", "--", "--unknown", "--diagnostic-format=json", code=2)
        assert json.loads(malformed.stderr)["severity"] == "error"
    run(psc, "input.json", *common, "--emit", "ir", "-ojoined=output.psir")
    assert Path("joined=output.psir").is_file() and not Path("joined").exists()
    run(psx, "plain.psxp", *runtime, "-ojoined=results")
    assert values("joined=results") == values("plain.results")
    unknown = run(psc, "-", "--print-pipeline", "-x", "ir", "-Ooptimize=junk",
                  "-o", "unused.psxp", code=2)
    assert b"unknown optimizer" in unknown.stderr
    run(psc, "input.json", *common, "--emit", "ir", "--backend", "cpu", code=2)
    run(psc, "input.json", *common, "--emit", "ir:0", "--save-ir", "1=bad.psir", code=2)
    run(psc, "input.json", *common, "--release-ir-after-save", 0, code=2)
    run(psc, "input.json", *common, "-o", "input.json", code=2)
    os.link("input.json", "alias.json")
    run(psc, "input.json", *common, "-o", "alias.json", code=2)
    run(psc, "input.json", "--print-pipeline", "--load-ops", "missing-library", "-O", "optimize")
    Path("commented.psir").write_bytes(b"; leading comment\n" + Path("raw.psir").read_bytes())
    run(psc, "commented.psir", *common, "--check")
    run(psc, "commented.psir", "--print-pipeline")
    write("tag-only.json", {"format": "photospider.dag", "version": 1, "nodes": "unchecked"})
    run(psc, "tag-only.json", "--print-pipeline", "--load-ops", "missing-library")
    write("wrong-tag.json", {"format": "unrelated", "version": 1})
    run(psc, "wrong-tag.json", "--print-pipeline", code=1)

    run(psc, "input.json", "--print-pipeline", "--tile-extent", 3, code=2)
    run(psx, "plain.psxp", *runtime, "--check", "--parallelism", "invalid", code=2)
    run(psc, "input.json", *common, "--emit", "ir:0", "-O", "optimize", "-Werror", code=1)
    run(psc, "input.json", *common, "--emit", "ir:0", "-O", "optimize",
        "-Werror=unused-optimizer", code=1)
    quiet = run(psc, "input.json", *common, "--emit", "ir:0", "-O", "optimize",
                "-Werror", "-Wno-unused-optimizer", "-o", "quiet.psir")
    assert b"PS-W-unused-optimizer" not in quiet.stderr
    verbose = run(psc, "input.json", *common, "--emit", "ir", "-O", "optimize",
                  "--save-ir", "0=verbose-input.psir", "--release-ir-after-save", "0",
                  "-v", "--diagnostic-format=json", "-o", "verbose.psir")
    event_ids = {json.loads(line)["id"] for line in verbose.stderr.splitlines()}
    assert {"PS-STAGE", "PS-IR-RELEASE"} <= event_ids

    result = run(psc, "input.json", *common, "--emit", "ir", "-o", "-", "--time-report", "--diagnostic-format", "json")
    assert result.stdout.startswith(b"psir 5")
    assert all(json.loads(line)["version"] == 1 for line in result.stderr.splitlines())
    run(psc, "-", *common, "--emit", "ir", code=2, data=json.dumps(dag).encode())
    run(psc, "-", *common, "--emit", "ir", "-o", "stdin.psir", data=json.dumps(dag).encode())
    Path("old.psir").write_text(Path("raw.psir").read_text().replace("psir 5", "psir 3", 1))
    assert b"re-export" in run(psc, "old.psir", *common, code=1).stderr
    for text, reason in [('{"format":1,"format":2}', b'duplicate JSON key'),
                         ('{"x":"\\ud800"}', b'JSON 1:'),
                         ('{"x":18446744073709551617}', b'numeric range'),
                         ('{"x":-9223372036854775809}', b'numeric range'),
                         ('{"x":' + '[' * 140 + '0' + ']' * 140 + '}', b'limit')]:
        result = run(psc, "-", *common, "--check", "--diagnostic-format", "json", code=1, data=text.encode())
        assert json.loads(result.stderr)["severity"] == "error"
        assert reason in result.stderr
    if os.name != "nt":
        result = run(psc, b"--invalid-\xff", "--diagnostic-format=json", code=2)
        assert json.loads(result.stderr)["severity"] == "error"
        process = subprocess.Popen([psc, "-", "--check", "--time-report", "--diagnostic-format=json"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # Contract timing is a deterministic signal that handlers are installed.
        assert json.loads(process.stderr.readline())["id"] == "PS-TIME"
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=5) == 130
        process.stdin.close(); process.stdout.close(); process.stderr.close()
        checks += 1
        # A blocked named-file open must honor SIGINT as well as stdin polling.
        os.mkfifo("blocked.fifo")
        # Repeat immediate cancellation across the check-to-open race; also
        # hold a connected FIFO empty to exercise cancellation during reads.
        for connected in [False] * 32 + [True]:
            writer = os.open("blocked.fifo", os.O_RDWR | os.O_NONBLOCK) if connected else None
            process = subprocess.Popen([psc, "blocked.fifo", "--check", "--time-report",
                                        "--diagnostic-format=json"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                assert json.loads(process.stderr.readline())["id"] == "PS-TIME"
                if connected:
                    time.sleep(0.03)
                process.send_signal(signal.SIGINT)
                _, errors = process.communicate(timeout=5)
                assert process.returncode == 130, errors
                assert json.loads(errors)["id"] == "PS-Cancelled"
                checks += 1
            finally:
                if writer is not None:
                    os.close(writer)
                if process.poll() is None:
                    process.kill()
                    process.wait()
                process.stdout.close(); process.stderr.close()
        # A reader opened before its writer must still consume bytes and EOF.
        for data, expected_code in [(json.dumps(dag).encode(), 0), (b"", 1)]:
            process = subprocess.Popen([psc, "blocked.fifo", *common, "--check",
                                        "--time-report", "--diagnostic-format=json"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            writer = None
            try:
                assert json.loads(process.stderr.readline())["id"] == "PS-TIME"
                deadline = time.monotonic() + 5
                while writer is None:
                    try:
                        writer = os.open("blocked.fifo", os.O_WRONLY | os.O_NONBLOCK)
                    except OSError as error:
                        assert error.errno == errno.ENXIO, error
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(0.01)
                assert os.write(writer, data) == len(data)
                os.close(writer)
                writer = None
                _, errors = process.communicate(timeout=5)
                assert process.returncode == expected_code, errors
                checks += 1
            finally:
                if writer is not None:
                    os.close(writer)
                if process.poll() is None:
                    process.kill()
                    process.wait()
                process.stdout.close(); process.stderr.close()
    imprecise = copy.deepcopy(dag); imprecise["nodes"][0]["parameters"]["sha256"] = {"type": "f64", "value": 9007199254740993}
    write("imprecise.json", imprecise)
    assert b'cannot be represented exactly' in run(psc, "imprecise.json", *common, "--check", code=1).stderr
    oversized = copy.deepcopy(resources); oversized["resources"][0].update(path="absent-resource", byte_length=67108865)
    write("oversized.json", oversized)
    assert b'resource size' in run(psc, "input.json", "--load-ops", plugin, "--resources", "oversized.json", "--check", code=1).stderr
    assert b'input size limit' in run(psc, "-", *common, "--check", code=1, data=b' ' * 67108865).stderr
    missing = copy.deepcopy(bindings); missing["inputs"][0]["tensors"][0]["path"] = "absent.bin"
    write("missing-bindings.json", missing)
    result = run(psx, "plain.psxp", *common, "--bindings", "missing-bindings.json", "--check", "--max-live-bytes", 1, code=1)
    assert b'ResourceExhausted' in result.stderr
    requests["outputs"][0]["members"][0]["intervals"] = [[0, 1]] * 65537
    write("many-requests.json", requests)
    assert b'interval limit' in run(psx, "plain.psxp", *runtime, "--requests", "many-requests.json", code=1).stderr
    requests["outputs"][0]["members"][0]["intervals"] = [[0, 1]] * 65536
    write("many-requests.json", requests)
    run(psx, "plain.psxp", *runtime, "--requests", "many-requests.json", "-o", "many.results")
    errors = run(psc, "input.json", *common, "--save-ir", "0=missing-a/raw.psir",
                 "--save-ir", "0=missing-b/raw.psir", "--max-tasks", 1,
                 "-o", "unpublished.psxp", "--diagnostic-format=json", code=4)
    events = [json.loads(line) for line in errors.stderr.splitlines()]
    assert len([event for event in events if event["severity"] == "error"]) == 3
    assert {event["stage"] for event in events} == {"snapshot", "planner"}
    errors = run(psc, "input.json", *common, "--save-ir", "0=missing-a/raw.psir",
                 "--save-ir", "0=missing-b/raw.psir", "--max-tasks", 1,
                 "-o", "unpublished.psxp", "--error-limit", 1,
                 "--diagnostic-format=json", code=4)
    assert len(errors.stderr.splitlines()) == 1
    for _ in range(3):
        run(psc, "input.json", *common, "-O", "optimize", "--save-ir", "0=missing/snapshot.psir", "--max-tasks", 1, "-o", "absent.psxp", code=4)
        assert not Path("absent.psxp").exists()
    bad = copy.deepcopy(bindings); bad["inputs"][0]["tensors"][0]["dtype"] = "i64"
    write("bad-bindings.json", bad)
    run(psx, "plain.psxp", *common, "--bindings", "bad-bindings.json", code=1)
    Path("input.bin").write_bytes(b"short")
    run(psx, "plain.psxp", *runtime, code=1)
    Path("scale.bin").write_bytes(b"\x04")
    run(psc, "input.json", *common, code=1)
    Path("scale.bin").write_bytes(b"\x03")
    Path("main.psxp").write_text("preserve")
    run(psc, "input.json", *common, "--save-ir", "0=missing/snapshot.psir", "-o", "main.psxp", code=4)
    assert Path("main.psxp").read_text() == "preserve"
    kept = run(psc, "input.json", *common, "--save-ir", "0=saved.psir",
               "--max-tasks", 1, "-o", "main.psxp", "--diagnostic-format=json", code=1)
    assert any(json.loads(line)["id"] == "PS-SNAPSHOT-SAVED" for line in kept.stderr.splitlines())
    assert Path("saved.psir").exists() and Path("main.psxp").read_text() == "preserve"
    kept = run(psc, "input.json", *common, "--save-ir", "0=kept-on-io.psir",
               "-o", "absent-parent/main.psxp", "--diagnostic-format=json", code=4)
    events = [json.loads(line) for line in kept.stderr.splitlines()]
    assert any(event["id"] == "PS-SNAPSHOT-SAVED" for event in events)
    assert any(event["stage"] == "artifact" and event["severity"] == "error" for event in events)
    assert Path("kept-on-io.psir").exists() and not Path("absent-parent").exists()
    with open(os.devnull, "rb") as readonly:
        for args in [["--help"], ["--version"], ["--list-optimizers"], ["--help-optimizer", "optimize"], ["--list-planners"], ["--help-planner", "static-tiles"], ["--list-operations"], ["input.json", "--print-pipeline"]]:
            result = subprocess.run([psc, *args, "--diagnostic-format=json"], stdout=readonly, stderr=subprocess.PIPE, timeout=30)
            assert result.returncode == 4 and json.loads(result.stderr)["id"] == "PS-IO-WRITE"
            checks += 1
    operations = json.loads(run(psc, "--list-operations").stdout)
    assert bool(operations) == (builtins == "ON")
    assert not list(Path().glob(".psc-*")) and not list(Path().glob(".psx-*"))
print(f"{checks} CLI process checks passed")
