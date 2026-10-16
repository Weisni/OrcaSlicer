"""Durable single-printer preheat policy; returned intents never dispatch a print.

All temperatures are Celsius and observation timestamps are Unix seconds from
trusted printer telemetry. The HA adapter serializes tick/authorize/service/ack
with mutations. An authorization is valid only while that adapter lock is held.
"""
import hashlib
import json
import math
import time
import uuid

from .store import Conflict


FRESH_SECONDS = 30
WINDOW_SECONDS = 600
REFRESH_SECONDS = 60
LIFETIME_SECONDS = 2700
MAX_EXTENSION_SECONDS = 900
TERMINAL = {'cancelled', 'expired', 'failed', 'manual_takeover', 'handoff', 'needs_review', 'transferred'}
BASE_FIELDS = ('status', 'door_closed')
HEAT_FIELDS = ('chamber_temp', 'bed_temp', 'bed_target', 'air_mode',
               'idle_protection', 'maintenance_mode')


SCHEMAS = {
    'job_preheat': 'CREATE TABLE job_preheat(job_uuid TEXT PRIMARY KEY, '
                   'printer_id TEXT NOT NULL, active INTEGER NOT NULL, data TEXT NOT NULL)',
    'manual_preheat_attempts': 'CREATE TABLE manual_preheat_attempts(job_uuid TEXT PRIMARY KEY, '
                               'printer_id TEXT NOT NULL, created_at REAL NOT NULL)',
    'job_preheat_receipts': 'CREATE TABLE job_preheat_receipts(request_key TEXT PRIMARY KEY, '
                            'fingerprint TEXT NOT NULL, job_uuid TEXT NOT NULL)',
}
INDEXES = {
    'job_preheat_printer': 'CREATE UNIQUE INDEX job_preheat_printer '
                           'ON job_preheat(printer_id) WHERE active=1',
}


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _text(value):
    if not isinstance(value, str) or not value.strip() or len(value) > 128:
        raise ValueError('Provide a short nonempty identity')
    return value


class Preheat:
    """One active attempt per physical printer, sharing inventory transactions."""
    def __init__(self, store, clock=time.time, observation_reader=None):
        self.store = store
        self.clock = clock
        self.observation_reader = observation_reader
        self.generation = str(uuid.uuid4())
        with store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            for statement in SCHEMAS.values():
                db.execute(statement.replace('CREATE TABLE ', 'CREATE TABLE IF NOT EXISTS ', 1))
            self._migrate_legacy_job_reference(db)
            for statement in INDEXES.values():
                db.execute(statement.replace('CREATE UNIQUE INDEX ', 'CREATE UNIQUE INDEX IF NOT EXISTS ', 1))

    @staticmethod
    def _migrate_legacy_job_reference(db):
        """Remove the old print-job FK without losing attempts or their history."""
        references = db.execute('PRAGMA foreign_key_list(job_preheat)').fetchall()
        if not references:
            return
        if any(row['table'] != 'jobs' or row['from'] != 'job_uuid' for row in references):
            raise ValueError('Unsupported preheat foreign-key schema; preserve it for review')
        columns = [row['name'] for row in db.execute('PRAGMA table_info(job_preheat)')]
        if columns != ['job_uuid', 'printer_id', 'active', 'data']:
            raise ValueError('Unsupported preheat columns; preserve them for review')
        attached = [row['sql'] for row in db.execute(
            "SELECT sql FROM sqlite_master WHERE tbl_name='job_preheat' "
            "AND type IN ('index','trigger') AND sql IS NOT NULL")]
        db.execute(SCHEMAS['job_preheat'].replace('CREATE TABLE job_preheat(',
                                                  'CREATE TABLE job_preheat_migration('))
        db.execute('INSERT INTO job_preheat_migration SELECT job_uuid,printer_id,active,data FROM job_preheat')
        db.execute('DROP TABLE job_preheat')
        db.execute('ALTER TABLE job_preheat_migration RENAME TO job_preheat')
        for statement in attached:
            db.execute(statement)

    def _load(self, db, ident):
        row = db.execute('SELECT data FROM job_preheat WHERE job_uuid=?', (ident,)).fetchone()
        if not row:
            raise Conflict('Request preheat for this attempt first')
        return json.loads(row['data'])

    def _save(self, db, job):
        active = int(job['phase'] not in TERMINAL or job['phase'] == 'handoff')
        db.execute('INSERT INTO job_preheat VALUES (?,?,?,?) ON CONFLICT(job_uuid) '
                   'DO UPDATE SET active=excluded.active,data=excluded.data',
                   (job['job_uuid'], job['printer_id'], active, json.dumps(job, allow_nan=False)))

    def _inventory_job(self, db, ident, physical):
        manual = db.execute('SELECT printer_id FROM manual_preheat_attempts WHERE job_uuid=?', (ident,)).fetchone()
        if manual:
            return manual['printer_id'] == physical
        row = db.execute('SELECT state,settlement FROM jobs WHERE uuid=?', (ident,)).fetchone()
        origin = db.execute('SELECT source FROM origins WHERE job_uuid=?', (ident,)).fetchone()
        native = db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
        native_job = next((j for j in json.loads(native['data'])['tables']['print_jobs']
                           if j['id'] == ident), None) if native else None
        return bool(row and row['state'] == 'reserved' and row['settlement'] is None and
                    origin and origin['source'] == 'quack_provider' and native_job and
                    native_job.get('printer_id') == physical and native_job.get('state') == 'reserved')

    def _receipt(self, db, command, ident, data):
        key = _text(data.get('request_key'))
        payload = json.dumps([command, ident, data], sort_keys=True, allow_nan=False)
        fingerprint = hashlib.sha256(payload.encode()).hexdigest()
        row = db.execute('SELECT * FROM job_preheat_receipts WHERE request_key=?', (key,)).fetchone()
        if row and (row['fingerprint'] != fingerprint or row['job_uuid'] != ident):
            raise Conflict('Preheat request key was already used for different data')
        return key, fingerprint, bool(row)

    @staticmethod
    def _record_receipt(db, receipt, ident):
        db.execute('INSERT INTO job_preheat_receipts VALUES (?,?,?)', (receipt[0], receipt[1], ident))

    def _safe(self, observation, physical, heat, *, allow_open_door=False):
        if not isinstance(observation, dict) or observation.get('printer_id') != physical:
            return 'printer_identity'
        fields = BASE_FIELDS + (HEAT_FIELDS if heat else ())
        current = self.clock()
        for name in fields:
            timestamp = observation.get(name + '_at')
            if not _number(timestamp) or not 0 <= current - timestamp <= FRESH_SECONDS:
                return 'stale_' + name
        if observation.get('status') not in ('idle', 'finish', 'failed'):
            return 'printer_busy_or_unknown'
        if observation.get('door_closed') is not True and not (
                allow_open_door and observation.get('door_closed') is False):
            return 'door_open_or_unknown'
        if heat:
            if observation.get('idle_protection') is not True:
                return 'idle_protection_unavailable'
            if observation.get('maintenance_mode') is not False:
                return 'maintenance_mode'
            for name in ('chamber_temp', 'bed_temp', 'bed_target'):
                if not _number(observation.get(name)) or not -20 <= observation[name] <= 150:
                    return 'invalid_' + name
            if observation.get('air_mode') not in ('internal', 'external'):
                return 'air_mode_unknown'
            if type(observation.get('door_open_count')) is not int or observation['door_open_count'] < 0:
                return 'door_history_unknown'
        return None

    def _transition(self, db, job, phase, reason):
        if job['phase'] != phase or job['reason'] != reason:
            job.update(phase=phase, reason=reason)
            self.store.event(db, 'job_preheat_state', dict(job_uuid=job['job_uuid'],
                              phase=phase, reason=reason, version=job['version']))

    @staticmethod
    def _reset(job):
        job['samples'] = []
        job['stable'] = False
        job['conditioning'] = False
        job.pop('target_conditioning', None)

    @staticmethod
    def _stable(samples):
        if len(samples) < 2 or samples[-1][0] - samples[0][0] < WINDOW_SECONDS:
            return False
        values = [sample[1] for sample in samples]
        if max(values) - min(values) > 1:
            return False
        xs = [(sample[0] - samples[0][0]) / 60 for sample in samples]
        mean_x, mean_y = sum(xs) / len(xs), sum(values) / len(values)
        denominator = sum((x - mean_x) ** 2 for x in xs)
        slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, values)) / denominator
        return abs(slope) <= 0.05

    def _manual_change(self, job, observation):
        # A new target is accepted only if it belongs to the pending command.
        # The previous target is permitted briefly while a successful service
        # waits for its actual printer telemetry acknowledgement.
        target = observation['bed_target']
        allowed = {job['expected_bed_target']}
        pending = job.get('pending')
        if pending and pending['kind'] == 'bed_target':
            allowed.add(pending['value'])
        if job.get('previous_bed_target') is not None and self.clock() - job.get('bed_changed_at', 0) <= FRESH_SECONDS:
            allowed.add(job['previous_bed_target'])
        # An owned heater can be reset to zero by the P2S idle protection.
        # While this bounded job is active, zero requests target recovery;
        # cancellation of the job is the explicit way to stop that recovery.
        if target not in allowed and not (target == 0 and job.get('bed_owned') and job['expected_bed_target'] > 0):
            return True
        if target == job['expected_bed_target']:
            job['previous_bed_target'] = None
            if job.get('bed_owned'):
                job['bed_target_confirmed'] = True
        if observation['air_mode'] == 'internal':
            job['air_confirmed'] = True
        return bool(job.get('air_owned') and observation['air_mode'] != 'internal' and
                    (job.get('air_confirmed') or self.clock() - job.get('air_changed_at', 0) > FRESH_SECONDS))

    def _evaluate(self, db, job, observation, *, sample=True):
        current = self.clock()
        job['observation'] = observation if isinstance(observation, dict) else {}
        if not self._inventory_job(db, job['job_uuid'], job['printer_id']):
            if job['phase'] == 'handoff':
                # Dispatch owns the printer from this point. Only a terminal
                # inventory outcome releases the preheat slot; it never cools.
                row = db.execute('SELECT state,settlement FROM jobs WHERE uuid=?', (job['job_uuid'],)).fetchone()
                if row and row['state'] in ('printing', 'paused', 'needs_review') and not row['settlement']:
                    return
            self._transition(db, job, 'cancelled', 'inventory_attempt_changed')
            job['pending'] = None
            return
        if job['phase'] in TERMINAL:
            return
        if current >= job['expires_at']:
            samples = job['samples']
            self.store.event(db, 'job_preheat_deadline', dict(job_uuid=job['job_uuid'],
                previous_phase=job['phase'], previous_reason=job['reason'],
                sample_count=len(samples), window_seconds=samples[-1][0]-samples[0][0] if samples else 0,
                bed_target=job['observation'].get('bed_target'), chamber_temp=job['observation'].get('chamber_temp')))
            self._transition(db, job, 'expired', 'deadline')
            job['pending'] = None
            self._reset(job)
            return
        if job['generation'] != self.generation:
            job.update(generation=self.generation, pending=None, last_sent_at=0)
            job.pop('bed_reset_started_at', None)
            job.pop('bed_reset_sent', None)
            self._reset(job)
        if job['phase'] == 'interrupted' and job['reason'] == 'heater_command_failed':
            return
        heat = job['parameters']['enabled'] and (not job.get('skipped') or job.get('skip_conditioning'))
        if heat:
            count = observation.get('door_open_count')
            if (type(count) is int and count != job['door_open_count']) or observation.get('door_closed') is False:
                prior = [job.get('last_chamber_temp'), job.get('recovery_temperature')]
                if job['samples']:
                    prior.append(job['samples'][-1][1])
                if observation.get('door_closed') is False:
                    prior.append(observation.get('chamber_temp'))
                values = [value for value in prior if _number(value) and -20 <= value <= 150]
                job['recovery_temperature'] = max(values) if values else None
                self._reset(job)
                if type(count) is int:
                    job['door_open_count'] = count
        reason = self._safe(observation, job['printer_id'], heat, allow_open_door=heat)
        if reason:
            self._transition(db, job, 'interrupted', reason)
            job['pending'] = None
            self._reset(job)
            return
        if not heat:
            self._transition(db, job, 'ready', 'manual_override' if job.get('skipped') else 'not_required')
            return
        if self._manual_change(job, observation):
            self._transition(db, job, 'manual_takeover', 'bed_target_or_air_mode_changed')
            job['pending'] = None
            self._reset(job)
            return
        recovering_target = observation['bed_target'] == 0 and job.get('bed_owned') and job['expected_bed_target'] > 0
        if recovering_target:
            if job.get('bed_reset_started_at') is None:
                job['bed_reset_started_at'] = current
                job['bed_reset_sent'] = False
                if job.get('bed_target_confirmed'):
                    self.store.event(db, 'job_preheat_bed_reset', dict(job_uuid=job['job_uuid'],
                        observed_target=0, expected_target=job['expected_bed_target'], at=current))
            if current - job['bed_reset_started_at'] > FRESH_SECONDS:
                self._transition(db, job, 'interrupted', 'heater_command_failed')
                job['pending'] = None
                self._reset(job)
                return
            # Keep sampling actual temperatures during a brief zero-target gap.
            # Otherwise five-minute protection resets prevent a ten-minute
            # plateau forever. Actual cooling, open doors and stale data still
            # invalidate the thermal evidence through the ordinary checks below.
        else:
            job.pop('bed_reset_started_at', None)
            job.pop('bed_reset_sent', None)
        if observation['air_mode'] != 'internal':
            self._reset(job)
            self._transition(db, job, 'selecting_air', 'heating_air_mode_required')
            return
        if observation['door_closed'] is False:
            self._reset(job)
            self._transition(db, job, 'heating', 'door_open_heating')
            return
        temperature = observation['chamber_temp']
        job['last_chamber_temp'] = temperature
        target_met = temperature >= job['parameters']['chamber_target']
        if job.get('skipped'):
            job['conditioning'] = True
        elif job.get('recovery_temperature') is not None:
            if temperature < job['recovery_temperature'] and not target_met:
                self._reset(job)
                self._transition(db, job, 'heating', 'recovering_after_door_open')
                return
            job['recovery_temperature'] = None
        timestamp = observation['chamber_temp_at']
        samples = job['samples']
        if samples and (timestamp < samples[-1][0] or timestamp - samples[-1][0] > FRESH_SECONDS):
            self._reset(job)
            samples = job['samples']
        # Reaching the requested chamber temperature needs no plateau. This
        # path conditions the print bed directly, without heating it to the
        # higher warm-up target. Unlike a proven plateau, target-only readiness
        # is revoked if the chamber falls below its target before dispatch.
        if not job.get('skipped') and not job['stable']:
            if target_met:
                job['target_conditioning'] = True
                job['conditioning'] = True
            elif job.get('target_conditioning'):
                self._reset(job)
                samples = job['samples']
        # Chamber stability can be a lower plateau than the requested target.
        # The bed must first have reached the configured warm-up temperature.
        preheat_target = job['parameters']['preheat_bed_target']
        bed_heating_ready = observation['bed_temp'] >= preheat_target - 2
        if not job['conditioning'] and not bed_heating_ready:
            samples.clear()
        elif sample and (not samples or timestamp > samples[-1][0]):
            samples.append([timestamp, observation['chamber_temp']])
            while len(samples) > 1 and samples[1][0] <= timestamp - WINDOW_SECONDS:
                samples.pop(0)
        if job.get('skipped'):
            job['stable'] = False
            job['conditioning'] = True
        elif not job['conditioning'] and self._stable(samples):
            # A proven warm-up plateau remains latched while the commanded
            # lower print-bed temperature naturally cools the chamber. Door,
            # telemetry gaps and safety changes still clear it above.
            job['stable'] = True
            job['conditioning'] = True
        # Give a waiting print one bounded chance to finish its fresh plateau
        # window. Grant before expiry so connected clients can confirm the new
        # deadline; an expired owner or a manual hold is never revived/renewed.
        if (0 < job['expires_at'] - current <= REFRESH_SECONDS and not job['extended'] and
                not job['conditioning'] and not job.get('skipped') and len(samples) >= 2 and
                job.get('bed_owned') and job.get('bed_target_confirmed') and
                observation['bed_target'] == preheat_target and
                abs(observation['bed_temp'] - preheat_target) <= 2 and
                current - job['last_sent_at'] <= REFRESH_SECONDS + FRESH_SECONDS and
                not db.execute('SELECT 1 FROM manual_preheat_attempts WHERE job_uuid=?',
                               (job['job_uuid'],)).fetchone()):
            previous_expiry = job['expires_at']
            job['expires_at'] += MAX_EXTENSION_SECONDS
            job['extended'] = True
            self.store.event(db, 'job_preheat_auto_extension', dict(job_uuid=job['job_uuid'],
                reason='stabilization_window', previous_expires_at=previous_expiry,
                expires_at=job['expires_at'], seconds=MAX_EXTENSION_SECONDS))
        if job['conditioning']:
            target = job['parameters']['print_bed_target']
            if observation['bed_target'] == target and abs(observation['bed_temp'] - target) <= 2:
                self._transition(db, job, 'ready', 'manual_override' if job.get('skipped') else
                                 'target_reached' if job.get('target_conditioning') else 'stable')
            else:
                self._transition(db, job, 'conditioning_bed', 'print_bed_temperature_required')
        else:
            self._transition(db, job, 'heating', 'bed_target_reset' if recovering_target and job.get('bed_target_confirmed')
                             else 'stabilizing' if bed_heating_ready else 'warming_bed')

    @staticmethod
    def _response(job):
        result = {name: job[name] for name in ('job_uuid', 'printer_id', 'instance_id', 'version',
                  'phase', 'reason', 'expires_at', 'parameters')}
        result.update(schema_version=1, ready=job['phase'] == 'ready')
        if job.get('successor_job_uuid'):
            result['successor_job_uuid'] = job['successor_job_uuid']
        observation = job.get('observation')
        if observation:
            result['telemetry'] = observation
        return result

    def request(self, job_uuid, data, observation):
        return self._request(job_uuid, data, observation, manual=False)

    def request_manual(self, job_uuid, data, observation):
        return self._request(job_uuid, data, observation, manual=True)

    def _request(self, job_uuid, data, observation, *, manual):
        if not isinstance(data, dict) or set(data) != {'request_key', 'instance_id', 'printer_id', 'parameters'}:
            raise ValueError('Invalid preheat request')
        _text(job_uuid)
        _text(data['instance_id'])
        parameters = data['parameters']
        if not isinstance(parameters, dict) or set(parameters) != {'enabled', 'chamber_target', 'print_bed_target', 'preheat_bed_target'}:
            raise ValueError('Invalid preheat parameters')
        if type(parameters['enabled']) is not bool:
            raise ValueError('Preheat enabled must be boolean')
        for name, lower, upper in (('chamber_target', 1 if parameters['enabled'] else 0, 100), ('print_bed_target', 0, 110), ('preheat_bed_target', 0, 110)):
            if type(parameters[name]) is not int or not lower <= parameters[name] <= upper:
                raise ValueError('Preheat temperature outside supported limits')
        if parameters['enabled'] and not parameters['preheat_bed_target']:
            raise ValueError('Enabled preheat requires a nonzero bed target')
        if manual and (not parameters['enabled'] or parameters['print_bed_target'] != parameters['preheat_bed_target']):
            raise ValueError('Manual preheat requires enabled heating with equal hold-bed targets')
        physical = self.store.settings.get('provider_printer_id')
        if not physical or data['printer_id'] != physical:
            raise Conflict('Physical printer identity does not match HA configuration')
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            manual_row = db.execute('SELECT printer_id FROM manual_preheat_attempts WHERE job_uuid=?', (job_uuid,)).fetchone()
            if manual:
                if db.execute('SELECT 1 FROM jobs WHERE uuid=?', (job_uuid,)).fetchone():
                    raise Conflict('Manual preheat requires a separate identity from print attempts')
                if manual_row and manual_row['printer_id'] != physical:
                    raise Conflict('Manual preheat belongs to another physical printer')
                if not manual_row:
                    db.execute('INSERT INTO manual_preheat_attempts VALUES (?,?,?)',
                               (job_uuid, physical, self.clock()))
            elif manual_row:
                raise Conflict('A manual preheat identity cannot be used as a provider print attempt')
            receipt = self._receipt(db, 'manual_request' if manual else 'request', job_uuid, data)
            prior = db.execute('SELECT data FROM job_preheat WHERE job_uuid=?', (job_uuid,)).fetchone()
            if prior:
                job = json.loads(prior['data'])
                if any(job[name] != data[name] for name in ('instance_id', 'printer_id', 'parameters')):
                    raise Conflict('Changed preheat data requires a new print attempt')
                self._evaluate(db, job, observation)
                self._save(db, job)
                if not receipt[2]: self._record_receipt(db, receipt, job_uuid)
                return self._response(job)
            if receipt[2] or not self._inventory_job(db, job_uuid, physical):
                raise Conflict('Reserve the matching provider print attempt first')
            job = dict(job_uuid=job_uuid, printer_id=physical, instance_id=data['instance_id'],
                       parameters=dict(parameters), version=1, generation=self.generation,
                       phase='heating', reason='accepted', expires_at=self.clock() + LIFETIME_SECONDS,
                       extended=False, skipped=False, samples=[], stable=False, conditioning=False,
                       door_open_count=observation.get('door_open_count', 0),
                       expected_bed_target=observation.get('bed_target', 0), previous_bed_target=None,
                       air_owned=False, pending=None, last_sent_at=0, observation=observation)
            # Expired or manually closed occupants are released inside this same
            # admission transaction, so two connections cannot both win a slot.
            for row in db.execute('SELECT data FROM job_preheat WHERE active=1').fetchall():
                occupant = json.loads(row['data'])
                self._evaluate(db, occupant, observation)
                self._save(db, occupant)
                if (not manual and occupant['printer_id'] == physical and occupant['phase'] not in TERMINAL and
                        db.execute('SELECT 1 FROM manual_preheat_attempts WHERE job_uuid=? AND printer_id=?',
                                   (occupant['job_uuid'], physical)).fetchone()):
                    self._take_manual_ownership(db, job, occupant, observation)
            reason = self._admission_reason(db, observation, physical, parameters['enabled'])
            if reason:
                raise Conflict('Printer cannot accept preheat: ' + reason)
            self._evaluate(db, job, observation)
            self._save(db, job)
            self._record_receipt(db, receipt, job_uuid)
            self.store.event(db, 'job_preheat_request', dict(job_uuid=job_uuid, version=1))
            return self._response(job)

    def _take_manual_ownership(self, db, job, manual, observation):
        """Transfer the existing heater lease in the print admission transaction.

        The adapter holds its actuator lock throughout admission. Revoking old
        pending commands and installing the successor therefore cannot race a
        service send. A later admission failure rolls every change back.
        """
        reason = self._safe(observation, job['printer_id'], True)
        if reason:
            raise Conflict('Printer cannot transfer manual preheat: ' + reason)
        if manual['reason'] == 'heater_command_failed':
            raise Conflict('Retry or stop the failed manual heater attempt before printing')
        # Changing the owner never starts another lease or grants another
        # extension. Preserve in-flight actuator acknowledgement deadlines too.
        job['expires_at'] = manual['expires_at']
        job['extended'] = manual['extended']
        for name in ('expected_bed_target', 'previous_bed_target', 'bed_owned',
                     'bed_target_confirmed', 'bed_changed_at', 'bed_reset_started_at',
                     'bed_reset_sent', 'air_owned', 'air_confirmed', 'air_changed_at',
                     'last_sent_at', 'door_open_count', 'last_chamber_temp', 'recovery_temperature'):
            if name in manual:
                job[name] = manual[name]
        compatible = (job['parameters']['enabled'] and not manual['skipped'] and
                      all(job['parameters'][name] == manual['parameters'][name]
                          for name in ('chamber_target', 'preheat_bed_target')))
        if compatible:
            job['samples'] = [list(sample) for sample in manual['samples']]
            job['stable'] = manual['stable']
            job['conditioning'] = manual['conditioning']
            job['target_conditioning'] = manual.get('target_conditioning', False)
        manual['version'] += 1
        manual['pending'] = None
        manual['successor_job_uuid'] = job['job_uuid']
        self._reset(manual)
        self._transition(db, manual, 'transferred', 'print_preheat_claimed')
        self._save(db, manual)
        self.store.event(db, 'job_preheat_manual_transfer', dict(
            manual_job_uuid=manual['job_uuid'], job_uuid=job['job_uuid'],
            thermal_evidence_preserved=bool(compatible), expires_at=job['expires_at']))

    def _admission_reason(self, db, observation, physical, heat):
        for row in db.execute('SELECT data FROM job_preheat WHERE printer_id=? AND active=1', (physical,)):
            job = json.loads(row['data'])
            if job['phase'] == 'handoff' or (job['expires_at'] > self.clock() and
                    self._inventory_job(db, job['job_uuid'], physical)):
                return 'printer_owned'
        if db.execute("SELECT 1 FROM provider_dispatch WHERE printer_id=? "
                      "AND status IN ('prepared','accepted','uncertain','observing')", (physical,)).fetchone():
            return 'dispatch_pending'
        observed = db.execute("SELECT state,job_uuid FROM observations WHERE provider='duck-poop'").fetchone()
        if observed and (observed['job_uuid'] or observed['state'] in ('prepare', 'running', 'pause')):
            return 'printer_observation_pending'
        return self._safe(observation, physical, heat)

    def manual_admission_reason(self, observation):
        """Read-only diagnostic. Request admission always checks again atomically."""
        physical = self.store.settings.get('provider_printer_id')
        if not physical:
            return 'printer_identity'
        with self.store.connection() as db:
            db.execute('BEGIN')
            return self._admission_reason(db, observation, physical, True)

    def reconcile_modes(self, job_enabled, manual_enabled):
        """Revoke disabled owners before runtime timers can resume heating."""
        if type(job_enabled) is not bool or type(manual_enabled) is not bool:
            raise ValueError('Preheat mode flags must be boolean')
        changed = []
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            rows = db.execute('SELECT p.data,m.job_uuid AS manual_uuid FROM job_preheat AS p '
                              'LEFT JOIN manual_preheat_attempts AS m ON m.job_uuid=p.job_uuid '
                              'WHERE p.active=1').fetchall()
            for row in rows:
                enabled = manual_enabled if row['manual_uuid'] is not None else job_enabled
                if enabled:
                    continue
                job = json.loads(row['data'])
                job['version'] += 1
                job['pending'] = None
                self._reset(job)
                self._transition(db, job, 'cancelled', 'mode_disabled')
                self._save(db, job)
                changed.append(self._response(job))
        return changed

    def manual_status(self, observation):
        """Reconcile the latest manual attempt only; never issue commands."""
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT p.data FROM manual_preheat_attempts AS m '
                             'JOIN job_preheat AS p ON p.job_uuid=m.job_uuid '
                             'ORDER BY m.created_at DESC,m.rowid DESC LIMIT 1').fetchone()
            if not row:
                return None
            job = json.loads(row['data'])
            self._evaluate(db, job, observation)
            self._save(db, job)
            return self._response(job)

    def has_active_manual_in_transaction(self, db, physical):
        """Legacy-named guard: every durable owner blocks legacy dispatch."""
        if not db.in_transaction:
            raise ValueError('Manual preheat guard requires the existing dispatch transaction')
        rows = db.execute('SELECT data FROM job_preheat WHERE printer_id=? AND active=1',
                          (physical,)).fetchall()
        active = False
        for row in rows:
            job = json.loads(row['data'])
            self._evaluate(db, job, job.get('observation'), sample=False)
            self._save(db, job)
            active = active or job['phase'] not in TERMINAL or job['phase'] == 'handoff'
        return active

    def summaries(self, db=None):
        """Display-only stored state. A summary never grants fresh readiness."""
        if db is None:
            with self.store.connection() as connection:
                return self.summaries(connection)
        result = []
        for row in db.execute('SELECT data FROM job_preheat ORDER BY rowid'):
            summary = self._response(json.loads(row['data']))
            summary['ready'] = False
            result.append(summary)
        return result

    def status(self, job_uuid, observation):
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            job = self._load(db, job_uuid)
            self._evaluate(db, job, observation)
            self._save(db, job)
            return self._response(job)

    def _command(self, command, job_uuid, data, observation):
        expected = {'request_key', 'instance_id', 'version'} | ({'seconds'} if command == 'extend' else set())
        if not isinstance(data, dict) or set(data) != expected:
            raise ValueError('Invalid preheat command')
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            receipt = self._receipt(db, command, job_uuid, data)
            job = self._load(db, job_uuid)
            if job['instance_id'] != data['instance_id']:
                raise Conflict('This preheat attempt belongs to another client instance')
            self._evaluate(db, job, observation)
            if receipt[2]:
                self._save(db, job)
                return self._response(job)
            if type(data['version']) is not int or data['version'] != job['version']:
                raise Conflict('Preheat version changed; refresh first')
            if job['phase'] in TERMINAL:
                raise Conflict('This preheat attempt is already terminal or handed off')
            if command == 'extend':
                seconds = data['seconds']
                if type(seconds) is not int or not 1 <= seconds <= MAX_EXTENSION_SECONDS:
                    raise ValueError('Extension must be 1 to 900 seconds')
                if job['extended']:
                    raise Conflict('This preheat attempt was already extended')
                job['expires_at'] += seconds
                job['extended'] = True
            elif command == 'cancel':
                self._transition(db, job, 'cancelled', 'user_cancelled')
                self._reset(job)
            elif command == 'skip':
                job['skipped'] = True
                job['skip_conditioning'] = bool(job['parameters']['enabled'] and
                    (job.get('bed_owned') or job.get('air_owned')))
                self._reset(job)
            elif command == 'retry':
                if job['phase'] != 'interrupted':
                    raise Conflict('Only an interrupted attempt can be retried')
                self._reset(job)
                job.pop('bed_reset_started_at', None)
                job.pop('bed_reset_sent', None)
                self._transition(db, job, 'heating', 'retry_requested')
            job['version'] += 1
            job['pending'] = None
            job['last_sent_at'] = 0
            if command in ('skip', 'retry'):
                self._evaluate(db, job, observation)
            self._save(db, job)
            self._record_receipt(db, receipt, job_uuid)
            self.store.event(db, 'job_preheat_command', dict(job_uuid=job_uuid, command=command, version=job['version']))
            return self._response(job)

    def cancel(self, job_uuid, data, observation):
        return self._command('cancel', job_uuid, data, observation)

    def skip(self, job_uuid, data, observation):
        return self._command('skip', job_uuid, data, observation)

    def retry(self, job_uuid, data, observation):
        return self._command('retry', job_uuid, data, observation)

    def extend(self, job_uuid, data, observation):
        return self._command('extend', job_uuid, data, observation)

    def tick(self, observation):
        intents = []
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            for row in db.execute('SELECT data FROM job_preheat WHERE active=1').fetchall():
                job = json.loads(row['data'])
                self._evaluate(db, job, observation)
                if job['parameters']['enabled'] and (not job['skipped'] or job.get('skip_conditioning')) and job['phase'] in ('heating', 'selecting_air', 'conditioning_bed', 'ready'):
                    kind = 'air_mode' if job['phase'] == 'selecting_air' else 'bed_target'
                    value = 'internal' if kind == 'air_mode' else job['parameters']['print_bed_target' if job['conditioning'] else 'preheat_bed_target']
                    awaiting_reset = job.get('bed_reset_sent', False)
                    if not job['pending'] and not awaiting_reset and (self.clock() - job['last_sent_at'] >= REFRESH_SECONDS or observation.get(kind) != value):
                        intent = dict(job_uuid=job['job_uuid'], printer_id=job['printer_id'], version=job['version'],
                                      generation=self.generation, command_id=str(uuid.uuid4()), kind=kind, value=value)
                        job['pending'] = intent
                        intents.append(dict(intent))
                self._save(db, job)
        return intents

    def authorize(self, intent, observation):
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            try:
                job = self._load(db, intent.get('job_uuid'))
            except Conflict:
                return False
            self._evaluate(db, job, observation)
            allowed = bool(job.get('pending') == intent and intent.get('generation') == self.generation and
                           job['phase'] in ('heating', 'selecting_air', 'conditioning_bed', 'ready') and
                           job['parameters']['enabled'] and (not job['skipped'] or job.get('skip_conditioning')))
            if allowed:
                desired_kind = 'air_mode' if job['phase'] == 'selecting_air' else 'bed_target'
                desired_value = 'internal' if desired_kind == 'air_mode' else job['parameters']['print_bed_target' if job['conditioning'] else 'preheat_bed_target']
                allowed = intent['kind'] == desired_kind and intent['value'] == desired_value
                if not allowed:
                    # The printer may already report the old requested target
                    # while readiness advances to bed conditioning. Retain that
                    # observed allowed target as the baseline for the replacement.
                    if intent['kind'] == 'bed_target' and observation.get('bed_target') == intent['value']:
                        job['expected_bed_target'] = intent['value']
                    job['pending'] = None
            self._save(db, job)
            return allowed

    def acknowledge(self, intent, success, observation=None):
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            job = self._load(db, intent.get('job_uuid'))
            if job.get('pending') != intent or job['phase'] in TERMINAL or intent.get('generation') != self.generation:
                return
            job['pending'] = None
            if success:
                job['last_sent_at'] = self.clock()
                if intent['kind'] == 'bed_target':
                    job['bed_owned'] = True
                    if job['expected_bed_target'] != intent['value']:
                        job['bed_target_confirmed'] = False
                    job['previous_bed_target'] = job['expected_bed_target']
                    job['expected_bed_target'] = intent['value']
                    job['bed_changed_at'] = self.clock()
                else:
                    job['air_owned'] = True
                    job['air_confirmed'] = False
                    job['air_changed_at'] = self.clock()
                if observation is not None:
                    self._evaluate(db, job, observation)
                if intent['kind'] == 'bed_target' and job.get('bed_reset_started_at') is not None:
                    job['bed_reset_sent'] = True
            else:
                self._transition(db, job, 'interrupted', 'heater_command_failed')
                self._reset(job)
            self._save(db, job)
            self.store.event(db, 'job_preheat_heater_result', dict(job_uuid=job['job_uuid'],
                              kind=intent['kind'], success=bool(success), at=self.clock()))

    def claim_in_transaction(self, db, job_uuid, metadata):
        """Called once inside prepare_print, sharing its dispatch/material commit."""
        if not db.in_transaction:
            raise ValueError('Preheat claim requires the existing dispatch transaction')
        if not isinstance(metadata, dict) or set(metadata) != {'instance_id', 'version'}:
            raise ValueError('Provide the current preheat instance and version')
        if db.execute('SELECT 1 FROM manual_preheat_attempts WHERE job_uuid=?', (job_uuid,)).fetchone():
            raise Conflict('Manual preheat cannot authorize a print dispatch')
        job = self._load(db, job_uuid)
        if (job['generation'] != self.generation or metadata['instance_id'] != job['instance_id'] or
                type(metadata['version']) is not int or metadata['version'] != job['version']):
            raise Conflict('Preheat instance, version or HA generation changed')
        observation = job.get('observation')
        if self.observation_reader is not None:
            # Read the trusted raw tap inside the same inventory transaction as
            # dispatch admission. Never authorize from an earlier HTTP GET when
            # physical status, safety flags or door history have since changed.
            observation = self.observation_reader()
            if not isinstance(observation, dict):
                raise Conflict('Current trusted printer observation is unavailable')
        self._evaluate(db, job, observation, sample=self.observation_reader is not None)
        if job['phase'] != 'ready':
            raise Conflict('Fresh preheat readiness is required before dispatch')
        self._transition(db, job, 'handoff', 'dispatch_claimed')
        job['pending'] = None
        self._save(db, job)
        self.store.event(db, 'job_preheat_claim', dict(job_uuid=job_uuid, instance_id=job['instance_id'], version=job['version']))
        return self._response(job)
