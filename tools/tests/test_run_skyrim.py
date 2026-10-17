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


class ConfigurationRestore(unittest.TestCase):
    def test_failure_before_launch_restores_game_configuration(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ini, prefs, logs = root / 'WoWBridge.ini', root / 'SkyrimPrefs.ini', root / 'SKSE'
            ini.write_text('[Test]\nautorun=0\n')
            prefs.write_text('[General]\nbAlwaysActive=0\n')
            logs.mkdir()
            with patch.object(runner, 'INI', ini), patch.object(runner, 'PREFS', prefs), \
                    patch.object(runner, 'SKSE_LOGS', logs), patch.object(runner, 'OUT', root / 'runs'), \
                    patch.object(runner, 'game_running', return_value=False), patch.object(runner, 'Injector'), \
                    patch.object(runner, 'stop_game') as stop, \
                    patch.object(runner, 'start_service', side_effect=OSError('port check failed')), \
                    patch.object(sys, 'argv', ['run_skyrim.py', '--autorun']):
                with self.assertRaises(OSError):
                    runner.main()
            self.assertEqual(ini.read_text(), '[Test]\nautorun=0\n')
            self.assertEqual(prefs.read_text(), '[General]\nbAlwaysActive=0\n')
            stop.assert_called_once_with()


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


class RequiredSuite(unittest.TestCase):
    def options(self, **overrides):
        values = dict(rerun_failed=None, diagnostic=False, keep=False, snap=[], autorun=False,
                      only='', timeout=240, until=r'selftest: suite (passed|failed)')
        values.update(overrides)
        return types.SimpleNamespace(**values)

    def execute(self, options=None, failures=(), missing=(), raises=(), observe=lambda name: None):
        calls = []
        with tempfile.TemporaryDirectory() as td:
            def fake(a, directory):
                calls.append((directory.name, a.only, a.keep))
                observe(directory.name)
                directory.mkdir()
                if directory.name in raises:
                    raise OSError('launch failed')
                names = a.only.split(',') if a.only else ['ordinary_probe']
                rows = [{'test': n, 'pass': directory.name not in failures, 't': i + 1, 'frames': (i + 1) * 20}
                        for i, n in enumerate(names) if n not in missing]
                (directory / 'WoWBridge.log').write_text('selftest: suite passed')
                (directory / 'WoWBridge_test.jsonl').write_text('\n'.join(map(json.dumps, rows)))
                issues = runner.suite_failures(directory, a.only)
                return dict(status='failed' if issues else 'passed', issues=issues,
                            result='marker', run_dir=str(directory), elapsed=2)
            with patch.object(runner, 'OUT', Path(td)), patch.object(runner, 'launch', side_effect=fake), \
                    patch('builtins.print'):
                code = runner.run_suite(options or self.options())
            suite = next(Path(td).glob('suite-*'))
            self.assertTrue((suite / 'summary.txt').exists())
            return code, calls, json.loads((suite / 'summary.json').read_text())

    def test_order_and_diagnostic_is_non_gating(self):
        code, calls, summary = self.execute(self.options(diagnostic=True, keep=True), failures=['diagnostic'])
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in calls], [s[0] for s in runner.REQUIRED_SUITE])
        self.assertEqual([c[2] for c in calls], [False] * (len(runner.REQUIRED_SUITE) - 1) + [True])
        self.assertEqual(summary['verdict'], 'passed')
        self.assertFalse(summary['launches'][-1]['gating'])
        self.assertEqual(len(summary['slowest_steps']), 10)
        self.assertTrue(summary['under_30_fps'])
        self.assertTrue(all(step['fps'] == 20 for step in summary['under_30_fps']))

    def test_failure_and_exception_continue(self):
        for kwargs in (dict(failures=['ordinary']), dict(raises=['ordinary']),
                       dict(missing=['wow_prowl_miss'])):
            with self.subTest(kwargs=kwargs):
                code, calls, summary = self.execute(**kwargs)
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), len(runner.REQUIRED_SUITE) - 1)
                self.assertEqual(summary['verdict'], 'failed')

    def test_seed_failure_skips_verify_and_continues(self):
        code, calls, summary = self.execute(failures=['cold-seed'])
        self.assertEqual(code, 1)
        names = [s[0] for s in runner.REQUIRED_SUITE[:-1]]
        self.assertEqual([c[0] for c in calls], [n for n in names if n != 'cold-verify'])
        self.assertEqual(summary['launches'][names.index('cold-verify')]['status'], 'not_run')

    def test_rerun_selects_failed_steps_talents_missing_and_cold_pair(self):
        source = {'launches': [
            dict(name='ordinary', status='failed', only='', steps=[dict(test='bad', **{'pass': False}), dict(test='good', **{'pass': True})], failed_talents=[dict(step='tree')]),
            dict(name='explicit-world', status='failed', only='wow_start_dialog', steps=[]),
            dict(name='cold-verify', status='failed', only='wow_cold_persistence_verify', steps=[]),
            dict(name='diagnostic', status='failed', steps=[]),
        ]}
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / 'summary.json').write_text(json.dumps(source))
            code, calls, summary = self.execute(self.options(rerun_failed=td))
        self.assertEqual(code, 0)
        self.assertEqual([(c[0], c[1]) for c in calls], [
            ('ordinary', 'bad,tree'), ('explicit-world', 'wow_start_dialog'),
            ('cold-seed', dict((n, o) for n, o, _ in runner.REQUIRED_SUITE)['cold-seed']), ('cold-verify', 'wow_cold_persistence_verify')])
        self.assertTrue(summary['triage'])
        self.assertEqual(summary['verdict'], 'triage_passed')

    def test_compass_manifest_withheld_only_for_its_launch(self):
        for raises in ([], ['compass-fallback']):
            with self.subTest(raises=raises), tempfile.TemporaryDirectory() as game:
                manifest = Path(game) / 'compass.json'
                manifest.write_text('{}')
                seen = {}
                with patch.object(runner, 'WITHHELD', {'compass-fallback': manifest}):
                    self.execute(raises=raises, observe=lambda name: seen.setdefault(name, manifest.exists()))
                self.assertFalse(seen['compass-fallback'])
                self.assertTrue(all(present for name, present in seen.items() if name != 'compass-fallback'))
                self.assertTrue(manifest.exists())

    def test_timing_frames_and_talent_details(self):
        rows = [dict(test='first', **{'pass': True}, t=2, frames=120),
                dict(test='slow', **{'pass': False}, t=5, frames=180, error='x' * 400, after_unrestored=['first']),
                dict(event='talent_result', step='tree', talent='name', result={'pass': False, 'error': 'failure'}),
                dict(test='old', **{'pass': True}, t=7),
                dict(test='mixed', **{'pass': True}, t=9, frames=250)]
        with tempfile.TemporaryDirectory() as td:
            (Path(td) / 'WoWBridge_test.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            steps, talents = runner.step_evidence(Path(td))
        self.assertEqual([s['duration'] for s in steps], [2, 3, 2, 2])
        self.assertEqual([s['fps'] for s in steps], [None, 20, None, None])
        self.assertEqual(len(steps[1]['error']), 300)
        self.assertEqual(steps[1]['after_unrestored'], ['first'])
        self.assertEqual(talents[0]['error'], 'failure')

    def test_configuration_restores_each_launch_on_failure_and_sigterm(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ini, prefs, logs = root / 'ini', root / 'prefs', root / 'logs'
            ini.write_text('[Test]\nautorun=0\n')
            prefs.write_text('[General]\nbAlwaysActive=0\n')
            logs.mkdir()
            with patch.object(runner, 'INI', ini), patch.object(runner, 'PREFS', prefs), \
                    patch.object(runner, 'SKSE_LOGS', logs), patch.object(runner, 'game_running', return_value=False), \
                    patch.object(runner, 'Injector'), patch.object(runner, 'stop_game') as stop, \
                    patch.object(runner.time, 'sleep'), patch.object(runner.signal, 'signal') as handler:
                def fail():
                    self.assertIn('autorun=1', ini.read_text())
                    self.assertIn('bAlwaysActive=1', prefs.read_text())
                    raise OSError('startup failed')
                with patch.object(runner, 'start_service', side_effect=fail):
                    for i in range(2):
                        with self.assertRaises(OSError):
                            runner.launch(self.options(autorun=True), root / str(i))
                        self.assertEqual(ini.read_text(), '[Test]\nautorun=0\n')
                        self.assertEqual(prefs.read_text(), '[General]\nbAlwaysActive=0\n')
                def terminate():
                    handler.call_args_list[-1].args[1]()
                with patch.object(runner, 'start_service', side_effect=terminate):
                    with self.assertRaises(SystemExit) as exc:
                        runner.launch(self.options(autorun=True), root / 'term')
                self.assertEqual(exc.exception.code, 143)
                self.assertEqual(ini.read_text(), '[Test]\nautorun=0\n')
                self.assertEqual(prefs.read_text(), '[General]\nbAlwaysActive=0\n')
                self.assertEqual(stop.call_count, 3)
