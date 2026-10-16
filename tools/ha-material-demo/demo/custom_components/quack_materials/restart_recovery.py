"""Durable restart reconciliation without printer commands or invented consumption.

Call ``ingest`` before every printer observation. HA adapters must provide fresh
status/name report timestamps and explicitly flag restored state. Decisions use
the existing ledger revision and receipt table, so a repeated confirmation cannot
book stock twice. Review records contain no credentials.
"""
import json
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
from zoneinfo import ZoneInfo

ACTIVE = ('prepare', 'running', 'pause')
TERMINAL = ('finish', 'failed', 'idle')
UNKNOWN = ('', 'none', 'unknown', 'unavailable')


def timestamp(value):
    try:
        result = value if isinstance(value, datetime) else datetime.fromisoformat(value.replace('Z', '+00:00'))
        return result.astimezone(timezone.utc) if result.tzinfo is not None else None
    except (ValueError, TypeError, AttributeError):
        return None


def numeric(value, maximum):
    if isinstance(value, bool):
        return None
    try:
        result = Decimal(str(value))
        return float(result) if result.is_finite() and 0 <= result <= maximum else None
    except (ValueError, TypeError, InvalidOperation):
        return None


class RestartRecovery:
    """Gate one printer's observations until its persisted job is identified."""

    def __init__(self, store, boot_at=None, provider='duck-poop'):
        self.store = store
        self.provider = provider
        self.boot_at = timestamp(boot_at or datetime.now(timezone.utc))
        if self.boot_at is None:
            raise ValueError('Restart recovery requires an aware boot timestamp')
        self._resumed = set()
        self.resume_window = store.settings.get('restart_resume_window_seconds', 0)
        self.unique_daily = store.settings.get('restart_resume_unique_daily', False)
        if type(self.resume_window) is not int or not 0 <= self.resume_window <= 180:
            raise ValueError('Restart continuation window must be 0..180 seconds')
        if type(self.unique_daily) is not bool:
            raise ValueError('Daily restart continuation must be boolean')
        self.timezone = ZoneInfo(store.settings.get('restart_timezone', 'UTC'))
        with store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            jobs = db.execute('''SELECT DISTINCT j.* FROM jobs j
                LEFT JOIN observations o ON o.job_uuid=j.uuid AND o.provider=?
                LEFT JOIN provider_dispatch d ON d.job_uuid=j.uuid
                WHERE j.settlement IS NULL AND (o.job_uuid IS NOT NULL
                    OR (?='duck-poop' AND d.status IN ('observing','prepared','accepted','uncertain')))''', (provider, provider)).fetchall()
            jobs = {job['uuid']:job for job in jobs}
            for prior in self._records(db):
                if prior['pending'] and prior.get('blocks_observation', True):
                    job = db.execute('SELECT * FROM jobs WHERE uuid=? AND settlement IS NULL', (prior['job_uuid'],)).fetchone()
                    if job is not None:
                        jobs[job['uuid']] = job
            for job in jobs.values():
                record = self._load(db, job['uuid']) or self._new_record(db, job)
                record['baseline'].update(self._new_record(db, job)['baseline'])
                self._clean_initial_layers(record['baseline'])
                record.update(pending=True, session_boot_at=self.boot_at.isoformat(),
                              reason='restart_waiting_fresh', observed=None)
                self._save(db, record)

    def _key(self, ident):
        return 'restart_review:' + self.provider + ':' + ident

    def _load(self, db, ident):
        row = db.execute('SELECT data FROM bridge WHERE id=?', (self._key(ident),)).fetchone()
        return json.loads(row['data']) if row else None

    def _new_record(self, db, job):
        intent = db.execute('SELECT print_name FROM provider_dispatch WHERE job_uuid=?', (job['uuid'],)).fetchone()
        progress = db.execute("SELECT data FROM events WHERE kind='observed_progress' AND json_extract(data,'$.uuid')=? ORDER BY id DESC LIMIT 1", (job['uuid'],)).fetchone()
        baseline = json.loads(progress['data']) if progress else {}
        baseline.pop('uuid', None)
        self._clean_initial_layers(baseline)
        return dict(job_uuid=job['uuid'], provider=self.provider,
                    name=intent['print_name'] if intent else job['name'],
                    baseline=baseline, requires_confirmation=False, pending=True, blocks_observation=True,
                    session_boot_at=self.boot_at.isoformat(), reason='restart_waiting_fresh', observed=None)

    def _save(self, db, record):
        old = self._load(db, record['job_uuid'])
        if old == record:
            return
        db.execute('INSERT INTO bridge VALUES (?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data',
                   (self._key(record['job_uuid']), json.dumps(record, sort_keys=True)))
        def decision(record):
            # Persist report freshness without invalidating an unchanged decision.
            result = dict(record)
            result.pop('last_active_report_at', None)
            if result.get('observed') is not None:
                result['observed'] = {key:value for key,value in result['observed'].items()
                                      if key not in ('status_reported_at', 'name_reported_at')}
            return result
        if old is not None and decision(old) == decision(record):
            return
        self.store.event(db, 'restart_review_updated' if old else 'restart_review_opened',
                         dict(job_uuid=record['job_uuid'], provider=self.provider,
                              reason=record['reason'], pending=record['pending']))

    def _records(self, db):
        return [json.loads(row['data']) for row in db.execute('SELECT data FROM bridge WHERE id LIKE ?',
                 ('restart_review:' + self.provider + ':%',))]

    def reviews(self):
        """Pending records for HA notifications and explicit decision controls."""
        with self.store.connection() as db:
            result = []
            for record in self._records(db):
                job = db.execute('SELECT settlement FROM jobs WHERE uuid=?', (record['job_uuid'],)).fetchone()
                if record['pending'] and job and job['settlement'] is None:
                    result.append({**record, 'revision':self.store._revision(db),
                        'notification_id':'quack_materials_restart_' + record['job_uuid']})
            return result

    def known_notification_ids(self):
        """Include resolved durable reviews so restored HA notices can be dismissed."""
        with self.store.connection() as db:
            return ['quack_materials_restart_' + r['job_uuid'] for r in self._records(db)]

    def _progress(self, telemetry):
        result = {}
        for field, maximum, when in (('progress_percent', 100, 'progress_observed_at'),
                                     ('current_layer', 1000000, 'layer_observed_at')):
            observed_at = timestamp(telemetry.get(when))
            value = numeric(telemetry.get(field), maximum)
            if value is not None and observed_at is not None and observed_at >= self.boot_at:
                result[field] = value
        total = numeric(telemetry.get('total_layers'), 1000000)
        if 'current_layer' in result and total and result['current_layer'] <= total:
            result['total_layers'] = total
        elif 'current_layer' in result:
            result.pop('current_layer')
        self._clean_initial_layers(result)
        ident = telemetry.get('printer_job_id')
        identity_at = timestamp(telemetry.get('identity_observed_at'))
        if isinstance(ident, str) and ident.strip().lower() not in UNKNOWN + ('0',) and identity_at and identity_at >= self.boot_at:
            result['printer_job_id'] = ident.strip()[:256]
        return result

    @staticmethod
    def _clean_initial_layers(progress):
        # Bambu entities update separately at PREPARE: a new zero percent can
        # briefly coexist with the previous attempt's final layer.
        if progress.get('progress_percent') == 0:
            progress.pop('current_layer', None)
            progress.pop('total_layers', None)

    def _continuity(self, db, record, observed):
        """User-selected inference policy; this is not a printer-issued task ID."""
        if record['requires_confirmation'] or observed['state'] not in ACTIVE + ('finish',):
            return None
        previous = timestamp(record.get('last_active_report_at'))
        current = timestamp(observed['status_reported_at'])
        if not previous or not current or current < previous:
            return None
        active = db.execute('SELECT state,job_uuid FROM observations WHERE provider=?', (self.provider,)).fetchone()
        dispatch = db.execute('SELECT status FROM provider_dispatch WHERE job_uuid=?', (record['job_uuid'],)).fetchone()
        if not active or active['job_uuid'] != record['job_uuid'] or active['state'] not in ACTIVE:
            return None
        if not dispatch or dispatch['status'] != 'observing':
            return None
        # Require a fresh usable progress sample as well as matching state/name.
        if not any(k in observed['progress'] for k in ('progress_percent', 'current_layer')):
            return None
        gap = (current - previous).total_seconds()
        if self.resume_window and gap <= self.resume_window and observed['state'] in ACTIVE:
            return 'short_restart_checkpoint'
        if not self.unique_daily:
            return None
        start = db.execute("SELECT created_at FROM events WHERE kind='observed_provider_start' AND json_extract(data,'$.uuid')=? ORDER BY id LIMIT 1", (record['job_uuid'],)).fetchone()
        started = timestamp(start['created_at']) if start else None
        if not started:
            return None
        first_day = started.astimezone(self.timezone).date()
        last_day = current.astimezone(self.timezone).date()
        attempts = set()
        for row in db.execute('''SELECT j.uuid,j.created_at,j.updated_at,j.settlement,d.print_name,
                (SELECT e.created_at FROM events e WHERE e.kind='observed_provider_start'
                 AND json_extract(e.data,'$.uuid')=j.uuid ORDER BY e.id LIMIT 1) started_at
                FROM jobs j LEFT JOIN provider_dispatch d ON d.job_uuid=j.uuid
                WHERE d.print_name=? OR j.name=?''', (record['name'], record['name'])):
            when = timestamp(row['started_at'] or row['created_at'])
            if when is None or first_day <= when.astimezone(self.timezone).date() <= last_day:
                ended = timestamp(row['updated_at'])
                # Earlier accounted attempts have a closed timeline. They are
                # compared, but cannot be the print that was active at shutdown.
                if row['uuid'] != record['job_uuid'] and row['settlement'] and ended and ended < started:
                    continue
                attempts.add(row['uuid'])
        return 'unique_daily_attempt' if attempts == {record['job_uuid']} else None

    @staticmethod
    def _identity_reason(record, observed, check_progress=True):
        if observed['name'] != record['name']:
            return 'name_mismatch'
        current = observed['progress']; prior = record['baseline']
        if prior.get('printer_job_id') and current.get('printer_job_id') and prior['printer_job_id'] != current['printer_job_id']:
            return 'printer_job_changed'
        if ('total_layers' in prior and 'total_layers' in current
                and prior['total_layers'] != current['total_layers']):
            return 'layer_count_changed'
        if not check_progress:
            return None
        for field in ('progress_percent', 'current_layer'):
            if field in prior and field in current and current[field] < prior[field]:
                return 'progress_regressed'
        if ('current_layer' in prior and 'current_layer' in current
                and prior.get('total_layers') != current.get('total_layers')):
            return 'layer_count_changed'
        return None

    @staticmethod
    def _proven(record, observed):
        prior = record['baseline']; current = observed['progress']
        # Repeated plates can reuse both filename and a larger progress value.
        # Only the durable task identity can prove continuity across HA boots.
        return bool(prior.get('printer_job_id') and prior['printer_job_id'] != '0'
                    and prior['printer_job_id'] == current.get('printer_job_id'))

    def ingest(self, state, name, telemetry=None, *, status_reported_at=None,
               name_reported_at=None, status_restored=False, name_restored=False):
        """Return whether the ordinary observer may mutate the inventory ledger."""
        state = str(state).strip().lower(); name = str(name).strip()[:256]
        telemetry = telemetry if isinstance(telemetry, dict) else {}
        status_time = timestamp(status_reported_at); name_time = timestamp(name_reported_at)
        fresh = bool(status_time and name_time and status_time >= self.boot_at and name_time >= self.boot_at
                     and not status_restored and not name_restored
                     and state in ACTIVE + TERMINAL and name.lower() not in UNKNOWN)
        observed = dict(state=state, name=name, fresh=fresh, progress=self._progress(telemetry),
                        status_reported_at=status_time.isoformat() if status_time else None,
                        name_reported_at=name_time.isoformat() if name_time else None)
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            records = [r for r in self._records(db) if r['pending'] and r.get('blocks_observation', True) and db.execute(
                'SELECT 1 FROM jobs WHERE uuid=? AND settlement IS NULL', (r['job_uuid'],)).fetchone()]
            observation = db.execute('SELECT job_uuid FROM observations WHERE provider=?', (self.provider,)).fetchone()
            ident = records[0]['job_uuid'] if records else observation['job_uuid'] if observation else None
            job = db.execute('SELECT * FROM jobs WHERE uuid=? AND settlement IS NULL', (ident,)).fetchone() if ident else None
            if job is None:
                # With no prior attempt, fresh ACTIVE starts through the ordinary
                # observer; idle reads never create recovery documents or events.
                # Existing detached quantity reviews still need the current
                # report to accept an explicit original-job declaration later.
                for other in self._records(db):
                    if other['pending'] and not other.get('blocks_observation', True):
                        other.update(observed=observed, session_boot_at=self.boot_at.isoformat())
                        self._save(db, other)
                return fresh
            record = self._load(db, ident)
            if record is None:
                record = self._new_record(db, job)
                # A job first seen during this HA session is already live. Future
                # restarts still need the durable identity/progress baseline.
                self._resumed.add(ident)
                record['pending'] = False
            previous = record.get('observed')
            prior_status = timestamp(previous.get('status_reported_at')) if previous else None
            prior_name = timestamp(previous.get('name_reported_at')) if previous else None
            if ((status_time and prior_status and status_time < prior_status)
                    or (name_time and prior_name and name_time < prior_name)):
                return False
            # All notices refer to the same printer report, so an explicit choice
            # can address a selected attempt even when several are unresolved.
            for other in self._records(db):
                if other['job_uuid'] != ident and other['pending']:
                    other['observed'] = observed
                    other['session_boot_at'] = self.boot_at.isoformat()
                    self._save(db, other)
            record['observed'] = observed
            reason = None
            quality = None
            if not fresh:
                reason = 'awaiting_fresh_state'
            elif len(records) > 1:
                reason = 'multiple_unresolved_jobs'
            else:
                quality = self._continuity(db, record, observed)
                reason = self._identity_reason(record, observed,
                    check_progress=state != 'prepare' and not (
                        ident in self._resumed and state in ('finish','failed')
                        or state == 'finish' and quality == 'unique_daily_attempt'))
                if reason is None and state in ACTIVE:
                    if ident not in self._resumed and (record['requires_confirmation'] or not (self._proven(record, observed) or quality)):
                        reason = 'identity_unproven'
                elif reason is None and (ident not in self._resumed or state == 'idle'):
                    if not (state == 'finish' and quality == 'unique_daily_attempt'):
                        reason = 'terminal_during_restart' if state in ('finish', 'failed') else 'idle_without_outcome'
            if reason:
                record.update(pending=True, reason=reason)
                # A late progress sensor is still initialization. Contradictory
                # identities or terminal outcomes require a human decision even
                # if a later same-name print advances past the old progress.
                if fresh and reason != 'identity_unproven':
                    record['requires_confirmation'] = True
                if fresh:
                    self._resumed.discard(ident)
                self._save(db, record)
                return False
            self._resumed.add(ident)
            record.update(pending=False, reason='resumed')
            if quality:
                record['identity_quality'] = quality
            if state in ACTIVE:
                record['last_active_report_at'] = observed['status_reported_at']
                if state != 'prepare':
                    record['baseline'].update(observed['progress'])
                    self._clean_initial_layers(record['baseline'])
            self._save(db, record)
            return True

    def resolve(self, request):
        """Explicit revision-checked, idempotent decisions for the original job."""
        from .store import Conflict
        from .provider import canonical_request, digest, matches, stamp
        if not isinstance(request, dict) or set(request) != {'request_key','revision','confirmed','job_uuid','action'}:
            raise ValueError('Invalid restart confirmation')
        key = request['request_key']; ident = request['job_uuid']; action = request['action']
        if (not isinstance(key, str) or not key.strip() or len(key) > 128
                or type(request['revision']) is not int or request['confirmed'] is not True
                or action not in ('resume_same','completed','failed','keep_pending','different_job',
                                  'original_completed','original_failed')):
            raise ValueError('Confirm a restart decision with a fresh revision')
        fingerprint = canonical_request('restart_confirmation', request)
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            receipt = db.execute('SELECT * FROM receipts WHERE request_key=?', (key,)).fetchone()
            if receipt:
                if not matches(receipt['payload'], fingerprint):
                    raise Conflict('Restart confirmation key reused')
                return json.loads(receipt['result'])
            if request['revision'] != self.store._revision(db):
                raise Conflict('HA changed; reload before restart confirmation')
            record = self._load(db, ident)
            job = db.execute('SELECT * FROM jobs WHERE uuid=?', (ident,)).fetchone()
            if not record or not record['pending'] or not job or job['settlement'] is not None:
                raise Conflict('Restart review is no longer pending')
            observed = record.get('observed') or {}
            if action != 'keep_pending':
                if record['session_boot_at'] != self.boot_at.isoformat() or not observed.get('fresh'):
                    raise Conflict('Fresh matching printer identity is required')
                if action in ('resume_same','completed','failed'):
                    if observed.get('name') != record['name']:
                        raise Conflict('Fresh matching printer identity is required')
                    prior_id = record['baseline'].get('printer_job_id')
                    current_id = observed.get('progress', {}).get('printer_job_id')
                    if prior_id and current_id and prior_id != current_id:
                        raise Conflict('Printer reports a different print identity')
            if action == 'resume_same':
                if observed.get('state') not in ACTIVE:
                    raise Conflict('Only the currently active matching print can resume')
                self._resumed.add(ident)
                record.update(pending=False, requires_confirmation=False, reason='manually_resumed',
                              baseline={**record['baseline'], **observed.get('progress', {})},
                              last_active_report_at=observed['status_reported_at'])
                db.execute('INSERT INTO observations VALUES (?,?,?) ON CONFLICT(provider) DO UPDATE SET state=excluded.state,job_uuid=excluded.job_uuid',
                           (self.provider, observed['state'], ident))
            elif action == 'different_job':
                # Human confirmation detaches identity only. The old stock and
                # reservation remain intact until its outcome/quantity is known.
                db.execute("UPDATE jobs SET state='needs_review',updated_at=? WHERE uuid=?", (stamp(), ident))
                db.execute("UPDATE provider_dispatch SET status='review',updated_at=? WHERE job_uuid=?", (stamp(), ident))
                db.execute('UPDATE observations SET state=?,job_uuid=NULL WHERE provider=? AND job_uuid=?',
                           (observed['state'], self.provider, ident))
                record.update(blocks_observation=False, pending=True, reason='consumption_requires_review')
                self._resumed.discard(ident)
            elif action in ('completed','failed','original_completed','original_failed'):
                outcome = action.removeprefix('original_')
                explicit_original = action.startswith('original_')
                if not explicit_original and observed.get('state') != ('finish' if outcome == 'completed' else 'failed'):
                    raise Conflict('Printer terminal outcome does not match the decision')
                intent = db.execute('SELECT * FROM provider_dispatch WHERE job_uuid=?', (ident,)).fetchone()
                if intent and intent['print_name'] == record['name']:
                    estimate = self.store.cancelled_estimate(db, ident) if outcome == 'failed' else None
                    if outcome == 'completed' or estimate is not None:
                        self.store._settle_provider_finish(db, ident, record['name'], outcome=outcome,
                            estimate=estimate, dispatch_status=intent['status'])
                # This is the user's declaration about the ORIGINAL attempt,
                # never a claim that the current different printer job ended.
                self.store.event(db, 'observed_terminal', dict(uuid=ident, provider=self.provider,
                    outcome='finish' if outcome == 'completed' else 'failed', consumption_quality='unknown',
                    identity_quality='manual_restart_confirmation', declaration=action))
                db.execute("UPDATE provider_dispatch SET status='terminal',updated_at=? WHERE job_uuid=?", (stamp(), ident))
                db.execute('UPDATE observations SET job_uuid=NULL WHERE provider=? AND job_uuid=?', (self.provider, ident))
                record['blocks_observation'] = False
                settled = db.execute('SELECT settlement FROM jobs WHERE uuid=?', (ident,)).fetchone()['settlement']
                if settled is None:
                    db.execute("UPDATE jobs SET state='needs_review',updated_at=? WHERE uuid=?", (stamp(), ident))
                    record.update(reason='consumption_requires_review', requires_confirmation=True)
                else:
                    record.update(pending=False, reason='confirmed_' + outcome)
            self._save(db, record)
            self.store.event(db, 'restart_confirmation', dict(job_uuid=ident, action=action, pending=record['pending']))
            result = dict(accepted=True, job_uuid=ident, action=action, pending=record['pending'], revision=self.store._revision(db))
            db.execute('INSERT INTO receipts VALUES (?,?,?)', (key, digest(fingerprint), json.dumps(result)))
            return result
