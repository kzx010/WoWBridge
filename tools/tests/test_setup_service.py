"""Tester service startup keeps full game-state traces optional."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('setup_service_tested', Path(__file__).parents[2] / 'setup.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class TesterService(unittest.TestCase):
    def launch(self, *options):
        probe = Mock()
        probe.__enter__ = Mock(return_value=probe)
        probe.__exit__ = Mock(return_value=False)
        probe.connect_ex.return_value = 1
        with patch.object(sys, 'argv', ['setup.py', 'service', *options]), \
                patch.object(setup, 'check') as check, patch.object(setup.socket, 'socket', return_value=probe), \
                patch.object(setup.subprocess, 'call', return_value=0) as call, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(setup.main(), 0)
        check.assert_called_once_with()
        args, kwargs = call.call_args
        self.assertEqual(kwargs['cwd'], setup.RUNTIME)
        self.assertIn('--resist-table', args[0])
        return args[0]

    def test_normal_start_does_not_enable_request_tracing(self):
        self.assertNotIn('--trace', self.launch())

    def test_requested_trace_stays_in_local_directory(self):
        args = self.launch('--trace')
        self.assertEqual(args[args.index('--trace') + 1], str(setup.LOCAL / 'service_trace.jsonl'))
