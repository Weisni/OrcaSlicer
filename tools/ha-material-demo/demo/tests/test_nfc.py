"""Device isolation and transient scan delivery; no live HA or tag writes."""
import unittest
from copy import deepcopy
try:
    from custom_components.quack_material_demo.nfc import NfcBroker
except ImportError:
    NfcBroker = None

ROLL = '11111111-1111-4111-8111-111111111111'
OTHER = '93f273c8-2ee3-4122-b681-d4cf3f9e2401'
A, B, C = 'a' * 64, 'b' * 64, 'c' * 64


class NfcTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(NfcBroker, 'NFC broker is not implemented')
        self.now = 1000.
        self.broker = NfcBroker(clock=lambda: self.now)
        self.broker.bind('user', A, 'phone-a')
        self.broker.bind('user', B, 'phone-b')

    def scan(self, roll=ROLL, device='phone-a', event='event-1'):
        return self.broker.scan(device, roll, event_id=event, fired_at=self.now)

    def test_same_user_two_phones_receive_only_their_own_scan(self):
        self.scan()
        self.assertEqual(self.broker.poll('user', A)['scan']['spool_uuid'], ROLL)
        self.assertIsNone(self.broker.poll('user', B)['scan'])
        self.assertFalse(self.broker.poll('other-user', A)['paired'])
        self.assertFalse(self.broker.poll('user', C)['paired'])

    def test_late_frontend_can_claim_but_expired_scan_is_gone(self):
        self.scan()
        self.now += 70
        self.assertIsNotNone(self.broker.poll('user', A)['scan'])
        self.now += 21
        self.assertIsNone(self.broker.poll('user', A)['scan'])

    def test_wrong_client_ack_cannot_consume_another_phones_scan(self):
        self.scan()
        ident = self.broker.poll('user', A)['scan']['id']
        self.assertFalse(self.broker.ack('user', B, ident))
        self.assertFalse(self.broker.ack('other-user', A, ident))
        self.assertTrue(self.broker.ack('user', A, ident))
        self.assertIsNone(self.broker.poll('user', A)['scan'])

    def test_ack_of_previous_scan_preserves_newer_scan(self):
        self.scan()
        old = self.broker.poll('user', A)['scan']['id']
        self.now += 3
        self.scan(OTHER, event='event-2')
        self.assertFalse(self.broker.ack('user', A, old))
        self.assertEqual(self.broker.poll('user', A)['scan']['spool_uuid'], OTHER)

    def test_out_of_order_event_cannot_replace_or_reopen_newer_scan(self):
        self.scan(OTHER)
        ident = self.broker.poll('user', A)['scan']['id']
        self.assertFalse(self.broker.scan('phone-a', ROLL, event_id='delayed', fired_at=self.now - 1))
        self.assertEqual(self.broker.poll('user', A)['scan']['id'], ident)
        self.broker.ack('user', A, ident)
        self.now += 3
        self.assertFalse(self.broker.scan('phone-a', ROLL, event_id='delayed-again', fired_at=self.now - 4))
        self.assertIsNone(self.broker.poll('user', A)['scan'])

    def test_duplicate_event_cannot_reopen_after_ack(self):
        self.scan()
        ident = self.broker.poll('user', A)['scan']['id']
        self.broker.ack('user', A, ident)
        self.now += 3
        self.scan()
        self.assertIsNone(self.broker.poll('user', A)['scan'])
        self.scan(event='event-2')
        self.assertIsNotNone(self.broker.poll('user', A)['scan'])

    def test_fast_repeat_scan_is_debounced_but_deliberate_later_scan_works(self):
        self.scan()
        ident = self.broker.poll('user', A)['scan']['id']
        self.broker.ack('user', A, ident)
        self.now += .5
        self.scan(event='event-2')
        self.assertIsNone(self.broker.poll('user', A)['scan'])
        self.now += 3
        self.scan(event='event-3')
        self.assertIsNotNone(self.broker.poll('user', A)['scan'])

    def test_rebinding_revokes_old_browser_and_discards_pre_pairing_scan(self):
        self.scan()
        self.broker.bind('user', C, 'phone-a')
        self.assertFalse(self.broker.poll('user', A)['paired'])
        self.assertTrue(self.broker.poll('user', C)['paired'])
        self.assertIsNone(self.broker.poll('user', C)['scan'])

    def test_restart_preserves_hash_bindings_but_never_pending_scans(self):
        self.scan()
        data = self.broker.dump_bindings()
        self.assertNotIn(A, str(data))
        self.assertNotIn(ROLL, str(data))
        restarted = NfcBroker(bindings=deepcopy(data), clock=lambda: self.now)
        self.assertTrue(restarted.poll('user', A)['paired'])
        self.assertIsNone(restarted.poll('user', A)['scan'])

    def test_invalid_stale_or_unpaired_events_do_not_route(self):
        for ident in ['not-a-uuid', 'https://example.org', None]:
            self.broker.scan('phone-a', ident, event_id=str(ident), fired_at=self.now)
        self.broker.scan('phone-a', ROLL, event_id='stale', fired_at=self.now - 91)
        self.broker.scan('phone-a', ROLL, event_id='future', fired_at=self.now + 10)
        self.broker.scan('unknown-device', ROLL, event_id='unknown', fired_at=self.now)
        self.assertIsNone(self.broker.poll('user', A)['scan'])
        self.assertIsNone(self.broker.poll('user', B)['scan'])

    def test_unbind_revokes_access_and_requires_valid_capability(self):
        self.scan()
        self.assertFalse(self.broker.unbind('other-user', A))
        self.assertTrue(self.broker.unbind('user', A))
        self.assertFalse(self.broker.poll('user', A)['paired'])
        for key in ['', 'short', 'z' * 64]:
            with self.assertRaises(ValueError):
                self.broker.bind('user', key, 'phone-a')
