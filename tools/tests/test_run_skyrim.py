"""Service-action ownership checks; no processes, sockets or desktop input."""
import importlib.util
import json
import tempfile
import os
import shlex
import subprocess
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch


spec = importlib.util.spec_from_file_location('run_skyrim_tested', Path(__file__).parents[1] / 'run_skyrim.py')
runner = importlib.util.module_from_spec(spec)
# Loading the runner must not require the desktop injection dependency for these tests.
with patch.dict(sys.modules, {'inputdev': types.SimpleNamespace(focus_window=Mock(), kwin=Mock())}):
    spec.loader.exec_module(runner)


class ServiceActions(unittest.TestCase):
    def test_shared_service_stop_fails_without_success_or_focus(self):
        log = []
        with patch.object(runner, 'SERVICE', None), patch.object(runner, 'focus_window') as focus:
            runner.perform('service_stop', log)
        focus.assert_not_called()
        self.assertIn('FAILED', log[0])
        self.assertIn('runner-owned service', log[0])
        self.assertNotIn(' ok', log[0])

    def test_owned_stop_waits_before_success(self):
        service = Mock()
        service.poll.return_value = None
        log = []
        with patch.object(runner, 'SERVICE', service), patch.object(runner, 'focus_window') as focus:
            runner.perform('service_stop', log)
        service.terminate.assert_called_once_with()
        service.wait.assert_called_once_with(10)
        focus.assert_not_called()
        self.assertIn('ok (runner-owned service)', log[0])

    def test_dead_service_is_not_an_offline_transition(self):
        service = Mock()
        service.poll.return_value = 1
        with patch.object(runner, 'SERVICE', service):
            with self.assertRaisesRegex(RuntimeError, 'exited before'):
                runner.act_service_stop()
        service.terminate.assert_not_called()

    def test_restart_requires_owned_stopped_service(self):
        for service in (None, Mock(poll=Mock(return_value=None))):
            with self.subTest(service=service), patch.object(runner, 'SERVICE', service), \
                    patch.object(runner, 'start_service') as start:
                with self.assertRaisesRegex(RuntimeError, 'have stopped'):
                    runner.act_service_start()
                start.assert_not_called()

    def test_restart_rejects_port_taken_and_early_exit(self):
        old = Mock(poll=Mock(return_value=0))
        for new, message in ((None, 'port 7878'), (Mock(poll=Mock(return_value=1)), 'exited before')):
            with self.subTest(message=message), patch.object(runner, 'SERVICE', old), \
                    patch.object(runner, 'start_service', return_value=new), patch.object(runner.time, 'sleep'):
                with self.assertRaisesRegex(RuntimeError, message):
                    runner.act_service_start()

    def test_restart_retains_new_owned_process(self):
        old = Mock(poll=Mock(return_value=0))
        new = Mock(poll=Mock(return_value=None))
        with patch.object(runner, 'SERVICE', old), patch.object(runner, 'start_service', return_value=new), \
                patch.object(runner.time, 'sleep'):
            runner.act_service_start()
            self.assertIs(runner.SERVICE, new)


if __name__ == '__main__':
    unittest.main()

class CtrlMovement(unittest.TestCase):
    def test_ctrl_precedes_movement_and_always_releases(self):
        injection = Mock()
        injection.e.KEY_LEFTCTRL = 29
        injection.e.KEY_W = 17
        injection.e.EV_KEY = 1
        for fail in (False, True):
            calls = []
            injection.ui.write.side_effect = lambda *args: calls.append(args)
            def key(*args):
                calls.append(('move', *args))
                if fail:
                    raise RuntimeError('input unavailable')
            injection.key.side_effect = key
            with patch.object(runner, 'INJ', injection), patch.object(runner.time, 'sleep'):
                if fail:
                    with self.assertRaisesRegex(RuntimeError, 'input unavailable'):
                        runner.act_ctrl_movement('w')
                else:
                    runner.act_ctrl_movement('w')
            self.assertEqual(calls, [(1, 29, 1), ('move', 17, .5), (1, 29, 0)])


class SuiteResults(unittest.TestCase):
    def check(self, rows, only='', marker='passed'):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'WoWBridge.log').write_text('selftest: suite ' + marker)
            (root / 'WoWBridge_test.jsonl').write_text('\n'.join(json.dumps(row) for row in rows))
            return runner.suite_failures(root, only)

    def test_success_requires_requested_probe(self):
        self.assertEqual(self.check([{'test': 'wow_mounts', 'pass': True}], 'wow_mounts'), [])
        issues = self.check([{'test': 'setup', 'pass': True}], 'wow_mounts')
        self.assertIn('requested self-tests did not run: wow_mounts', issues)

    def test_failed_probe_is_not_a_successful_end_marker(self):
        for value in (False, None, 'true', 1):
            with self.subTest(value=value):
                issues = self.check([{'test': 'wow_mounts', 'pass': value}])
                self.assertIn('failed self-test: wow_mounts', issues)
        self.assertTrue(self.check([{'test': 'wow_mounts', 'pass': True}], marker='failed (1 failed)'))

    def test_unknown_name_cannot_hide_among_valid_names(self):
        issues = self.check([{'test': 'known', 'pass': True}], 'known, unknown')
        self.assertIn('requested self-tests did not run: unknown', issues)

    def test_empty_missing_or_malformed_evidence_fails(self):
        self.assertTrue(self.check([]))
        self.assertTrue(self.check([[]]))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertTrue(runner.suite_failures(root))
            (root / 'WoWBridge_test.jsonl').write_text('{broken')
            self.assertTrue(runner.suite_failures(root))

    def test_talent_evidence_requires_success_and_final_step(self):
        talent = {'event': 'talent_result', 'step': 'tree', 'talent': 'Improved Wrath', 'result': {'pass': True}}
        final = {'test': 'tree', 'pass': True}
        self.assertEqual(self.check([talent, final], 'tree'), [])
        self.assertIn('requested self-tests did not run: tree', self.check([talent], 'tree'))
        talent['result']['pass'] = False
        self.assertIn('failed talent check: tree / Improved Wrath', self.check([talent, final], 'tree'))


class PlayWrapper(unittest.TestCase):
    def run_wrapper(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'tools').mkdir()
            (root / 'build').mkdir()
            (root / 'bin').mkdir()
            source = Path(__file__).parents[1]
            shell = (source / 'play.sh').read_text()
            # The wrapper accepts an interpreter override without editing source.
            (root / 'tools/play.sh').write_text(shell)
            (root / 'tools/inputdev.py').write_text('focus_window = lambda *a: False\nkwin = lambda *a: []\n')
            (root / 'bin/pgrep').write_text('#!/bin/sh\nexit 1\n')
            (root / 'bin/pgrep').chmod(0o755)
            fake = """import json, os, runpy, sys
from pathlib import Path
if __name__ != '__main__':
    suite_failures = runpy.run_path(REAL_RUNNER)['suite_failures']
else:
    mode = os.environ['FAKE_RUN_MODE']
    if mode == 'no_evidence':
        print('launch failed')
        sys.exit(3)
    root = Path('build/runs/fixture').resolve()
    root.mkdir(parents=True)
    passed = mode != 'failed'
    (root/'WoWBridge.log').write_text('selftest: suite ' + ('passed' if passed else 'failed (1 failed)'))
    rows = [{'event':'talent_result','step':'probe','talent':'talent','result':{'pass':True}}, {'test':'probe','pass':passed}]
    (root/'WoWBridge_test.jsonl').write_text('\\n'.join(json.dumps(row) for row in rows))
    print('result=marker elapsed=0s run_dir=' + str(root))
    sys.exit(1 if mode in ('failed', 'runner_failed') else 0)
""".replace('REAL_RUNNER', repr(str(source / 'run_skyrim.py')))
            (root / 'tools/run_skyrim.py').write_text(fake)
            env = dict(os.environ, WOWBRIDGE_PYTHON=sys.executable, FAKE_RUN_MODE=mode, PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'])
            return subprocess.run(['bash', 'tools/play.sh', 'test', '--only', 'probe'], cwd=root, env=env, text=True, capture_output=True)

    def test_supplemental_talents_do_not_become_failed_steps(self):
        result = self.run_wrapper('passed')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('1/1 steps passed', result.stdout)
        self.assertIn('1/1 talent checks passed', result.stdout)
        self.assertNotIn('FAIL', result.stdout)

    def test_suite_and_runner_failures_propagate(self):
        for mode in ('failed', 'runner_failed', 'no_evidence'):
            with self.subTest(mode=mode):
                result = self.run_wrapper(mode)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
