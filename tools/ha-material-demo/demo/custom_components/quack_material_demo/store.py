"""Persistent, isolated demo inventory. Never controls a printer."""
import json
import sqlite3
import uuid
import time
from threading import RLock
from contextlib import contextmanager, nullcontext
from datetime import datetime, timezone
from pathlib import Path

from .topology import SLOTS, enabled_slots, validate_deactivation


class Conflict(ValueError):
    """Stale revision, insufficient stock or contradictory operation."""


def weight(value):
    if type(value) is not int or not 0 <= value <= 10**10:
        raise ValueError('Weight must be nonnegative integral milligrams')
    return value


def now():
    return datetime.now(timezone.utc).isoformat()


from .lifecycle import Lifecycle
from .native_bridge import NativeBridge
from .pilot import Pilot
from .explicit_sync import ExplicitSync
from .provider import Provider


class Store(Lifecycle, NativeBridge, Pilot, ExplicitSync, Provider):
    def __init__(self, path, seed_demo=True, settings=None):
        self.settings = settings or {}
        self.active_slots = enabled_slots(self.settings)
        # Only native read pages use this cache; write transactions always project
        # their own snapshot so staged changes cannot observe a cached old graph.
        self._native_page_lock = RLock()
        self._native_page_cache = None
        self.path = str(path)
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with self.connection() as db:
            db.executescript('''
                CREATE TABLE IF NOT EXISTS spools(uuid TEXT PRIMARY KEY, data TEXT NOT NULL,
                    remaining_mg INTEGER NOT NULL CHECK(remaining_mg >= 0));
                CREATE TABLE IF NOT EXISTS slots(id TEXT PRIMARY KEY,
                    spool_uuid TEXT UNIQUE REFERENCES spools(uuid), revision INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS jobs(uuid TEXT PRIMARY KEY, request_key TEXT UNIQUE NOT NULL,
                    name TEXT NOT NULL, state TEXT NOT NULL, allocations TEXT NOT NULL,
                    settlement TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, kind TEXT NOT NULL,
                    data TEXT NOT NULL, created_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS observations(provider TEXT PRIMARY KEY, state TEXT NOT NULL,
                    job_uuid TEXT REFERENCES jobs(uuid));
                CREATE TABLE IF NOT EXISTS joblinks(job_uuid TEXT PRIMARY KEY,order_uuid TEXT);
                CREATE TABLE IF NOT EXISTS bridge(id TEXT PRIMARY KEY, data TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS receipts(request_key TEXT PRIMARY KEY,payload TEXT NOT NULL,result TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS origins(job_uuid TEXT PRIMARY KEY REFERENCES jobs(uuid), source TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS provider_dispatch(job_uuid TEXT PRIMARY KEY REFERENCES jobs(uuid),
                    printer_id TEXT NOT NULL,status TEXT NOT NULL,print_name TEXT NOT NULL,
                    bindings TEXT NOT NULL,updated_at TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS material_profiles(spool_uuid TEXT PRIMARY KEY REFERENCES spools(uuid),
                    payload TEXT NOT NULL);
            ''')
            db.execute('BEGIN IMMEDIATE')
            for slot in SLOTS:
                db.execute('INSERT OR IGNORE INTO slots VALUES (?, NULL, 0)', (slot,))
            validate_deactivation(db, self.active_slots)
            if seed_demo and not db.execute('SELECT 1 FROM spools').fetchone():
                for index, (material, color) in enumerate((('PLA', '#367AF5'), ('PETG', '#FFFFFF'), ('PLA', '#367AF5'))):
                    ident = str(uuid.uuid5(uuid.NAMESPACE_URL, f'quack-ha-demo/spool/{index}'))
                    data = dict(uuid=ident, manufacturer='Demo manufacturer', product=f'Demo {material} roll {index + 1}',
                                material_type=material, color=color, diameter_mm=1.75,
                                density_g_cm3=1.24 if material == 'PLA' else 1.27,
                                material_preset=f'Generic {material} @BBL P2S', preset_revision='installed-local',
                                bambu_material=f'Bambu {material}', weight_quality='estimated', demo=True)
                    db.execute('INSERT INTO spools VALUES (?, ?, ?)', (ident, json.dumps(data), 800000))

    @contextmanager
    def connection(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA foreign_keys=ON')
        try:
            yield db
            db.commit()
        except BaseException:
            db.rollback()
            raise
        finally:
            db.close()

    def event(self, db, kind, data):
        db.execute('INSERT INTO events(kind,data,created_at) VALUES (?,?,?)', (kind, json.dumps(data), now()))

    def snapshot(self, db=None, *, provider=False):
        with self.connection() if db is None else nullcontext(db) as db:
            if not db.in_transaction:
                db.execute('BEGIN')
            job_query = ('SELECT allocations,settlement FROM jobs WHERE settlement IS NULL' if provider
                         else 'SELECT * FROM jobs ORDER BY created_at DESC')
            jobs = [dict(row) for row in db.execute(job_query)]
            reserved = {}
            for job in jobs:
                if not provider:
                    source = db.execute('SELECT source FROM origins WHERE job_uuid=?', (job['uuid'],)).fetchone()
                    job['source'] = source['source'] if source else 'simulation'
                    link=db.execute('SELECT order_uuid FROM joblinks WHERE job_uuid=?',(job['uuid'],)).fetchone()
                    job['customer_order_uuid']=link['order_uuid'] if link else None
                job['allocations'] = json.loads(job['allocations'])
                job['settlement'] = json.loads(job['settlement']) if job['settlement'] else None
                if job['settlement'] is None:
                    for allocation in job['allocations']:
                        key = allocation['spool_uuid']
                        reserved[key] = reserved.get(key, 0) + allocation['weight_mg']
            spools = []
            for row in db.execute('SELECT * FROM spools ORDER BY rowid'):
                data = json.loads(row['data'])
                data.update(remaining_mg=row['remaining_mg'], reserved_mg=reserved.get(row['uuid'], 0))
                data['available_mg'] = data['remaining_mg'] - data['reserved_mg']
                spools.append(data)
            slots = [dict(db.execute('SELECT * FROM slots WHERE id=?', (slot,)).fetchone()) for slot in self.active_slots]
            from .profiles import attach_profiles
            attach_profiles(db,spools)
            revision=self._revision(db)
            result = dict(schema_version=1, demo_mode=True, printer_id='duck-poop-demo', revision=revision,
                        provider_api_version=1,provider_printer_id=self.settings.get('provider_printer_id'),
                        captured_at=now(), captured_unix=int(time.time()), spools=spools, slots=slots)
            if provider:
                from .read_api import provider_view
                return provider_view(result)
            native=self.project_native(db,spools,jobs,revision)
            result.update(jobs=jobs, native_bundle=native, orders=native['tables']['customer_orders'] if native else [])
            return result

    def set_profile(self, spool_uuid, material_preset):
        if not isinstance(material_preset, str) or len(material_preset) > 256:
            raise ValueError('Provide a material preset name or leave it empty for standard selection')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT data FROM spools WHERE uuid=?', (spool_uuid,)).fetchone()
            if row is None:
                raise ValueError('Unknown spool')
            for job in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL'):
                if any(a['spool_uuid'] == spool_uuid for a in json.loads(job['allocations'])):
                    raise Conflict('Spool has an unsettled job')
            data = json.loads(row['data'])
            data['material_preset'] = material_preset.strip()
            data['profile_unresolved'] = not bool(data['material_preset'])
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(data), spool_uuid))
            db.execute('UPDATE slots SET revision=revision+1 WHERE spool_uuid=?', (spool_uuid,))
            self.event(db, 'profile', dict(spool_uuid=spool_uuid, material_preset=material_preset))
            return dict(spool_uuid=spool_uuid, material_preset=material_preset)

    def observe_printer(self, state, name, provider='duck-poop'):
        """Log printer attempts; settle estimates only for an exactly correlated dispatch."""
        state = str(state).lower()
        name = str(name)[:256] or 'Observed printer job'
        if state not in ('prepare', 'running', 'pause', 'finish', 'failed', 'idle'):
            return  # unavailable/unknown is not a terminal state.
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            previous = db.execute('SELECT * FROM observations WHERE provider=?', (provider,)).fetchone()
            job_uuid = previous['job_uuid'] if previous else None
            active=state in ('prepare','running','pause')
            pending=db.execute("SELECT * FROM provider_dispatch WHERE status IN ('prepared','accepted','uncertain')").fetchall() if provider=='duck-poop' else []
            exact=[intent for intent in pending if intent['print_name']==name]
            prior_active=previous and previous['state'] in ('prepare','running','pause')
            # Bambu's name sensor may lag PREPARE. Wait for its update instead
            # of creating a second anonymous job that strands the reservation.
            if active or job_uuid is None and prior_active and pending:
                if job_uuid is None:
                    if len(exact)==1:
                        job_uuid=exact[0]['job_uuid']
                        db.execute("UPDATE provider_dispatch SET status='observing',updated_at=? WHERE job_uuid=?",(now(),job_uuid))
                        self.event(db,'observed_provider_start',dict(uuid=job_uuid,provider=provider,identity_quality='dispatch_and_exact_name'))
                    elif not pending or not active:
                        job_uuid = str(uuid.uuid4())
                        db.execute('INSERT INTO jobs VALUES (?,?,?,?,?,NULL,?,?)',
                                   (job_uuid, 'observed:' + job_uuid, name, 'printing', '[]', now(), now()))
                        db.execute('INSERT INTO origins VALUES (?,?)', (job_uuid, 'printer_observation'))
                        self.event(db, 'observed_start', dict(uuid=job_uuid, provider=provider, initial_state=state,
                                   identity_quality='lifecycle_only', allocations_known=False))
                if active and job_uuid is not None:
                    job_state = 'paused' if state == 'pause' else 'printing'
                    changed = db.execute('UPDATE jobs SET state=?,updated_at=? WHERE uuid=? AND settlement IS NULL AND state!=?',
                                         (job_state, now(), job_uuid, job_state)).rowcount
                    if changed:
                        self.event(db, 'observed_state', dict(uuid=job_uuid, provider=provider, state=job_state))
            if not active and job_uuid is not None:
                db.execute('UPDATE jobs SET state=?,updated_at=? WHERE uuid=? AND settlement IS NULL', ('needs_review', now(), job_uuid))
                self.event(db, 'observed_terminal', dict(uuid=job_uuid, provider=provider, outcome=state,
                           consumption_quality='unknown'))
                if state=='finish':self._settle_provider_finish(db,job_uuid,name)
                db.execute("UPDATE provider_dispatch SET status='terminal',updated_at=? WHERE job_uuid=?",(now(),job_uuid))
                job_uuid = None
            db.execute('INSERT INTO observations VALUES (?,?,?) ON CONFLICT(provider) DO UPDATE SET state=excluded.state,job_uuid=excluded.job_uuid',
                       (provider, state, job_uuid))

    def assign(self, slot, spool_uuid, revision):
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            return self.assign_in_transaction(db, slot, spool_uuid, revision)

    def assign_in_transaction(self, db, slot, spool_uuid, revision, validate_only=False):
        if slot not in self.active_slots or type(revision) is not int:
            raise ValueError('Invalid slot or revision')
        with nullcontext(db):
            current = db.execute('SELECT * FROM slots WHERE id=?', (slot,)).fetchone()
            if current['revision'] != revision:
                raise Conflict('Assignment changed; refresh before retrying')
            if spool_uuid is not None and not db.execute('SELECT 1 FROM spools WHERE uuid=?', (spool_uuid,)).fetchone():
                raise Conflict('Unknown roll UUID; register the roll before assigning')
            if spool_uuid is not None:
                roll=db.execute('SELECT data,remaining_mg FROM spools WHERE uuid=?',(spool_uuid,)).fetchone()
                if json.loads(roll['data']).get('status')=='archived': raise Conflict('Archived roll UUID; restore it explicitly before assigning')
                if roll['remaining_mg']==0: raise Conflict('Roll is empty; select another roll or reconcile weight')
            for job in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL'):
                if any(a['slot'] == slot for a in json.loads(job['allocations'])):
                    raise Conflict('Slot has an unsettled job; reconcile it first')
            if spool_uuid is not None and db.execute('SELECT 1 FROM slots WHERE spool_uuid=? AND id<>?', (spool_uuid, slot)).fetchone():
                raise Conflict('Spool is already assigned to another slot')
            if validate_only:
                return
            try:
                db.execute('UPDATE slots SET spool_uuid=?,revision=revision+1 WHERE id=?', (spool_uuid, slot))
            except sqlite3.IntegrityError as error:
                raise Conflict('Spool is already assigned to another slot') from error
            self.event(db, 'assignment', dict(slot=slot, spool_uuid=spool_uuid, revision=revision + 1))
            return dict(slot=slot, spool_uuid=spool_uuid, revision=revision + 1,
                        printer_write='simulated', device_command_sent=False)

    def start_job(self, name, allocations, request_key, customer_order_uuid=None):
        if not isinstance(name, str) or not name.strip() or len(name) > 256:
            raise ValueError('A short job name is required')
        if not isinstance(request_key, str) or not request_key or len(request_key) > 128:
            raise ValueError('A unique request key is required')
        if not isinstance(allocations, list) or len(allocations) > len(self.active_slots):
            raise ValueError('Provide at most one allocation per enabled slot')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            if customer_order_uuid:
                bridge=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
                orders=json.loads(bridge['data'])['tables']['customer_orders'] if bridge else []
                if not any(o['id']==customer_order_uuid and not o.get('archived') for o in orders): raise ValueError('Unknown or archived customer order')
            existing = db.execute('SELECT * FROM jobs WHERE request_key=?', (request_key,)).fetchone()
            if existing:
                previous = json.loads(existing['allocations'])
                request = [{'slot': a['slot'], 'weight_mg': a['weight_mg']} for a in previous]
                link=db.execute('SELECT order_uuid FROM joblinks WHERE job_uuid=?',(existing['uuid'],)).fetchone()
                prior_order=link['order_uuid'] if link else None
                if name != existing['name'] or allocations != request or prior_order!=customer_order_uuid:
                    raise Conflict('Request key was already used for different job data')
                return dict(uuid=existing['uuid'], state=existing['state'])
            mapped = []
            seen = set()
            for allocation in allocations:
                slot = allocation['slot']
                if slot not in self.active_slots:raise ValueError('Printer slot is disabled')
                amount = weight(allocation['weight_mg'])
                if slot in seen:
                    raise ValueError('Duplicate slot allocation')
                seen.add(slot)
                row = db.execute('SELECT * FROM slots WHERE id=?', (slot,)).fetchone()
                if row is None or row['spool_uuid'] is None:
                    raise Conflict('Slot is not assigned')
                stock = db.execute('SELECT * FROM spools WHERE uuid=?', (row['spool_uuid'],)).fetchone()
                reserved = sum(a['weight_mg'] for job in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL')
                               for a in json.loads(job['allocations']) if a['spool_uuid'] == row['spool_uuid'])
                if amount > stock['remaining_mg'] - reserved:
                    raise Conflict('Insufficient available filament')
                data = json.loads(stock['data'])
                if data.get('status')=='archived': raise Conflict('Archived roll cannot be used')
                mapped.append(dict(slot=slot, spool_uuid=row['spool_uuid'], binding_revision=row['revision'],
                                   weight_mg=amount, material_preset=data['material_preset'], color=data['color']))
            ident = str(uuid.uuid4())
            db.execute('INSERT INTO jobs VALUES (?,?,?,?,?,NULL,?,?)',
                       (ident, request_key, name, 'printing', json.dumps(mapped), now(), now()))
            if customer_order_uuid: db.execute('INSERT INTO joblinks VALUES (?,?)',(ident,customer_order_uuid))
            self.event(db, 'job_start', dict(uuid=ident, allocations=mapped))
            return dict(uuid=ident, state='printing')

    def finish_job(self, job_uuid, outcome, consumption, quality):
        if outcome not in ('completed', 'failed') or quality not in ('estimated', 'measured', 'unknown'):
            raise ValueError('Invalid outcome or measurement quality')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            job = db.execute('SELECT * FROM jobs WHERE uuid=?', (job_uuid,)).fetchone()
            if job is None:
                raise ValueError('Unknown job UUID')
            allocations = json.loads(job['allocations'])
            if consumption is None and (outcome == 'failed' or quality == 'unknown' or not allocations):
                if job['settlement']:
                    raise Conflict('Job was already settled')
                db.execute('UPDATE jobs SET state=?,updated_at=? WHERE uuid=?', ('needs_review', now(), job_uuid))
                self.event(db, 'job_review', dict(uuid=job_uuid, outcome=outcome))
                return dict(uuid=job_uuid, state='needs_review')
            if consumption is None:
                consumption = {a['spool_uuid']: a['weight_mg'] for a in allocations}
                quality = 'estimated'
            if quality == 'unknown' or not isinstance(consumption, dict):
                raise ValueError('Settlement requires known quantities and provenance')
            keys = {a['spool_uuid'] for a in allocations}
            if set(consumption) != keys or not keys:
                raise ValueError('Consumption must cover exactly the allocated spools')
            consumption = {key: weight(value) for key, value in consumption.items()}
            settlement = json.dumps(dict(outcome=outcome, quality=quality, consumption=consumption), sort_keys=True)
            if job['settlement']:
                if settlement != job['settlement']:
                    raise Conflict('Job was already settled with different quantities')
                return dict(uuid=job_uuid, state=job['state'])
            for key, value in consumption.items():
                row = db.execute('SELECT remaining_mg FROM spools WHERE uuid=?', (key,)).fetchone()
                if value > row['remaining_mg']:
                    raise Conflict('Consumption exceeds remaining stock; reconcile measurement')
                other_reserved = sum(a['weight_mg'] for other in db.execute(
                    'SELECT allocations FROM jobs WHERE settlement IS NULL AND uuid<>?', (job_uuid,))
                    for a in json.loads(other['allocations']) if a['spool_uuid'] == key)
                if value > row['remaining_mg'] - other_reserved:
                    raise Conflict('Consumption conflicts with other reservations; reconcile before settling')
                db.execute('UPDATE spools SET remaining_mg=remaining_mg-? WHERE uuid=?', (value, key))
                material=db.execute('SELECT data,remaining_mg FROM spools WHERE uuid=?',(key,)).fetchone()
                metadata=json.loads(material['data']); metadata['weight_quality']=quality
                if material['remaining_mg']==0: metadata['status']='empty'
                db.execute('UPDATE spools SET data=? WHERE uuid=?',(json.dumps(metadata),key))
            db.execute('UPDATE jobs SET state=?,settlement=?,updated_at=? WHERE uuid=?',
                       (outcome, settlement, now(), job_uuid))
            self.event(db, 'job_settlement', dict(uuid=job_uuid, settlement=json.loads(settlement)))
            return dict(uuid=job_uuid, state=outcome)

    def dispatch(self, action, data):
        if not isinstance(data, dict):
            raise ValueError('Request must be an object')
        if action == 'customer': return self.manage_record('customer',data)
        if action == 'order': return self.manage_record('order',data) if self.settings.get('mode')=='pilot' else self.order_action(data)
        if action in ('job_order','create_invoice'):
            from .accounting import job_order, create_invoice
            return (job_order if action=='job_order' else create_invoice)(self,data)
        if action == 'reconcile_observed': return self.reconcile_observed(data)
        if action == 'reconcile_provider': return self.reconcile_provider(data)
        if action == 'label':
            import io
            import qrcode
            import qrcode.image.svg
            spool=next((s for s in self.snapshot()['spools'] if s['uuid']==data['spool_uuid']),None)
            if spool is None: raise ValueError('Unknown roll UUID')
            target=data.get('target','web');payload=self.label_payload(spool['uuid'],target)
            svg=io.BytesIO(); qrcode.make(payload,image_factory=qrcode.image.svg.SvgPathImage).save(svg)
            return dict(uuid=spool['uuid'],payload=payload,target=target,native_payload='quackslicer://spool/'+spool['uuid'],svg=svg.getvalue().decode(),status=spool.get('status','active'))
        if action in ('create','archive','restore','weigh','edit'):
            return self.lifecycle(action,data)
        if action == 'native_sync': return self.sync_native(data)
        if action == 'native_apply': return self.apply_native(data)
        if action == 'provider_apply': return self.provider_apply(data)
        if action == 'provider_delta': return self.provider_delta(data)
        if action == 'provider_job': return self.provider_job(data)
        if action == 'profile':
            return self.set_profile(data['spool_uuid'], data['material_preset'])
        if action == 'assign':
            return self.assign(data['slot'], data.get('spool_uuid'), data['revision'])
        if action == 'start':
            return self.start_job(data['name'], data.get('allocations', []), data['request_key'], data.get('customer_order_uuid'))
        if action == 'finish':
            return self.finish_job(data['job_uuid'], data['outcome'], data.get('consumption'), data.get('quality', 'unknown'))
        raise ValueError('Unknown demo action')
