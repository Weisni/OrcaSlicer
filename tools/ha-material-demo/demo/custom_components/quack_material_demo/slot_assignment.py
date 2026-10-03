"""Durable, opt-in inventory/printer assignment. No physical loading commands."""
import asyncio
import hashlib
import json
import re
import time

from .store import Conflict, SLOTS, now

# Verified against the installed Bambu integration's filaments_detail.json.
PROFILES = {
    'PLA': dict(name='Bambu PLA Basic', tray_info_idx='GFA00', nozzle_temp_min=190, nozzle_temp_max=240),
    'PETG': dict(name='Bambu PETG Basic', tray_info_idx='GFG00', nozzle_temp_min=230, nozzle_temp_max=270),
    'TPU': dict(name='Generic TPU', tray_info_idx='GFU99', nozzle_temp_min=200, nozzle_temp_max=250),
}


class CommandNotSent(Conflict):
    """Adapter rejected a command before invoking the printer service."""


def validate_config(config, active_slots=SLOTS):
    slots = config.get('slots', {})
    if not isinstance(slots, dict) or not set(active_slots) <= set(slots) or set(slots) - set(SLOTS):
        raise ValueError('Configure the enabled verified printer slot entities')
    if any(not isinstance(v, str) or not re.fullmatch(r'sensor\.[a-z0-9_]+', v) for v in slots.values()):
        raise ValueError('Invalid printer slot entity')
    if len(set(slots.values())) != len(slots):raise ValueError('Configure distinct printer slot entities')
    return dict(slots={slot:slots[slot] for slot in active_slots})


def printer_payload(roll, slot, config):
    if slot not in config['slots']:raise Conflict('Printer slot is disabled')
    material = roll.get('material_type', '').upper()
    if material not in PROFILES or (material == 'TPU' and slot != 'EXT'):
        raise Conflict('No verified printer profile / slot compatibility for this material')
    if roll.get('status', 'active') != 'active' or roll.get('remaining_mg', 0) <= 0:
        raise Conflict('Select an active, nonempty inventory roll')
    color = roll.get('color', '')
    if not isinstance(color, str) or not re.fullmatch(r'#[0-9a-fA-F]{6}', color):
        raise Conflict('The inventory roll needs a valid RGB color')
    return dict(entity_id=config['slots'][slot], tray_type=material,
                tray_color=color[1:].upper()+'FF',
                **{k:v for k,v in PROFILES[material].items() if k != 'name'})


def material_matches(state, payload):
    if not state or not state.get('available'):
        return False
    try:
        return (state.get('filament_id') == payload['tray_info_idx']
                and str(state.get('type', '')).upper() == payload['tray_type']
                and int(state.get('nozzle_temp_min')) == payload['nozzle_temp_min']
                and int(state.get('nozzle_temp_max')) == payload['nozzle_temp_max'])
    except (TypeError, ValueError):
        return False


def rgb(value):
    value = str(value or '').lstrip('#').upper()
    return '#'+value[:6] if re.fullmatch(r'[0-9A-F]{6}([0-9A-F]{2})?', value) else None


def matches(state, payload):
    return material_matches(state, payload) and rgb(state.get('color')) == rgb(payload['tray_color'])


def color_result(state, payload):
    requested, reported = rgb(payload['tray_color']), rgb((state or {}).get('color'))
    return dict(requested_color=requested, reported_color=reported,
                color_status='exact' if reported == requested else 'approximate' if reported else 'unknown',
                material_confirmed=material_matches(state, payload))


def observed_after(state, operation):
    # An unchanged cached read is not evidence that the command reached the device.
    observed = (state or {}).get('observed_at')
    return bool(operation.get('attempted_at') and observed
                and observed != operation.get('observed_before'))


def material_acknowledged(state, operation):
    return matches(state, operation['payload']) or (material_matches(state, operation['payload'])
        and rgb((state or {}).get('color')) and observed_after(state, operation))


class AssignmentCoordinator:
    def __init__(self, store, config, send, read_slot, ready, executor, timeout=15, interval=0.25):
        self.store, self.config = store, validate_config(config, store.active_slots)
        self.send, self.read_slot, self.ready, self.executor = send, read_slot, ready, executor
        self.timeout, self.interval = timeout, interval
        self.lock = asyncio.Lock()

    def initialize(self):
        with self.store.connection() as db:
            db.execute('''CREATE TABLE IF NOT EXISTS printer_assignments
                (request_key TEXT PRIMARY KEY, request TEXT NOT NULL, operation TEXT NOT NULL)''')
            db.execute('''CREATE TABLE IF NOT EXISTS printer_metadata_sync
                (slot TEXT PRIMARY KEY, operation TEXT NOT NULL)''')

    def pending(self):
        self.initialize()
        with self.store.connection() as db:
            for row in db.execute('SELECT operation FROM printer_assignments ORDER BY rowid DESC'):
                operation = json.loads(row[0])
                if operation['status'] == 'pending':
                    return operation
        return None

    @staticmethod
    def request_fields(request):
        keys = ('slot', 'spool_uuid', 'revision', 'inventory_revision', 'request_key')
        data = {key:request[key] for key in keys}
        if (data['slot'] not in SLOTS or type(data['revision']) is not int
                or type(data['inventory_revision']) is not int
                or not isinstance(data['spool_uuid'], str)
                or not isinstance(data['request_key'], str)
                or not re.fullmatch(r'[a-zA-Z0-9_-]{8,100}', data['request_key'])):
            raise ValueError('Invalid assignment request')
        if any(flag in request and type(request[flag]) is not bool for flag in ('retry','cancel')):
            raise ValueError('Invalid recovery flag')
        if request.get('retry') and request.get('cancel'):
            raise ValueError('Choose one recovery operation')
        return data

    def existing(self, request):
        self.initialize()
        with self.store.connection() as db:
            row = db.execute('SELECT request,operation FROM printer_assignments WHERE request_key=?', (request['request_key'],)).fetchone()
            if row:
                if row['request'] != json.dumps(request, sort_keys=True):
                    raise Conflict('Operation ID already belongs to a different assignment')
                return json.loads(row['operation'])
        return None

    def prepare(self, request):
        self.initialize()
        encoded = json.dumps(request, sort_keys=True)
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            previous = db.execute('SELECT request,operation FROM printer_assignments WHERE request_key=?',
                                  (request['request_key'],)).fetchone()
            if previous:
                if previous['request'] != encoded:
                    raise Conflict('Operation ID already belongs to a different assignment')
                return json.loads(previous['operation']), False
            if any(json.loads(row[0])['status'] == 'pending' for row in db.execute('SELECT operation FROM printer_assignments')):
                raise Conflict('Resolve the pending printer assignment first')
            snapshot = self.store.snapshot(db)
            if snapshot['revision'] != request['inventory_revision']:
                raise Conflict('Inventory changed; reopen the selection')
            roll = next((r for r in snapshot['spools'] if r['uuid'] == request['spool_uuid']), None)
            if not roll:
                raise Conflict('Unknown inventory roll')
            self.store.assign_in_transaction(db, request['slot'], roll['uuid'], request['revision'], validate_only=True)
            payload = printer_payload(roll, request['slot'], self.config)
            operation = dict(request, status='pending', payload=payload, created_at=now(),
                             message='Waiting for printer state confirmation')
            db.execute('INSERT INTO printer_assignments VALUES (?,?,?)',
                       (request['request_key'], encoded, json.dumps(operation)))
            return operation, True

    def finish(self, operation, state):
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            self.validate_operation(operation, db)
            self.store.assign_in_transaction(db, operation['slot'], operation['spool_uuid'], operation['revision'])
            result = dict(operation, status='confirmed', confirmed_at=now(), **color_result(state, operation['payload']))
            result['message'] = ('Material and HA assignment confirmed; printer reports an approximate color.'
                                 if result['color_status'] != 'exact' else 'Printer state and HA assignment confirmed')
            db.execute('UPDATE printer_assignments SET operation=? WHERE request_key=?',
                       (json.dumps(result), operation['request_key']))
            # The assignment already sent this desired payload; background refresh must not send it again.
            metadata = self.metadata_desired(self.store.snapshot(db), operation['slot'])
            self.save_metadata(dict(metadata, **{k:v for k,v in result.items()
                if k not in metadata}), db)
            return result

    def save_assignment(self, operation):
        with self.store.connection() as db:
            db.execute('UPDATE printer_assignments SET operation=? WHERE request_key=?',
                       (json.dumps(operation), operation['request_key']))

    def validate_operation(self, operation, db=None):
        if db is None:
            with self.store.connection() as connection:
                connection.execute('BEGIN')
                return self.validate_operation(operation, connection)
        roll = next((r for r in self.store.snapshot(db)['spools'] if r['uuid'] == operation['spool_uuid']), None)
        if not roll or printer_payload(roll, operation['slot'], self.config) != operation['payload']:
            raise Conflict('Selected roll material changed; manual reconciliation required')
        self.store.assign_in_transaction(db, operation['slot'], operation['spool_uuid'], operation['revision'], validate_only=True)

    def cancel(self, operation):
        # Explicitly clear the canonical slot, never pretend to roll back a device.
        # Existing revision/job protection still applies even if the selected roll is empty.
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            current = db.execute('SELECT spool_uuid FROM slots WHERE id=?', (operation['slot'],)).fetchone()
            if current['spool_uuid'] is not None:
                self.store.assign_in_transaction(db, operation['slot'], None, operation['revision'])
            # Reconciliation may already have cleared the slot and advanced its revision.
            # Terminalize without touching that newer empty binding.
            result = dict(operation, status='cancelled', cancelled_at=now(),
                          message='HA slot cleared. Printer material is unverified; select and confirm a stock roll before use.')
            db.execute('UPDATE printer_assignments SET operation=? WHERE request_key=?',
                       (json.dumps(result), operation['request_key']))
            self.store.event(db, 'printer_assignment_cancelled', dict(slot=operation['slot'], request_key=operation['request_key']))
            return result

    def check_device(self, slot):
        if slot not in self.config['slots']:raise Conflict('Printer slot is disabled')
        if not self.ready():
            raise Conflict('Printer must be online and idle before assigning material')
        state = self.read_slot(slot)
        if not state or not state.get('available'):
            raise Conflict('Printer slot is unavailable')
        if slot != 'EXT' and state.get('empty') is not False:
            raise Conflict('Insert a physical roll into this AMS slot before assigning it')

    async def assign(self, request):
        data = self.request_fields(request)
        if data['slot'] not in self.config['slots']:raise Conflict('Printer slot is disabled')
        async with self.lock:
            operation = await self.executor(self.existing, data)
            if operation and operation['status'] != 'pending':
                return operation
            if request.get('cancel'):
                if not operation:
                    raise Conflict('Only an existing pending assignment can be cancelled')
                if not self.ready():
                    raise Conflict('Printer must be idle before clearing its HA assignment')
                return await self.executor(self.cancel, operation)
            self.check_device(data['slot'])
            operation, created = await self.executor(self.prepare, data)
            await self.executor(self.validate_operation, operation)
            self.check_device(data['slot'])
            # Repeated POSTs only reconcile. Resending requires an explicit Retry.
            state = self.read_slot(data['slot'])
            if material_acknowledged(state, operation):
                return await self.executor(self.finish, operation, state)
            if created or request.get('retry') is True:
                operation = dict(operation, attempted_at=now(), observed_before=(state or {}).get('observed_at'))
                await self.executor(self.save_assignment, operation)
                try:
                    self.check_device(data['slot'])
                except Conflict as error:
                    operation = dict(operation, attempted_at=None, observed_before=None, message=str(error))
                    await self.executor(self.save_assignment, operation)
                    return operation
                try:
                    await asyncio.wait_for(self.send(operation['payload']), timeout=10)
                except CommandNotSent as error:
                    operation = dict(operation, attempted_at=None, observed_before=None, message=str(error))
                    await self.executor(self.save_assignment, operation)
                    return operation
                except Exception:
                    return dict(operation, message='Printer command outcome is uncertain. Check state or explicitly retry.')
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                self.check_device(data['slot'])
                state = self.read_slot(data['slot'])
                if material_acknowledged(state, operation):
                    return await self.executor(self.finish, operation, state)
                await asyncio.sleep(self.interval)
            return dict(operation, message='Printer has not confirmed the requested material. HA binding is unchanged; check or retry.')

    def metadata_desired(self, snapshot, slot):
        binding = next(s for s in snapshot['slots'] if s['id'] == slot)
        roll = next((r for r in snapshot['spools'] if r['uuid'] == binding['spool_uuid']), None)
        if not roll:
            raise Conflict('Slot has no inventory roll')
        if any(a['spool_uuid'] == roll['uuid'] for j in snapshot['jobs'] if j['settlement'] is None for a in j['allocations']):
            raise Conflict('Roll has an unsettled print job')
        payload = printer_payload(roll, slot, self.config)
        fingerprint = hashlib.sha256(json.dumps([roll['uuid'], payload], sort_keys=True).encode()).hexdigest()
        return dict(slot=slot, spool_uuid=roll['uuid'], revision=binding['revision'],
                    payload=payload, fingerprint=fingerprint)

    def metadata_status(self):
        self.initialize()
        with self.store.connection() as db:
            return {row['slot']:json.loads(row['operation']) for row in db.execute('SELECT * FROM printer_metadata_sync')}

    def save_metadata(self, operation, db=None):
        if db is None:
            with self.store.connection() as connection:
                connection.execute('BEGIN IMMEDIATE')
                current = self.metadata_desired(self.store.snapshot(connection), operation['slot'])
                if any(current[k] != operation[k] for k in ('fingerprint', 'revision')):
                    raise Conflict('HA slot changed during metadata synchronization')
                return self.save_metadata(operation, connection)
        db.execute('INSERT INTO printer_metadata_sync VALUES (?,?) ON CONFLICT(slot) DO UPDATE SET operation=excluded.operation',
                   (operation['slot'], json.dumps(operation)))

    def defer_metadata(self, operation):
        # Definitely no service call was made. Clear the attempt marker even if a
        # newly reserved job prevents ordinary metadata validation at this moment.
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT operation FROM printer_metadata_sync WHERE slot=?', (operation['slot'],)).fetchone()
            if row and json.loads(row['operation']) == operation:
                self.save_metadata(dict(operation, status='deferred', attempted_at=None, observed_before=None,
                    message='Metadata has not been sent; waiting for an idle, unchanged slot.'), db)

    async def reconcile_metadata(self):
        """Send each current desired metadata payload at most once; polling only acknowledges it."""
        for slot in self.config['slots']:
            # Yield the mutation lock between slots. Background metadata never
            # monopolizes it for six printer timeouts while a client is saving.
            async with self.lock:
                if not self.ready() or await self.executor(self.pending):
                    return
                saved = await self.executor(self.metadata_status)
                unsent = None
                try:
                    self.check_device(slot)
                    desired = self.metadata_desired(await self.executor(self.store.snapshot), slot)
                    state = self.read_slot(slot)
                    operation = saved.get(slot)
                    if operation and operation['fingerprint'] == desired['fingerprint'] and operation['status'] != 'deferred':
                        # Preserve an uncertain receipt across restarts. No automatic command replay.
                        prior = operation
                        operation = dict(operation, revision=desired['revision'])
                        if material_acknowledged(state, operation):
                            operation = dict(operation, status='confirmed', **color_result(state, desired['payload']))
                        elif not material_matches(state, desired['payload']):
                            operation = dict(operation, status='uncertain', material_confirmed=False, color_status='unknown',
                                reported_color=rgb((state or {}).get('color')),
                                message='Current printer material differs from HA; review the slot. No command was resent.')
                        if operation != prior:
                            await self.executor(self.save_metadata, operation)
                        continue
                    operation = dict(desired, status='pending', attempted_at=now(),
                                     observed_before=(state or {}).get('observed_at'),
                                     requested_color=rgb(desired['payload']['tray_color']), reported_color=rgb((state or {}).get('color')),
                                     color_status='unknown', material_confirmed=False)
                    if matches(state, desired['payload']):
                        await self.executor(self.save_metadata, dict(operation, status='confirmed', **color_result(state, desired['payload'])))
                        continue
                    # Receipt is durable before the service call, including in a crash/timeout window.
                    await self.executor(self.save_metadata, operation)
                    unsent = operation
                    fresh = self.metadata_desired(await self.executor(self.store.snapshot), slot)
                    if any(fresh[k] != operation[k] for k in ('fingerprint','revision')):
                        await self.executor(self.defer_metadata, operation)
                        continue
                    self.check_device(slot)
                    unsent = None
                    try:
                        await asyncio.wait_for(self.send(operation['payload']), timeout=2)
                    except CommandNotSent:
                        await self.executor(self.defer_metadata, operation)
                        continue
                    except Exception:
                        operation.update(status='uncertain', message='Metadata command outcome is uncertain; it will not be resent automatically.')
                    else:
                        state = self.read_slot(slot)
                        if material_acknowledged(state, operation):
                            operation.update(status='confirmed', **color_result(state, operation['payload']))
                        else:
                            operation.update(status='uncertain', message='Waiting for fresh printer metadata; exact color has not been confirmed.')
                    await self.executor(self.save_metadata, operation)
                except (Conflict, ValueError, KeyError, TypeError):
                    # Busy/empty/reserved/unsupported/stale slots are revisited after a later authoritative update.
                    if unsent is not None:
                        await self.executor(self.defer_metadata, unsent)
                    continue
