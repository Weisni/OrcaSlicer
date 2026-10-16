"""Explicitly close a cancelled, unobserved transfer without a stock write."""
import json
from .provider import identity,text,stamp
from .receipts import canonical_request,digest,matches


def can_discard_provider(db,job):
    if not job or job['state'] not in ('reserved','needs_review','printing') or job['settlement']:
        return False
    ident=job.get('uuid') if isinstance(job,dict) else job['uuid']
    source=db.execute('SELECT source FROM origins WHERE job_uuid=?',(ident,)).fetchone()
    intent=db.execute('SELECT status FROM provider_dispatch WHERE job_uuid=?',(ident,)).fetchone()
    if not source or source['source']!='quack_provider' or not intent or intent['status'] not in ('prepared','uncertain'):
        return False
    if db.execute('SELECT 1 FROM observations WHERE job_uuid=?',(ident,)).fetchone():
        return False
    return not db.execute("SELECT 1 FROM events WHERE kind IN ('observed_provider_start','observed_start','observed_state','observed_progress','observed_terminal') AND json_extract(data,'$.uuid')=?",(ident,)).fetchone()


def discard_provider(store,request):
    from .store import Conflict
    if not isinstance(request,dict) or set(request)!={'request_key','job_uuid','revision','confirmed'}:
        raise ValueError('Invalid never-started transfer confirmation')
    key=text(request['request_key'],128);ident=identity(request['job_uuid'])
    if request['confirmed'] is not True or type(request['revision']) is not int or request['revision']<0:
        raise ValueError('Confirm the cancelled transfer against a fresh revision')
    fingerprint=canonical_request('discard_provider',request)
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        receipt=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
        if receipt:
            if not matches(receipt['payload'],fingerprint):raise Conflict('Transfer confirmation key reused')
            return json.loads(receipt['result'])
        if request['revision']!=store._revision(db):raise Conflict('HA changed; reopen the cancelled transfer review')
        job=db.execute('SELECT * FROM jobs WHERE uuid=?',(ident,)).fetchone()
        if not can_discard_provider(db,job):
            raise Conflict('Only an unobserved prepared or uncertain transfer can be confirmed as never started')
        intent=db.execute('SELECT status FROM provider_dispatch WHERE job_uuid=?',(ident,)).fetchone()
        settlement=dict(outcome='failed',quality='estimated',consumption={},confirmation='user_confirmed_never_started')
        db.execute('UPDATE jobs SET state=?,settlement=?,updated_at=? WHERE uuid=?',('discarded',json.dumps(settlement,sort_keys=True),stamp(),ident))
        db.execute("UPDATE provider_dispatch SET status='rejected',updated_at=? WHERE job_uuid=?",(stamp(),ident))
        store.event(db,'provider_transfer_cancelled',dict(uuid=ident,prior_dispatch_status=intent['status'],confirmed_never_started=True,settlement=settlement))
        result=dict(accepted=True,job_uuid=ident,state='discarded',revision=store._revision(db))
        db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(result)))
        return result
