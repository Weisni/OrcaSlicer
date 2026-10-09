"""Bounded generation/revision reads captured in a single SQLite transaction."""
import json
import sys
import uuid

from .native_bridge import TABLES
from .profiles import published_profiles

MAX_REPLY_BYTES = 64 * 1024 * 1024
MAX_PROVIDER_BYTES = 4 * 1024 * 1024
MAX_ROW_BYTES = 256 * 1024
MAX_ROWS = 250000
HISTORY_BYTES = 64 * 1024 * 1024
HISTORY_REVISIONS = 8
IDENTITY_KEYS = {'spool_identifiers': ('kind', 'value'),
                 'job_identifiers': ('provider', 'kind', 'value'),
                 'print_job_manual_overrides': ('job_id',)}


def row_key(table, row):
    """Match HaInventoryTransport::row_key, including UTF-8 JSON encoding."""
    if not isinstance(row, dict) or not row:
        raise ValueError('Invalid native row')
    identity = []
    for key in IDENTITY_KEYS.get(table, ('id',)):
        value = row[key]
        if table == 'inventory_settings':
            if type(value) is not int or value != 1:
                raise ValueError('Invalid inventory settings identity')
        elif not isinstance(value, str) or not value:
            raise ValueError('A stable native row identity is required')
        identity.append(value)
    return json.dumps(identity, ensure_ascii=False, separators=(',', ':'), allow_nan=False)


def _index(bundle):
    if (not isinstance(bundle, dict) or bundle.get('schema_version') != 8 or
            not isinstance(bundle.get('tables'), dict) or set(bundle['tables']) != set(TABLES)):
        raise ValueError('Native synchronization requires schema 8')
    result, count = {}, 0
    for table in TABLES:
        rows = bundle['tables'][table]
        if not isinstance(rows, list):raise ValueError('Invalid native table')
        count += len(rows)
        if count > MAX_ROWS:raise ValueError('Native graph exceeds 250,000 rows')
        indexed = {}
        for row in rows:
            key = row_key(table, row)
            if key in indexed:raise ValueError('Duplicate native row identity')
            if len(json.dumps(row, allow_nan=False).encode('utf-8')) > MAX_ROW_BYTES:
                raise ValueError('Native row exceeds 256 KiB')
            indexed[key] = row
        result[table] = indexed
    if len(json.dumps(bundle, allow_nan=False).encode('utf-8')) > MAX_REPLY_BYTES:
        raise ValueError('Native graph exceeds 64 MiB')
    return result


def _retained_size(value):
    """Bound actual retained Python objects, independently of JSON reply size."""
    pending, seen, size = [value], set(), 0
    while pending:
        item = pending.pop()
        if id(item) in seen:continue
        seen.add(id(item))
        size += sys.getsizeof(item)
        if size > HISTORY_BYTES:return size
        if isinstance(item, dict):
            pending.extend(item.keys());pending.extend(item.values())
        elif isinstance(item, (list, tuple)):
            pending.extend(item)
    return size


def _trim_history(store):
    while store._sync_history and (len(store._sync_history) > HISTORY_REVISIONS or
                                  store._sync_history_bytes > HISTORY_BYTES):
        _, (_, size) = store._sync_history.popitem(last=False)
        store._sync_history_bytes -= size


def _delta(current, previous):
    tables = {}
    for table in TABLES:
        old = previous[table] if previous is not None else {}
        new = current[table]
        tables[table] = dict(upserts=[row for key, row in new.items() if old.get(key) != row],
                             deletes=[key for key in old if key not in new])
    return dict(schema_version=8, tables=tables)


def sync(store, query):
    from .read_api import _integer, provider_view
    if set(query) - {'generation', 'revision'}:
        raise ValueError('Invalid synchronization query')
    generation = query.get('generation')
    revision = _integer(query, 'revision')
    if generation is not None and (not isinstance(generation, str) or not 1 <= len(generation) <= 128 or revision is None):
        raise ValueError('A generation requires its revision')
    with store._sync_lock, store.connection() as db:
        db.execute('BEGIN')
        current = store._revision(db)
        if current < store._sync_revision:
            store._sync_generation = str(uuid.uuid4())
            store._sync_history.clear()
            store._sync_history_bytes = 0
        store._sync_revision = current
        _trim_history(store)
        # No graph scan or projection occurs here. Both revision reads and the
        # changed snapshot below observe the same SQLite read transaction.
        prior = store._sync_history.get(revision) if generation == store._sync_generation else None
        unchanged = generation == store._sync_generation and revision == current
        result = dict(sync_version=1, generation=store._sync_generation,
                      revision=current, unchanged=unchanged, reset=not unchanged and prior is None)
        if unchanged:return result
        snapshot = store.snapshot(db)
        bundle = snapshot['native_bundle'] or dict(schema_version=8, tables={table: [] for table in TABLES})
        indexed = _index(bundle)
        result['snapshot'] = provider_view(snapshot)
        result['snapshot']['profiles'] = published_profiles(db)
        # Match the client's independently persisted provider cache budget so
        # a successful graph import always has persistable material metadata.
        if len(json.dumps(result['snapshot'], allow_nan=False).encode('utf-8')) > MAX_PROVIDER_BYTES:
            raise ValueError('Provider material metadata exceeds 4 MiB')
        result['native_delta'] = _delta(indexed, prior[0] if prior else None)
        operations = sum(len(part['upserts']) + len(part['deletes']) for part in result['native_delta']['tables'].values())
        if operations > MAX_ROWS:
            result['reset'] = True
            result['native_delta'] = _delta(indexed, None)
        if len(json.dumps(result, allow_nan=False).encode('utf-8')) > MAX_REPLY_BYTES:
            # A large deletion/update union can exceed a complete graph's size.
            result['reset'] = True
            result['native_delta'] = _delta(indexed, None)
            if len(json.dumps(result, allow_nan=False).encode('utf-8')) > MAX_REPLY_BYTES:
                raise ValueError('Synchronization reply exceeds 64 MiB')
        size = _retained_size(indexed)
        if size <= HISTORY_BYTES:
            if current in store._sync_history:
                store._sync_history_bytes -= store._sync_history.pop(current)[1]
            store._sync_history[current] = (indexed, size)
            store._sync_history_bytes += size
            _trim_history(store)
        # Own retained rows independently of response callers/serializers.
        # The JSON round trip also prevents mutable nested row values escaping.
        return json.loads(json.dumps(result, allow_nan=False))
