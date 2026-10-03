"""Companion labels retain identity and never change inventory or assign slots."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from custom_components.quack_material_demo.store import Store


class LabelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.store = Store(Path(self.tmp.name) / 'test.sqlite3', settings={
            'base_url': 'http://homeassistant.local:8123'})
        self.roll = self.store.snapshot()['spools'][0]['uuid']

    def durable_state(self):
        with self.store.connection() as db:
            return list(db.iterdump())

    def test_app_and_browser_qr_encode_the_displayed_payload_without_writes(self):
        before = self.durable_state()
        for target, prefix in [('app', 'homeassistant://navigate'),
                               ('web', 'http://homeassistant.local:8123')]:
            expected = prefix + '/dashboard-filament/rolls?spool=' + self.roll
            with patch('qrcode.make') as make:
                result = self.store.dispatch('label', {'spool_uuid': self.roll, 'target': target})
                self.assertEqual(make.call_args.args[0], expected)
            self.assertEqual(result['payload'], expected)
            self.assertEqual(result['target'], target)
            self.assertEqual(result['native_payload'], 'quackslicer://spool/' + self.roll)
        self.assertEqual(self.durable_state(), before)

    def test_legacy_label_request_keeps_web_payload(self):
        label = self.store.dispatch('label', {'spool_uuid': self.roll})
        self.assertTrue(label['payload'].startswith('http://homeassistant.local:8123/'))

    def test_unknown_target_is_rejected_without_writes(self):
        before = self.durable_state()
        with self.assertRaises(ValueError):
            self.store.dispatch('label', {'spool_uuid': self.roll, 'target': 'call_service'})
        self.assertEqual(self.durable_state(), before)
