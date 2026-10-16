"""Small permanent idempotency identities, compatible with legacy JSON receipts."""
import hashlib
import hmac
import json


def canonical_request(action, request):
    """Response shape is a transport hint, never a new inventory mutation."""
    data = {key: value for key, value in request.items() if key != 'response'}
    return json.dumps(dict(action=action, data=data), sort_keys=True, allow_nan=False)


def digest(payload):
    """Hash the existing canonical request encoding, including its action and key."""
    return 'sha256:v1:' + hashlib.sha256(payload.encode('utf-8')).hexdigest()


def matches(stored, payload):
    # Old deployments stored canonical JSON verbatim. Do not expire either form:
    # a forgotten request key could replay an already committed stock mutation.
    expected = digest(payload) if stored.startswith('sha256:v1:') else payload
    return hmac.compare_digest(stored.encode('utf-8'), expected.encode('utf-8'))


def compact_legacy_receipts(store, limit=250):
    """Compact a bounded startup batch without deleting any replay identity.

    Unknown receipt formats and the production-import metadata stay untouched.
    Large historical native responses become acceptance markers: replays return
    current authority, while their original accepted revision remains available.
    """
    if type(limit) is not int or not 1 <= limit <= 1000:
        raise ValueError('Invalid receipt compaction batch size')
    actions = ('native_apply', 'provider_apply', 'provider_job', 'provider_delta')
    compacted = saved = scanned_bytes = 0
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        candidates = db.execute("""SELECT request_key,payload,result FROM receipts
            WHERE payload NOT LIKE 'sha256:v1:%' AND
            (request_key LIKE 'native:%' OR payload LIKE '{"action": "native_apply",%'
             OR payload LIKE '{"action": "provider_apply",%' OR payload LIKE '{"action": "provider_job",%'
             OR payload LIKE '{"action": "provider_delta",%') ORDER BY rowid LIMIT ?""", (limit,))
        for row in candidates:
            payload = row['payload']; result = row['result']
            size = len(payload.encode('utf-8')) + len(result.encode('utf-8'))
            if scanned_bytes and scanned_bytes + size > 8 * 1024 * 1024:
                break
            scanned_bytes += size
            try:
                value = json.loads(payload)
                if isinstance(value, dict) and set(value) == {'action', 'data'} and value['action'] in actions and isinstance(value['data'], dict):
                    if canonical_request(value['action'], value['data']) != payload:
                        continue
                    if value['action'] == 'native_apply':
                        previous = json.loads(result)
                        if not isinstance(previous, dict):continue
                        if previous.get('receipt_kind') != 'native_apply_v1':
                            revision = previous.get('revision')
                            if type(revision) is not int or revision < 0 or previous.get('schema_version') != 1 or 'native_bundle' not in previous:
                                continue
                            result = json.dumps(dict(receipt_kind='native_apply_v1', accepted_revision=revision))
                elif (row['request_key'] == 'native:' + hashlib.sha256(payload.encode('utf-8')).hexdigest()
                      and isinstance(value, dict) and set(value) == {'revision', 'bundle'}
                      and json.dumps(value, sort_keys=True, allow_nan=False) == payload):
                    pass
                else:
                    continue
            except (ValueError, TypeError):
                continue
            hashed = digest(payload)
            db.execute('UPDATE receipts SET payload=?,result=? WHERE request_key=?', (hashed, result, row['request_key']))
            compacted += 1
            saved += size - len(hashed.encode('utf-8')) - len(result.encode('utf-8'))
    return dict(compacted=compacted, bytes_saved=saved)
