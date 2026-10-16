"""Configurable subset of the verified single-printer P2S slot topology."""
import json

SLOTS = ('A1', 'A2', 'A3', 'A4', 'HT1', 'EXT')


def enabled_slots(settings):
    selected = settings.get('enabled_slots', list(SLOTS))
    if (not isinstance(selected, list) or not selected
            or any(not isinstance(slot, str) or slot not in SLOTS for slot in selected)
            or len(set(selected)) != len(selected)):
        raise ValueError('enabled_slots must be a nonempty unique subset of A1-A4, HT1 and EXT')
    return tuple(slot for slot in SLOTS if slot in selected)


def validate_deactivation(db, active):
    """Keep all ledger rows; never hide occupied or unresolved physical slots."""
    disabled = set(SLOTS) - set(active)
    if not disabled:return
    if any(row['id'] in disabled for row in db.execute('SELECT id FROM slots WHERE spool_uuid IS NOT NULL')):
        raise ValueError('Unassign occupied slots before disabling them')
    if any(a.get('slot') in disabled for row in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL')
           for a in json.loads(row['allocations'])):
        raise ValueError('Settle reserved slot jobs before disabling them')
    tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    if 'printer_assignments' in tables:
        for row in db.execute('SELECT operation FROM printer_assignments'):
            operation = json.loads(row['operation'])
            if operation.get('slot') in disabled and operation.get('status') == 'pending':
                raise ValueError('Resolve pending printer assignments before disabling their slots')
    if 'printer_metadata_sync' in tables:
        for row in db.execute('SELECT slot,operation FROM printer_metadata_sync'):
            if row['slot'] in disabled and json.loads(row['operation']).get('status') in ('pending', 'uncertain'):
                raise ValueError('Resolve uncertain printer metadata before disabling its slot')
