"""Transient device-bound NFC navigation, independent of inventory mutations."""
from hashlib import sha256
import re
import time
from uuid import UUID, uuid4

TTL = 90


class NfcBroker:
    """Called on HA's event loop; only hashed phone bindings are persisted."""

    def __init__(self, bindings=None, clock=time.time):
        self.clock = clock
        self.bindings = {}
        self.pending = {}
        self.seen = {}
        self.recent = {}
        if isinstance(bindings, list):
            for row in bindings:
                if (isinstance(row, dict) and re.fullmatch(r'[0-9a-f]{64}', str(row.get('key_hash', '')))
                        and all(isinstance(row.get(k), str) and 0 < len(row[k]) <= 128
                                for k in ('user_id', 'device_id'))):
                    self.bindings[row['key_hash']] = {k: row[k] for k in ('user_id', 'device_id')}

    @staticmethod
    def _hash(key):
        if not isinstance(key, str) or not re.fullmatch(r'[0-9a-f]{64}', key):
            raise ValueError('Invalid phone binding')
        return sha256(key.encode()).hexdigest()

    def _binding(self, user, key):
        row = self.bindings.get(self._hash(key))
        return row if row and row['user_id'] == user else None

    def bind(self, user, key, device):
        digest = self._hash(key)
        if not all(isinstance(v, str) and 0 < len(v) <= 128 for v in (user, device)):
            raise ValueError('Invalid phone binding')
        previous = self.bindings.get(digest)
        if previous:
            self.pending.pop(previous['device_id'], None)
        # Exactly one browser capability may receive a device's scans.
        self.bindings = {k: v for k, v in self.bindings.items() if v['device_id'] != device}
        self.bindings[digest] = {'user_id': user, 'device_id': device}
        self.pending.pop(device, None)

    def unbind(self, user, key):
        row = self._binding(user, key)
        if not row:
            return False
        self.pending.pop(row['device_id'], None)
        self.bindings.pop(self._hash(key), None)
        return True

    def dump_bindings(self):
        return [dict(key_hash=key, **value) for key, value in self.bindings.items()]

    def _prune(self):
        now = self.clock()
        self.pending = {k: v for k, v in self.pending.items() if v['expires_at'] > now}
        self.seen = {k: v for k, v in self.seen.items() if v > now}
        self.recent = {k: v for k, v in self.recent.items() if now - v[1] < TTL}

    def scan(self, device, roll, *, event_id, fired_at):
        self._prune()
        now = self.clock()
        if not any(row['device_id'] == device for row in self.bindings.values()):
            return False
        try:
            roll = str(UUID(roll))
            fired_at = float(fired_at)
        except (ValueError, TypeError, AttributeError):
            return False
        if not now - TTL < fired_at <= now + 5:
            return False
        event_key = (device, str(event_id))
        if event_key in self.seen:
            return False
        self.seen[event_key] = now + TTL
        # Bound transient metadata even if an event source floods scans.
        if len(self.seen) > 512:
            self.seen.pop(next(iter(self.seen)))
        prior = self.recent.get(device)
        if prior and fired_at < prior[2]:
            return False  # Concurrent inventory lookups may finish out of event order.
        if prior and prior[0] == roll and now - prior[1] < 2:
            return False
        self.recent[device] = (roll, now, fired_at)
        self.pending[device] = {
            'id': str(uuid4()), 'spool_uuid': roll, 'expires_at': fired_at + TTL,
        }
        return True

    def poll(self, user, key):
        self._prune()
        row = self._binding(user, key)
        if not row:
            return {'paired': False, 'scan': None}
        pending = self.pending.get(row['device_id'])
        return {'paired': True, 'device_id': row['device_id'],
                'scan': dict(pending) if pending else None}

    def ack(self, user, key, request_id):
        result = self.poll(user, key)
        if result['scan'] and result['scan']['id'] == request_id:
            self.pending.pop(result['device_id'], None)
            return True
        return False
