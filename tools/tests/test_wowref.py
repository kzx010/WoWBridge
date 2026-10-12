"""Reference-client input must keep focus checks and release held modifiers."""
import os
import sys
from pathlib import Path
import unittest
from unittest.mock import MagicMock, call, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import wowref


class ReferenceInput(unittest.TestCase):
    def test_failed_shift_click_still_releases_shift(self):
        inp = MagicMock()
        inp.click.side_effect = RuntimeError('pointer disconnected')
        with patch.object(wowref, 'Input', return_value=inp), \
             patch.object(wowref, 'frame_origin', return_value=(0, 0, 1)), \
             patch.object(wowref, 'focus', return_value=True):
            with self.assertRaisesRegex(RuntimeError, 'pointer disconnected'):
                wowref.main(['shiftclick:100,200'])
        self.assertEqual(inp.kbd.mock_calls, [
            call.write(wowref.e.EV_KEY, wowref.e.KEY_LEFTSHIFT, 1), call.syn(),
            call.write(wowref.e.EV_KEY, wowref.e.KEY_LEFTSHIFT, 0), call.syn()])

    def test_shift_click_and_wheel_require_reference_client_focus(self):
        for step in ['shiftclick:100,200', 'wheel:1']:
            with self.subTest(step=step):
                inp = MagicMock()
                with patch.object(wowref, 'Input', return_value=inp), \
                     patch.object(wowref, 'frame_origin', return_value=(0, 0, 1)), \
                     patch.object(wowref, 'focus', return_value=False), \
                     patch.object(wowref.time, 'sleep'):
                    with self.assertRaises(SystemExit):
                        wowref.main([step])
                inp.click.assert_not_called()
                inp.wheel.assert_not_called()
                inp.kbd.write.assert_not_called()


class ReferenceCredentials(unittest.TestCase):
    def test_missing_password_stops_before_input_or_launch(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(wowref, 'Input') as inp:
            with self.assertRaisesRegex(SystemExit, 'set WOWBRIDGE_REFERENCE_PASSWORD'):
                wowref.main(['launch', 'login'])
            inp.assert_not_called()

    def test_command_line_password_is_rejected_without_echoing_it(self):
        supplied = 'test-only-argument-value'
        with patch.object(wowref, 'Input') as inp:
            with self.assertRaises(SystemExit) as error:
                wowref.main(['login:' + supplied])
            self.assertNotIn(supplied, str(error.exception))
            inp.assert_not_called()

    def test_environment_password_reaches_only_focused_client(self):
        supplied = 'test-only-environment-value'
        inp = MagicMock()
        with patch.dict(os.environ, WOWBRIDGE_REFERENCE_PASSWORD=supplied), \
             patch.object(wowref, 'Input', return_value=inp), \
             patch.object(wowref, 'frame_origin', return_value=(0, 0, 1)), \
             patch.object(wowref, 'focus', return_value=True), \
             patch.object(wowref.time, 'sleep'):
            wowref.main(['login'])
        inp.type.assert_called_once_with(supplied)

    def test_failed_focus_does_not_echo_typed_content(self):
        supplied = 'test-only-sensitive-input'
        with patch.object(wowref, 'Input'), \
             patch.object(wowref, 'frame_origin', return_value=(0, 0, 1)), \
             patch.object(wowref, 'focus', return_value=False), \
             patch.object(wowref.time, 'sleep'):
            with self.assertRaises(SystemExit) as error:
                wowref.main(['type:' + supplied])
        self.assertNotIn(supplied, str(error.exception))

    def test_reference_recorder_selection_is_explicit_when_ambiguous(self):
        from tempfile import TemporaryDirectory
        with TemporaryDirectory() as directory, patch.dict(os.environ, {}, clear=True):
            root = Path(directory)
            with patch.object(wowref, 'RUN_WOW', root / 'run-wow.sh'):
                with self.assertRaisesRegex(RuntimeError, 'missing or ambiguous'):
                    wowref.saved_variables()
                one = root / 'client/WTF/Account/TEST_ONE/SavedVariables/WBOracle.lua'
                one.parent.mkdir(parents=True)
                one.write_text('fixture')
                self.assertEqual(wowref.saved_variables(), one)
                two = root / 'client/WTF/Account/TEST_TWO/SavedVariables/WBOracle.lua'
                two.parent.mkdir(parents=True)
                two.write_text('fixture')
                with self.assertRaisesRegex(RuntimeError, 'missing or ambiguous'):
                    wowref.saved_variables()
                with patch.dict(os.environ, WOWBRIDGE_REFERENCE_SAVED_VARIABLES=str(two)):
                    self.assertEqual(wowref.saved_variables(), two)
