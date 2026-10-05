"""Bounded provider reads; clients stage all pages before replacing their cache."""
import copy
import json
import sys

from .native_bridge import TABLES
from .profiles import profile_summary, read_profile

CAPABILITIES = dict(native_apply_fields=True, provider_delta=True,
                    native_snapshot_pages=True, provider_materials_view=True, material_profiles_v1=True,
                    material_profile_variants_v1=True)
PAGE_BYTES = 192 * 1024
NATIVE_CACHE_BYTES = 64 * 1024 * 1024


def _within_cache_budget(bundle):
    """Count retained Python graph objects, not just compressed/serialized bytes."""
    pending, seen, size = [bundle], set(), 0
    while pending:
        value = pending.pop()
        ident = id(value)
        if ident in seen:continue
        seen.add(ident)
        size += sys.getsizeof(value)
        if size > NATIVE_CACHE_BYTES:return False
        if isinstance(value, dict):
            pending.extend(value.keys());pending.extend(value.values())
        elif isinstance(value, (list, tuple)):
            pending.extend(value)
    return True


def provider_view(snapshot):
    """Keep the material selection envelope independent of ledger history size."""
    result = {key: value for key, value in snapshot.items() if key not in ('jobs', 'native_bundle', 'orders')}
    result['spools'] = [dict(spool, material_profile=profile_summary(spool.get('material_profile')),
        material_profile_variants={key: profile_summary(value) for key, value in spool.get('material_profile_variants', {}).items()})
                        for spool in snapshot['spools']]
    result['capabilities'] = dict(CAPABILITIES)
    return result


def profile(store, query):
    if not {'spool_uuid', 'sha256'} <= set(query) or set(query) - {'spool_uuid', 'sha256', 'context'}:
        raise ValueError('Select a roll and its expected profile digest')
    with store.connection() as db:
        return read_profile(db, query['spool_uuid'], query['sha256'], query.get('context'))


def _integer(query, name, default=None):
    value = query.get(name, default)
    if value is None:return None
    if type(value) not in (str, int):raise ValueError('Invalid page parameter')
    try:result = int(value)
    except (ValueError, TypeError) as error:raise ValueError('Invalid page parameter') from error
    if result < 0:raise ValueError('Page parameters must be nonnegative')
    return result


def materials(store, query):
    from .store import Conflict
    if set(query) - {'view', 'revision'} or query.get('view', 'full') not in ('full', 'provider'):
        raise ValueError('Invalid materials view')
    revision = _integer(query, 'revision')
    result = store.snapshot(provider=query.get('view') == 'provider')
    if revision is not None and revision != result['revision']:
        raise Conflict('HA changed; restart the snapshot read')
    if query.get('view') == 'provider':
        return result
    result['capabilities'] = dict(CAPABILITIES)
    return result


def native_page(store, query):
    from .store import Conflict
    if set(query) - {'table', 'revision', 'cursor', 'limit'} or query.get('table') not in TABLES:
        raise ValueError('Invalid native table')
    table = query['table']
    revision = _integer(query, 'revision')
    cursor = _integer(query, 'cursor', 0)
    limit = _integer(query, 'limit', 200)
    if not 1 <= limit <= 200 or cursor and revision is None:
        raise ValueError('Continue pages with the original revision')
    with store._native_page_lock, store.connection() as db:
        db.execute('BEGIN')
        current = store._revision(db)
        if store._native_page_cache is not None and store._native_page_cache[0] != current:
            store._native_page_cache = None
        if revision is not None and revision != current:
            raise Conflict('HA changed; restart the snapshot read')
        if store._native_page_cache is None:
            bundle = store.snapshot(db)['native_bundle']
            if _within_cache_budget(bundle):
                store._native_page_cache = (current, bundle)
        else:
            bundle = store._native_page_cache[1]
        return _page(bundle, current, table, cursor, limit)


def _page(bundle, revision, table, cursor, limit):
    rows = bundle['tables'][table] if bundle else []
    if cursor > len(rows):raise ValueError('Cursor exceeds table')
    result = dict(schema_version=8, revision=revision, table=table,
                  rows=[], next_cursor=None, total=len(rows))
    # Reserve ample envelope space; the standard JSON encoding is an upper bound
    # for the serializer used by HA, including escaped non-ASCII characters.
    size = len(json.dumps(result).encode('utf-8')) + 64
    for row in rows[cursor:cursor + limit]:
        length = len(json.dumps(row).encode('utf-8')) + 2
        if size + length > PAGE_BYTES:
            if not result['rows']:raise ValueError('A native row exceeds the page limit')
            break
        result['rows'].append(copy.deepcopy(row))
        size += length
    following = cursor + len(result['rows'])
    result['next_cursor'] = following if following < len(rows) else None
    return result
