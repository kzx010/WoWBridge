#!/usr/bin/env python3
"""Run non-interactive regressions and keep logs under ignored build/tests/."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SUITES = ("portable", "service", "tools", "data")


def run_step(name, command, output, timeout):
    started = time.monotonic()
    log_path = output / f"{name}.log"
    result = {"name": name, "command": command, "log": log_path.name}
    with log_path.open("w", encoding="utf-8") as log:
        try:
            process = subprocess.run(command, cwd=ROOT, stdout=log,
                                     stderr=subprocess.STDOUT, timeout=timeout)
            result.update(status="passed" if process.returncode == 0 else "failed",
                          returncode=process.returncode)
        except subprocess.TimeoutExpired:
            result.update(status="timeout", returncode=None)
            log.write(f"\nStopped after {timeout} seconds.\n")
        except OSError as error:
            result.update(status="error", returncode=None)
            log.write(f"\nCould not run command: {error}\n")
    result["seconds"] = round(time.monotonic() - started, 2)
    print(f"{name}: {result['status']} ({result['seconds']}s)", flush=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", action="append", choices=SUITES,
                        help="Select one or more suites; defaults to all four.")
    parser.add_argument("--build", action="store_true",
                        help="Rebuild the configured portable test target first; does not deploy.")
    parser.add_argument("--portable-filter", default="~[skyrim-icons]",
                        help="Catch2 filter; excludes the separately generated Skyrim icon assets by default.")
    parser.add_argument("--timeout", type=float, default=600,
                        help="Maximum seconds per command (default: 600).")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args(argv)
    suites = list(dict.fromkeys(args.suite or SUITES))
    if args.timeout <= 0 or args.jobs <= 0:
        parser.error("--timeout and --jobs must be positive")
    if args.build and "portable" not in suites:
        parser.error("--build requires the portable suite")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    output = ROOT / "build" / "tests" / f"{stamp}-{os.getpid()}"
    output.mkdir(parents=True, exist_ok=False)
    print(f"Logs: {output}", flush=True)
    report = {"completed": False, "passed": False, "native_gameplay_tested": False, "results": []}

    def record(name, command):
        result = run_step(name, command, output, args.timeout)
        report["results"].append(result)
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return result["status"] == "passed"

    build_ok = True
    if args.build:
        build_ok = record("portable-build", ["cmake", "--build", "build/wowcore",
                          "--target", "wowcore_tests", "-j", str(args.jobs)])
    for suite in suites:
        if suite == "portable":
            if build_ok:
                record(suite, [str(ROOT / "build/wowcore/wowcore_tests"), args.portable_filter])
        else:
            record(suite, [sys.executable, "-m", "unittest", "discover", "-s", f"{suite}/tests"])
    report["completed"] = True
    report["passed"] = all(r["status"] == "passed" for r in report["results"])
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
