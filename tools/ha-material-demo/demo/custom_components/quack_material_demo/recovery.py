"""Credential-free, versioned inventory recovery bundles."""
import hashlib
import json
import shutil
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from .store import Store
from .topology import validate_deactivation

CORE_TABLES = ('spools','slots','jobs','events','observations','joblinks','bridge',
               'receipts','origins','provider_dispatch','material_profiles','material_profile_variants')
EXTRA_SCHEMAS = {
    'printer_assignments': 'CREATE TABLE printer_assignments (request_key TEXT PRIMARY KEY, request TEXT NOT NULL, operation TEXT NOT NULL)',
    'printer_metadata_sync': 'CREATE TABLE printer_metadata_sync (slot TEXT PRIMARY KEY, operation TEXT NOT NULL)',
}
MAX_BYTES = 64 * 1024 * 1024


def canonical(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False,allow_nan=False).encode('utf-8')


def checksum(tables):
    return hashlib.sha256(canonical(tables)).hexdigest()


def export_recovery(store):
    """Read one transaction; exclude credentials, HA config and unrelated files."""
    tables={}
    with store.connection() as db:
        db.execute('BEGIN')
        present={r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")}
        if present-set(CORE_TABLES)-set(EXTRA_SCHEMAS):
            raise ValueError('Update recovery support before exporting this database schema')
        for name in sorted(present):
            columns=[r[1] for r in db.execute('PRAGMA table_info('+name+')')]
            rows=[list(row) for row in db.execute('SELECT * FROM '+name+' ORDER BY rowid')]
            tables[name]={'columns':columns,'rows':rows}
    result={'format':'quack-ha-recovery','version':1,'created_at':datetime.now(timezone.utc).isoformat(),
        'tables':tables,'sha256':checksum(tables)}
    if len(canonical(result)) > MAX_BYTES:
        raise ValueError('Recovery export exceeds 64 MiB; use a consistent HA backup instead')
    return result


def restore_recovery(bundle, destination, settings=None):
    """Validate in isolation, then create a NEW database. Never replace live data."""
    destination=Path(destination)
    if destination.exists():raise FileExistsError('Restore destination already exists')
    if (not isinstance(bundle,dict) or set(bundle)!={'format','version','created_at','tables','sha256'}
            or bundle['format']!='quack-ha-recovery' or type(bundle['version']) is not int or bundle['version']!=1):
        raise ValueError('Unsupported recovery bundle')
    if len(canonical(bundle))>MAX_BYTES or not isinstance(bundle['tables'],dict):
        raise ValueError('Invalid recovery size or tables')
    tables=bundle['tables']
    if not set(CORE_TABLES)-{'material_profiles','material_profile_variants'} <= set(tables) or set(tables)-set(CORE_TABLES)-set(EXTRA_SCHEMAS):
        raise ValueError('Unsupported recovery tables')
    if checksum(tables)!=bundle['sha256']:raise ValueError('Recovery checksum mismatch')
    destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='quack-restore-',dir=destination.parent) as temporary:
        staged=Path(temporary)/'inventory.sqlite3'
        restored=Store(staged,seed_demo=False,settings=settings or {'mode':'pilot'})
        with restored.connection() as db:
            for name,statement in EXTRA_SCHEMAS.items():
                if name in tables:db.execute(statement)
            db.execute('BEGIN IMMEDIATE')
            db.execute('PRAGMA defer_foreign_keys=ON')
            # Restore SQL comes only from the installed schema and allowlisted
            # identifiers. Exported field values always use bound parameters.
            for name in tables:db.execute('DELETE FROM '+name)
            for name,table in tables.items():
                columns=[r[1] for r in db.execute('PRAGMA table_info('+name+')')]
                if not isinstance(table,dict) or set(table)!={'columns','rows'} or table['columns']!=columns or not isinstance(table['rows'],list):
                    raise ValueError('Recovery schema mismatch')
                for row in table['rows']:
                    if not isinstance(row,list) or len(row)!=len(columns) or any(type(v) not in (str,int,float,type(None)) for v in row):
                        raise ValueError('Invalid recovery row')
                db.executemany('INSERT INTO '+name+' VALUES ('+','.join('?' for _ in columns)+')',table['rows'])
            if db.execute('PRAGMA foreign_key_check').fetchone():raise ValueError('Recovery references are inconsistent')
            if db.execute('PRAGMA quick_check').fetchone()[0]!='ok':raise ValueError('Recovery database integrity failed')
            # Imported bindings and journals must fit the requested topology
            # before publishing even a new destination database.
            validate_deactivation(db, restored.active_slots)
        snapshot=restored.snapshot()
        if any(s['available_mg']<0 for s in snapshot['spools']):raise ValueError('Recovery reservations exceed stock')
        created=False
        try:
            with destination.open('xb') as output, staged.open('rb') as source:
                created=True
                shutil.copyfileobj(source,output)
        except BaseException:
            if created:destination.unlink()
            raise
    return Store(destination,seed_demo=False,settings=settings or {'mode':'pilot'})
