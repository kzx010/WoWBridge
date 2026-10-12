"""Exercise runner failure handling without launching games or accessing the desktop."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("test_project_runner", Path(__file__).parents[1] / "test_project.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RegressionRunner(unittest.TestCase):
    def test_child_failure_is_reported_and_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            output = Path(directory)
            result = runner.run_step("failure", [sys.executable, "-c", "print('fixture failure'); raise SystemExit(7)"], output, 10)
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["returncode"], 7)
            self.assertIn("fixture failure", (output / result["log"]).read_text())

    def test_failed_suite_does_not_hide_remaining_suites_or_report_success(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(runner, "ROOT", Path(directory)), \
                contextlib.redirect_stdout(io.StringIO()):
            commands = []
            def run_step(name, command, output, timeout):
                if commands:
                    pending = json.loads(next((output.parent).glob("*/summary.json")).read_text())
                    self.assertFalse(pending["passed"])
                    self.assertFalse(pending["completed"])
                commands.append(name)
                return {"name": name, "status": "failed" if name == "service" else "passed"}
            with patch.object(runner, "run_step", side_effect=run_step):
                code = runner.main(["--suite", "service", "--suite", "tools"])
            self.assertEqual(code, 1)
            self.assertEqual(commands, ["service", "tools"])
            report = json.loads(next((Path(directory) / "build/tests").glob("*/summary.json")).read_text())
            self.assertTrue(report["completed"])
            self.assertFalse(report["passed"])
            self.assertFalse(report["native_gameplay_tested"])
            self.assertEqual(len(report["results"]), 2)
