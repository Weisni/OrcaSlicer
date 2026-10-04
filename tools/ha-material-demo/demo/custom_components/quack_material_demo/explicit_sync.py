"""Confirmed, selective schema-8 roll commands; never replace a client graph."""
import hashlib
import json
import math
import re
import uuid

from .native_bridge import TABLES, stamp, uid
from .receipts import canonical_request, digest, matches
from .profiles import apply_profile_change


FIELDS = frozenset(('name', 'manufacturer', 'material_type', 'filament_preset_id',
    'color_hex', 'nominal_capacity_mg', 'diameter_mm', 'density_g_cm3', 'warning_mode',
    'warning_value', 'material_price_per_kg_micros', 'price_currency', 'status'))
CANONICAL = dict(name='product', filament_preset_id='material_preset',
                 color_hex='color', nominal_capacity_mg='nominal_mg')
CHANGE_KEYS = frozenset(('spool_uuid', 'fields', 'expected', 'create', 'remaining_mg',
                         'expected_remaining_mg', 'quality', 'material_profile', 'expected_profile_sha256'))


def validate_fields(fields):
    """Validate selected values only; untouched legacy metadata stays intact."""
    from .store import weight
    if not isinstance(fields, dict) or set(fields) - FIELDS:
        raise ValueError('Unknown native spool field')
    for key, value in fields.items():
        if key in ('nominal_capacity_mg', 'material_price_per_kg_micros', 'warning_value'):
            weight(value)
            if key == 'nominal_capacity_mg' and not value:
                raise ValueError('Nominal capacity must be positive')
        elif key in ('diameter_mm', 'density_g_cm3'):
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError('Invalid ' + key)
            if value <= 0:
                raise ValueError('Invalid ' + key)
        elif key == 'status':
            if value not in ('active', 'empty', 'archived'): raise ValueError('Invalid status')
        elif key == 'warning_mode':
            if value not in ('none', 'grams', 'percent'): raise ValueError('Invalid warning mode')
        elif key == 'color_hex':
            if not isinstance(value, str) or not re.fullmatch(r'#[0-9a-fA-F]{6}', value):
                raise ValueError('Use #RRGGBB color')
        elif key == 'price_currency':
            if not isinstance(value, str) or not re.fullmatch(r'[A-Z]{3}', value):
                raise ValueError('Use a three-letter currency')
        elif not isinstance(value, str) or len(value) > (40 if key == 'material_type' else 256):
            raise ValueError('Invalid ' + key)
        elif key not in ('filament_preset_id', 'manufacturer') and not value.strip():
            raise ValueError('Provide ' + key)


class ExplicitSync:
    def apply_native(self, request):
        """Apply selected fields and receipt in one locked database transaction."""
        from .store import Conflict, weight
        if set(request) - {'revision', 'request_key', 'confirmed', 'changes', 'concurrency', 'response'}:
            raise ValueError('Unknown native apply request field')
        response = request.get('response', 'snapshot')
        if response not in ('snapshot', 'ack'):raise ValueError('Invalid native response format')
        concurrency = request.get('concurrency', 'revision')
        if concurrency not in ('revision', 'fields'):
            raise ValueError('Invalid concurrency mode')
        key = request.get('request_key')
        changes = request.get('changes')
        if request.get('confirmed') is not True:
            raise ValueError('Explicit confirmation is required')
        if not isinstance(key, str) or not key.strip() or len(key) > 128:
            raise ValueError('A unique request key is required')
        if type(request.get('revision')) is not int or request['revision'] < 0:
            raise ValueError('A nonnegative revision is required')
        if not isinstance(changes, list) or not 1 <= len(changes) <= 100:
            raise ValueError('Select between one and 100 rolls')
        try:
            fingerprint = canonical_request('native_apply', request)
        except (TypeError, ValueError) as error:
            raise ValueError('Invalid JSON request') from error
        if len(fingerprint.encode()) > 2000000:
            raise ValueError('Selected changes exceed size limit')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior = db.execute('SELECT * FROM receipts WHERE request_key=?', (key,)).fetchone()
            if prior:
                if not matches(prior['payload'], fingerprint): raise Conflict('Request key reused with different data')
                result = json.loads(prior['result'])
                if response == 'ack':
                    return dict(accepted=True, request_key=key, revision=self._revision(db),
                                accepted_revision=result.get('accepted_revision', result.get('revision')))
                return self.snapshot(db) if result.get('receipt_kind') == 'native_apply_v1' else result
            revision = self._revision(db)
            if request['revision'] > revision or (concurrency == 'revision' and request['revision'] != revision):
                raise Conflict('HA changed; refresh the comparison before confirming')
            # Start from authoritative HA rows, including independent observer history.
            if not db.execute("SELECT 1 FROM bridge WHERE id='native'").fetchone():
                empty = dict(schema_version=8, tables={table: [] for table in TABLES})
                db.execute("INSERT INTO bridge VALUES ('native',?)", (json.dumps(empty),))
            snapshot = self.snapshot(db)
            bundle = snapshot['native_bundle']
            tables = bundle['tables']
            byid = {row['id']: row for row in tables['spools']}
            seen = set()
            for change in changes:
                if not isinstance(change, dict) or set(change) - CHANGE_KEYS:
                    raise ValueError('Unknown selected change field')
                ident = change.get('spool_uuid')
                if not isinstance(ident, str): raise ValueError('A spool UUID is required')
                try:
                    parsed = uuid.UUID(ident)
                except (ValueError, AttributeError) as error:
                    raise ValueError('Invalid spool UUID') from error
                if str(parsed) != ident: raise ValueError('Use a canonical spool UUID')
                if ident in seen: raise ValueError('Duplicate selected spool UUID')
                seen.add(ident)
                fields, expected = change.get('fields'), change.get('expected')
                validate_fields(fields)
                if not isinstance(expected, dict) or set(expected) - FIELDS:
                    raise ValueError('Expected native fields are required')
                create = change.get('create', False)
                if type(create) is not bool: raise ValueError('Create must be boolean')
                stock_selected = 'remaining_mg' in change
                if not fields and not stock_selected: raise ValueError('No fields or stock selected')
                if stock_selected:
                    amount = weight(change['remaining_mg'])
                    if change.get('quality') not in ('measured', 'estimated'):
                        raise ValueError('Stock requires measured or estimated provenance')
                elif 'quality' in change or 'expected_remaining_mg' in change:
                    raise ValueError('Stock provenance requires an explicit stock change')
                row = db.execute('SELECT * FROM spools WHERE uuid=?', (ident,)).fetchone()
                if create:
                    if row or ident in byid: raise Conflict('Spool UUID is already registered')
                    if expected or 'expected_remaining_mg' in change:
                        raise ValueError('A new roll has no previous baseline')
                    required = {'name', 'manufacturer', 'material_type', 'color_hex'}
                    if not required <= set(fields): raise ValueError('New roll metadata is incomplete')
                    native = dict(id=ident, filament_preset_id='', nominal_capacity_mg=1000000,
                        diameter_mm=1.75, density_g_cm3=1.26, warning_mode='none', warning_value=0,
                        material_price_per_kg_micros=20000000, price_currency='EUR',
                        status='active' if stock_selected and amount else 'empty', created_at=stamp())
                    metadata = dict(uuid=ident, created_at=native['created_at'],
                        demo=self.settings.get('mode') != 'pilot', preset_revision='installed-local',
                        weight_quality=change.get('quality', 'estimated'))
                    old_amount = 0
                    amount = amount if stock_selected else 0
                else:
                    if not row or ident not in byid: raise Conflict('Unknown roll UUID; confirm creation explicitly')
                    if set(expected) != set(fields): raise ValueError('Every selected field needs its expected value')
                    native = byid[ident]
                    for name, value in expected.items():
                        if native.get(name) != value or isinstance(value, bool) != isinstance(native.get(name), bool):
                            raise Conflict('Selected roll field changed; refresh the comparison')
                    metadata = json.loads(row['data'])
                    old_amount = row['remaining_mg']
                    if stock_selected:
                        if 'expected_remaining_mg' not in change: raise ValueError('Expected stock is required')
                        if weight(change['expected_remaining_mg']) != old_amount:
                            raise Conflict('Stock changed; refresh the comparison')
                        self._assert_unreserved(db, ident)
                        if native['status'] == 'archived': raise Conflict('Restore archived roll before changing stock')
                    else:
                        amount = old_amount
                    if 'status' in fields and fields['status'] != native['status']:
                        self._assert_unreserved(db, ident)
                native.update(fields)
                if stock_selected and 'status' not in fields:
                    native['status'] = 'active' if amount else 'empty'
                lifecycle_changed = create or stock_selected or bool({'status', 'nominal_capacity_mg'} & set(fields))
                if lifecycle_changed and amount > native['nominal_capacity_mg']:
                    raise ValueError('Fill must fit nominal capacity')
                if lifecycle_changed and (native['status'] == 'empty' and amount or native['status'] == 'active' and not amount):
                    raise ValueError('Status contradicts remaining stock')
                # Native percent thresholds use basis points: 2000 means 20%.
                if native.get('warning_mode') == 'percent' and native.get('warning_value', 0) > 10000:
                    raise ValueError('Percent warning must be between zero and 10000 basis points')
                native['updated_at'] = stamp()
                # Only selected canonical fields are written for existing records.
                selected = native if create else fields
                for name, value in selected.items():
                    if name in FIELDS: metadata[CANONICAL.get(name, name)] = value
                if stock_selected:
                    metadata.update(status=native['status'], weight_quality=change['quality'])
                if create or 'material_type' in fields:
                    material = native['material_type']
                    metadata['bambu_material'] = 'Bambu ' + material if material in ('PLA', 'PETG') else None
                if create or 'filament_preset_id' in fields:
                    metadata['profile_unresolved'] = not bool(native['filament_preset_id'].strip())
                db.execute('INSERT INTO spools VALUES (?,?,?) ON CONFLICT(uuid) DO UPDATE SET data=excluded.data,remaining_mg=excluded.remaining_mg',
                    (ident, json.dumps(metadata), amount))
                apply_profile_change(db, ident, change, native)
                if create:
                    tables['spools'].append(native)
                    tables['spool_identifiers'].append(dict(kind='quack_ndef_uuid', value=ident,
                        spool_id=ident, created_at=native['created_at']))
                elif fields or stock_selected:
                    if native['status'] in ('archived', 'empty'):
                        db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?', (ident,))
                    else:
                        db.execute('UPDATE slots SET revision=revision+1 WHERE spool_uuid=?', (ident,))
                if stock_selected:
                    operation = 'ha-explicit:' + hashlib.sha256(key.encode()).hexdigest() + ':' + ident
                    tables['stock_events'].append(dict(id=uid(operation), spool_id=ident, job_id=None,
                        allocation_id=None, event_type='initial' if create else 'adjustment',
                        delta_mg=amount-old_amount, balance_after_mg=amount, operation_key=operation,
                        note='HA ' + change['quality'] + ' explicit stock ' + ('creation' if create else 'correction'),
                        created_at=stamp()))
            db.execute("UPDATE bridge SET data=? WHERE id='native'", (json.dumps(bundle),))
            self.event(db, 'native_apply', dict(request_key=key, spool_uuids=sorted(seen)))
            revision = self._revision(db)
            receipt = dict(receipt_kind='native_apply_v1', accepted_revision=revision)
            db.execute('INSERT INTO receipts VALUES (?,?,?)', (key, digest(fingerprint), json.dumps(receipt)))
            if response == 'ack':
                return dict(accepted=True, request_key=key, accepted_revision=revision, revision=revision)
            return self.snapshot(db)
