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


class AccountNfcBroker(NfcBroker):
    """Account-scoped intents; displaying is separate from committing an assignment."""

    def __init__(self, bindings=None, preferences=None, clock=time.time):
        super().__init__(bindings=bindings, clock=clock)
        preferences = preferences if isinstance(preferences, dict) else {}
        self.defaults = {user: device for user, device in preferences.get('defaults', {}).items()
                         if isinstance(user, str) and (device is None or isinstance(device, str))}
        self.labels = preferences.get('labels', {})
        if not isinstance(self.labels, dict):
            self.labels = {}
        self.requests = {}
        self.route_seen = {}
        self.route_recent = {}

    def dump_preferences(self):
        from copy import deepcopy
        return deepcopy({'defaults': self.defaults, 'labels': self.labels})

    def set_default(self, user, device):
        if not isinstance(user, str) or not user or (device is not None and not isinstance(device, str)):
            raise ValueError('Invalid default recipient')
        self.defaults[user] = device

    def mark_label(self, roll, kind, confirmed, source):
        roll = str(UUID(roll))
        if kind not in ('nfc', 'qr') or type(confirmed) is not bool or source not in ('native_write', 'user_confirmation'):
            raise ValueError('Invalid label confirmation')
        if kind == 'qr' and source != 'user_confirmation':
            raise ValueError('QR printing requires user confirmation')
        self.labels.setdefault(roll, {})[kind] = {
            'confirmed': confirmed, 'source': source, 'updated_at': self.clock()}

    def _route_prune(self):
        now = self.clock()
        self.requests = {k: v for k, v in self.requests.items() if v['commit_expires_at'] > now}
        self.route_seen = {k: v for k, v in self.route_seen.items() if v > now}
        self.route_recent = {k: v for k, v in self.route_recent.items() if now - v[1] < TTL}

    def route_scan(self, user, device, roll, devices, *, event_id, fired_at):
        self._route_prune()
        now = self.clock()
        known = {row['id'] for row in devices}
        try:
            roll, fired_at = str(UUID(roll)), float(fired_at)
        except (ValueError, TypeError, AttributeError):
            return False
        if device not in known or not now - TTL < fired_at <= now + 5:
            return False
        event = (user, device, str(event_id))
        prior = self.route_recent.get((user, device))
        if event in self.route_seen or (prior and (fired_at < prior[2] or (prior[0] == roll and now - prior[1] < 2))):
            return False
        self.route_seen[event] = now + TTL
        self.route_recent[(user, device)] = (roll, now, fired_at)
        if len(self.route_seen) > 512:
            self.route_seen.pop(next(iter(self.route_seen)))
        selected = user in self.defaults
        target = self.defaults.get(user)
        if target is not None and target not in known:
            selected, target = False, None
        if not selected and len(known) == 1:
            target = next(iter(known))
        unidentified = target is not None and len(known) > 1 and not any(
            r['user_id'] == user and r['device_id'] == target for r in self.bindings.values())
        ident = str(uuid4())
        self.requests[ident] = {'id': ident, 'user': user, 'source_device': device,
            'spool_uuid': roll, 'target_device': target,
            'needs_choice': (not selected and len(known) > 1) or unidentified,
            'duplicate_notice': target is None, 'expires_at': fired_at + TTL,
            'commit_expires_at': fired_at + 900, 'displayed': set(), 'completed': False}
        if len(self.requests) > 128:
            self.requests.pop(next(iter(self.requests)))
        return True

    def _request(self, user, ident):
        self._route_prune()
        row = self.requests.get(ident)
        if not row or row['user'] != user:
            raise ValueError('Scan expired or belongs to another user')
        return row

    def route_poll(self, user, key, devices, companion=False):
        self._route_prune()
        digest = self._hash(key)
        known = {row['id'] for row in devices}
        binding = self._binding(user, key)
        if binding and binding['device_id'] not in known:
            binding = None
        # A single existing registration is unambiguous inside Companion only.
        if not binding and companion and len(known) == 1:
            self.bind(user, key, next(iter(known)))
            binding = self._binding(user, key)
        recipient = binding['device_id'] if binding else None
        candidates = [r for r in self.requests.values() if r['user'] == user
                      and not r['completed'] and r['expires_at'] > self.clock()
                      and digest not in r['displayed']
                      and (r['needs_choice'] or r['target_device'] is None or r['target_device'] == recipient)]
        row = candidates[-1] if candidates else None
        scan = ({k: v for k, v in row.items() if k not in ('user', 'displayed', 'commit_expires_at')}
                if row else None)
        if scan:
            scan['expires_in'] = max(0, row['expires_at'] - self.clock())
        return {'enabled': True, 'paired': bool(binding), 'device_id': recipient,
                'default_defined': user in self.defaults, 'default_device': self.defaults.get(user),
                'devices': devices, 'scan': scan}

    def select_route(self, user, key, ident, device, devices):
        row = self._request(user, ident)
        if row['completed'] or row['expires_at'] <= self.clock() or not row['needs_choice']:
            raise ValueError('Recipient was already selected or the scan expired')
        if device is not None and device not in {r['id'] for r in devices}:
            raise ValueError('Choose a device registered to this user')
        if device is not None:
            # An existing destination stays attached to its known frontend. Without
            # one, the explicit device choice identifies the current frontend.
            if not any(r['user_id'] == user and r['device_id'] == device for r in self.bindings.values()):
                self.bind(user, key, device)
        row.update(target_device=device, needs_choice=False, duplicate_notice=device is None)

    def route_ack(self, user, key, ident):
        row = self._request(user, ident)
        row['displayed'].add(self._hash(key))
        if len(row['displayed']) > 128:
            raise ValueError('Too many recipients')
        return True

    def route_status(self, user, ident):
        row = self._request(user, ident)
        return {'completed': row['completed'], 'expires_in': max(0, row['commit_expires_at'] - self.clock())}

    def validate_assignment(self, user, ident, data):
        row = self._request(user, ident)
        if (row['needs_choice'] or data.get('spool_uuid') != row['spool_uuid']
                or data.get('request_key') != ident.replace('-', '')):
            raise ValueError('Assignment does not match the scanned roll')

    def route_complete(self, user, ident):
        self._request(user, ident)['completed'] = True
