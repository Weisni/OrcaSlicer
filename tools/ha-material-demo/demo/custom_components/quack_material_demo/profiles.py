"""Versioned material profile content, independent of the schema-8 inventory."""
import hashlib
import json
import re


MAX_PROFILE_BYTES = 262144
PROFILE_KEYS = {'schema_version', 'name', 'material_type', 'settings', 'dependencies', 'sha256'}
DEPENDENCY_KEYS = {'inherits', 'filament_id', 'vendor'}


def _text(value, maximum, allow_empty=False):
    if (not isinstance(value, str) or '\0' in value or len(value.encode('utf-8')) > maximum
            or not allow_empty and not value.strip()):
        raise ValueError('Invalid material profile text')


def _sha(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value):
        raise ValueError('A material profile SHA-256 is required')


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False)


def validate_profile(value):
    if not isinstance(value, dict) or set(value) != PROFILE_KEYS:
        raise ValueError('Unsupported material profile envelope')
    if type(value['schema_version']) is not int or value['schema_version'] != 1:
        raise ValueError('Unsupported material profile schema')
    _text(value['name'], 256)
    _text(value['material_type'], 40)
    _sha(value['sha256'])
    settings = value['settings']
    if not isinstance(settings, dict) or not 1 <= len(settings) <= 2000:
        raise ValueError('Invalid material settings')
    for key, setting in settings.items():
        if not isinstance(key, str) or not re.fullmatch(r'[a-zA-Z][a-zA-Z0-9_]{0,127}', key):
            raise ValueError('Invalid material setting key')
        if key in ('inherits', 'filament_settings_id'):
            raise ValueError('Profile identity belongs outside flattened settings')
        _text(setting, 65536, True)
    if settings.get('filament_type') not in (value['material_type'], json.dumps(value['material_type'], ensure_ascii=False)):
        raise ValueError('Profile material type contradicts its settings')
    dependencies = value['dependencies']
    if not isinstance(dependencies, dict) or set(dependencies) != DEPENDENCY_KEYS:
        raise ValueError('Invalid material profile dependency metadata')
    for dependency in dependencies.values():
        _text(dependency, 256, True)
    if len(_canonical(value).encode()) > MAX_PROFILE_BYTES:
        raise ValueError('Material profile exceeds 256 KiB')
    content = {key: item for key, item in value.items() if key != 'sha256'}
    if hashlib.sha256(_canonical(content).encode()).hexdigest() != value['sha256']:
        raise ValueError('Material profile content does not match its SHA-256')
    return value


def apply_profile_change(db, ident, change, native):
    """Join the caller's roll/receipt transaction; never commit independently."""
    from .store import Conflict
    if 'material_profile' not in change:
        if 'expected_profile_sha256' in change:
            raise ValueError('A profile baseline requires a selected profile upload')
        detach_incompatible_profile(db, ident, native)
        return
    profile = validate_profile(change['material_profile'])
    if (change.get('fields', {}).get('filament_preset_id') != profile['name'] or
            native.get('filament_preset_id') != profile['name'] or
            native.get('material_type') != profile['material_type']):
        raise ValueError('Select the matching material profile association for this roll')
    if 'expected_profile_sha256' not in change:
        raise ValueError('The previous HA profile digest is required')
    expected = change['expected_profile_sha256']
    if expected is not None:
        _sha(expected)
    prior = db.execute('SELECT payload FROM material_profiles WHERE spool_uuid=?', (ident,)).fetchone()
    before = json.loads(prior['payload'])['sha256'] if prior else None
    if before != expected:
        raise Conflict('HA material profile changed; refresh before synchronizing')
    db.execute('INSERT INTO material_profiles VALUES (?,?) ON CONFLICT(spool_uuid) DO UPDATE SET payload=excluded.payload',
               (ident, _canonical(profile)))


def detach_incompatible_profile(db, ident, native):
    prior = db.execute('SELECT payload FROM material_profiles WHERE spool_uuid=?', (ident,)).fetchone()
    if not prior:
        return
    value = json.loads(prior['payload'])
    if value['name'] != native.get('filament_preset_id') or value['material_type'] != native.get('material_type'):
        db.execute('DELETE FROM material_profiles WHERE spool_uuid=?', (ident,))


def read_profile(db, ident, expected_sha256):
    from .store import Conflict
    _sha(expected_sha256)
    row = db.execute('SELECT payload FROM material_profiles WHERE spool_uuid=?', (ident,)).fetchone()
    if not row:
        raise Conflict('HA material profile is no longer available; refresh the roll')
    value = validate_profile(json.loads(row['payload']))
    if value['sha256'] != expected_sha256:
        raise Conflict('HA material profile changed; refresh before loading it')
    spool = db.execute('SELECT data FROM spools WHERE uuid=?', (ident,)).fetchone()
    metadata = json.loads(spool['data']) if spool else {}
    if metadata.get('material_preset') != value['name'] or metadata.get('material_type') != value['material_type']:
        raise Conflict('HA roll profile association changed; refresh before loading it')
    return value


def profile_summary(value):
    if value is None:
        return None
    validate_profile(value)
    return {key: value[key] for key in ('schema_version', 'name', 'material_type', 'dependencies', 'sha256')}


def attach_profiles(db, spools):
    by_id = {row['spool_uuid']: json.loads(row['payload'])
             for row in db.execute('SELECT spool_uuid,payload FROM material_profiles')}
    for spool in spools:
        value = by_id.get(spool['uuid'])
        if value is not None and (spool.get('material_preset', value['name']) != value['name'] or
                                  spool.get('material_type', value['material_type']) != value['material_type']):
            value = None
        spool['material_profile'] = value
