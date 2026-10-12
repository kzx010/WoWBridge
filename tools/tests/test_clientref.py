"""Reject the concrete mislabeled/empty reference states identified by the fidelity audit."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apiref import load
from clientref import validate
import clientref
from unittest.mock import patch


class CaptureValidation(unittest.TestCase):
    def test_clean_spellbook_saved_variables_roundtrip(self):
        text = '''WBOracleAPI = {
\t["capture_book_scale080"] = {
\t\t["kind"] = "spellbook",
\t\t["UIParent:IsVisible()"] = "1",
\t\t["visible:SpellBookFrame"] = "true",
\t\t["visible:GameMenuFrame"] = "false",
\t},
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'state.lua'
            path.write_text(text)
            data = load(path)['capture_book_scale080']
            self.assertEqual(validate(data, 'SpellBookFrame', 'spellbook'), [])

    def test_menu_mislabeled_as_book_is_rejected(self):
        data = {'UIParent:IsVisible()': '1', 'kind': 'spellbook', 'visible:SpellBookFrame': 'false', 'visible:GameMenuFrame': 'true'}
        self.assertEqual(len(validate(data, 'SpellBookFrame', 'spellbook')), 2)

    def test_idle_with_open_book_is_rejected(self):
        self.assertTrue(validate({'kind': 'idle', 'visible:SpellBookFrame': 'true'}, 'none', 'idle'))

    def test_empty_taxi_layout_is_not_route_evidence(self):
        data = {'UIParent:IsVisible()': '1', 'kind': 'taxi', 'visible:TaxiFrame': 'true', 'NumTaxiNodes()': '0'}
        self.assertTrue(validate(data, 'TaxiFrame', 'taxi'))
        data['NumTaxiNodes()'] = '4'
        self.assertEqual(validate(data, 'TaxiFrame', 'taxi'), [])

    def test_script_error_and_missing_recorder_are_rejected(self):
        self.assertTrue(validate({'kind': 'idle', 'visible:ScriptErrors': 'true'}, 'none', 'idle'))
        self.assertTrue(validate({}, 'none', 'idle'))
        self.assertTrue(validate({'error': 'fixture error'}, 'MerchantFrame', 'merchant'))

    def test_available_trainer_row_is_not_proof_of_enabled_train_button(self):
        data = {'kind': 'trainer', 'UIParent:IsVisible()': '1', 'visible:ClassTrainerFrame': 'true',
                'GetNumTrainerServices()': '183', 'ClassTrainerTrainButton:IsEnabled()': '0'}
        wanted = {'ClassTrainerTrainButton:IsEnabled()': '1'}
        self.assertTrue(validate(data, 'ClassTrainerFrame', 'trainer', wanted))
        data['ClassTrainerTrainButton:IsEnabled()'] = '1'
        self.assertEqual(validate(data, 'ClassTrainerFrame', 'trainer', wanted), [])

    def test_reset_trainer_filters_invalidate_old_service_index(self):
        data = {'kind': 'trainer', 'UIParent:IsVisible()': '1', 'visible:ClassTrainerFrame': 'true',
                'GetNumTrainerServices()': '27', 'GetTrainerSelectionIndex()': '71'}
        self.assertTrue(validate(data, 'ClassTrainerFrame', 'trainer'))

    def test_filtered_index_must_match_requested_service_category(self):
        data = {'kind': 'trainer', 'UIParent:IsVisible()': '1', 'visible:ClassTrainerFrame': 'true',
                'GetNumTrainerServices()': '27', 'GetTrainerSelectionIndex()': '2',
                'selectedServiceType': 'unavailable'}
        wanted = {'selectedServiceType': 'used'}
        self.assertTrue(validate(data, 'ClassTrainerFrame', 'trainer', wanted))
        data['selectedServiceType'] = 'used'
        self.assertEqual(validate(data, 'ClassTrainerFrame', 'trainer', wanted), [])

    def test_character_requires_the_native_framexml_rotation_field(self):
        data = {'kind': 'character', 'UIParent:IsVisible()': '1', 'visible:CharacterFrame': 'true',
                'CharacterModelFrame:GetRotation()': 'error: unsupported method'}
        self.assertTrue(validate(data, 'CharacterFrame', 'character'))
        data['CharacterModelFrame.rotation'] = '3.1415926535898'
        self.assertEqual(validate(data, 'CharacterFrame', 'character'), [])

    def test_empty_or_missing_buff_is_not_a_buff_reference(self):
        data = {'kind': 'buff', 'UIParent:IsVisible()': '1'}
        self.assertTrue(validate(data, 'none', 'buff'))
        data['GetPlayerBuff(0,"HELPFUL")'] = '-1, 0'
        self.assertTrue(validate(data, 'none', 'buff'))
        data['GetPlayerBuff(0,"HELPFUL")'] = '0, 0'
        self.assertEqual(validate(data, 'none', 'buff'), [])

    def test_quantity_dialog_requires_explicit_merchant_expectation(self):
        data = {'kind': 'merchant', 'UIParent:IsVisible()': '1', 'visible:MerchantFrame': 'true',
                'GetMerchantNumItems()': '36', 'visible:StackSplitFrame': 'true',
                'StackSplitFrame.split': '1', 'StackSplitFrame.maxStack': '20',
                'StackSplitFrame.owner:GetName()': '"MerchantItem1ItemButton"'}
        self.assertTrue(validate(data, 'MerchantFrame', 'merchant'))
        expected = {'visible:StackSplitFrame': 'true'}
        self.assertEqual(validate(data, 'MerchantFrame', 'merchant', expected), [])
        for key,value in [('StackSplitFrame.split','21'), ('StackSplitFrame.maxStack','1'),
                          ('StackSplitFrame.owner:GetName()','"ContainerFrame1Item1"')]:
            broken = dict(data); broken[key] = value
            with self.subTest(key=key):
                self.assertTrue(validate(broken, 'MerchantFrame', 'merchant', expected))
        del data['StackSplitFrame.split']
        self.assertTrue(validate(data, 'MerchantFrame', 'merchant', expected))


class PreparedCapture(unittest.TestCase):
    def test_capture_failure_releases_input_before_any_further_chat(self):
        events = []
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(clientref, 'OUT', Path(temporary)), \
             patch.object(clientref, 'signatures', return_value={}), \
             patch.object(clientref.wowref, 'main', side_effect=lambda steps: events.append(('chat', steps))), \
             patch.object(clientref, 'save_native', side_effect=RuntimeError('screenshot missing')):
            with self.assertRaisesRegex(RuntimeError, 'screenshot missing'):
                clientref.record('prepared_failure', 'merchant', 'MerchantFrame',
                    prepare=lambda: events.append(('prepare',)),
                    cleanup=lambda: events.append(('cleanup',)))
        self.assertEqual([event[0] for event in events], ['chat', 'prepare', 'cleanup'])
        self.assertIn(',5)', events[0][1][0])

    def test_unpaired_preparation_is_rejected_before_input(self):
        with patch.object(clientref.wowref, 'main') as main:
            with self.assertRaisesRegex(ValueError, 'both prepare and cleanup'):
                clientref.record('unpaired', 'merchant', 'MerchantFrame', prepare=lambda: None)
        main.assert_not_called()
