"""Durable approximation of cancelled consumption and explicit corrections."""
import json
from datetime import datetime
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP


def number(value, maximum):
    if isinstance(value, bool):
        return None
    try:
        result = Decimal(str(value))
        return result if result.is_finite() and 0 <= result <= maximum else None
    except (InvalidOperation, ValueError, TypeError):
        return None


class Consumption:
    def record_print_progress(self, db, ident, telemetry):
        if not isinstance(telemetry, dict):
            return
        start = db.execute("SELECT created_at FROM events WHERE kind='observed_provider_start' AND json_extract(data,'$.uuid')=? ORDER BY id LIMIT 1", (ident,)).fetchone()
        if not start:
            return
        def fresh(field):
            when = telemetry.get(field)
            if when is None:
                return True  # Internal fixtures/backfill provide explicitly scoped samples.
            try:
                return datetime.fromisoformat(when.replace('Z','+00:00')) >= datetime.fromisoformat(start['created_at'].replace('Z','+00:00'))
            except (ValueError, TypeError, AttributeError):
                return False
        percent = number(telemetry.get('progress_percent'), 100) if fresh('progress_observed_at') else None
        layer = number(telemetry.get('current_layer'), 1000000) if fresh('layer_observed_at') else None
        total = number(telemetry.get('total_layers'), 1000000)
        if percent == 0:
            layer = None  # A previous attempt's layer may still be published at start.
        previous = db.execute("SELECT data FROM events WHERE kind='observed_progress' AND json_extract(data,'$.uuid')=? ORDER BY id DESC LIMIT 1",(ident,)).fetchone()
        data = json.loads(previous['data']) if previous else {'uuid':ident}
        before = dict(data)
        if percent is not None and percent >= Decimal(str(data.get('progress_percent',0))):
            data['progress_percent'] = float(percent)
        if layer is not None and total is not None and total > 0 and layer <= total:
            if 'current_layer' not in data or layer >= data['current_layer']:
                data.update(current_layer=int(layer), total_layers=int(total))
        if data != before:
            self.event(db, 'observed_progress', data)

    def cancelled_estimate(self, db, ident):
        row = db.execute("SELECT data FROM events WHERE kind='observed_progress' AND json_extract(data,'$.uuid')=? ORDER BY id DESC LIMIT 1",(ident,)).fetchone()
        if not row:
            return None
        sample = json.loads(row['data'])
        if 'progress_percent' in sample:
            numerator = Decimal(str(sample['progress_percent'])); denominator = Decimal(100)
            method = 'progress_percent_v1'
        elif sample.get('total_layers',0) > 0:
            numerator = Decimal(sample['current_layer']); denominator = Decimal(sample['total_layers'])
            method = 'layer_fraction_v1'
        else:
            return None
        job = db.execute('SELECT allocations FROM jobs WHERE uuid=?',(ident,)).fetchone()
        planned = {}
        for allocation in json.loads(job['allocations']):
            roll = allocation['spool_uuid']
            planned[roll] = planned.get(roll,0) + allocation['weight_mg']
        consumption = {roll:int((Decimal(amount)*numerator/denominator).quantize(Decimal(1),rounding=ROUND_HALF_UP)) for roll,amount in planned.items()}
        return consumption, dict(method=method,numerator=str(numerator),denominator=str(denominator),
            planned_mg=planned,sample={k:v for k,v in sample.items() if k!='uuid'})

    def correct_consumption(self, request):
        """Append a correction without rewriting original stock or invoice records."""
        from .store import Conflict, weight
        from .provider import identity, text, canonical_request, digest, matches, stamp
        if not isinstance(request,dict) or set(request)!={'request_key','revision','confirmed','job_uuid','quality','consumption'}:
            raise ValueError('Invalid consumption correction')
        key=text(request['request_key'],128);ident=identity(request['job_uuid'])
        if request['confirmed'] is not True or type(request['revision']) is not int:
            raise ValueError('Confirm the correction against a fresh revision')
        if request['quality'] not in ('estimated','measured') or not isinstance(request['consumption'],dict):
            raise ValueError('Provide estimated or measured allocated quantities')
        quantities={identity(roll):weight(amount) for roll,amount in request['consumption'].items()}
        fingerprint=canonical_request('correct_consumption',request)
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            receipt=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
            if receipt:
                if not matches(receipt['payload'],fingerprint):raise Conflict('Correction key reused')
                return json.loads(receipt['result'])
            if request['revision']!=self._revision(db):raise Conflict('HA changed; reload before correction')
            job=db.execute('SELECT * FROM jobs WHERE uuid=?',(ident,)).fetchone()
            source=db.execute('SELECT source FROM origins WHERE job_uuid=?',(ident,)).fetchone()
            if not job or not job['settlement'] or not source or source['source']!='quack_provider':
                raise Conflict('Only a settled provider print can be corrected')
            old=json.loads(job['settlement'])
            if set(quantities)!=set(old['consumption']) or not quantities:
                raise ValueError('Provide exactly the originally allocated rolls')
            deltas={roll:old['consumption'][roll]-amount for roll,amount in quantities.items()}
            for roll,delta in deltas.items():
                row=db.execute('SELECT remaining_mg FROM spools WHERE uuid=?',(roll,)).fetchone()
                reserved=sum(a['weight_mg'] for other in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL')
                    for a in json.loads(other['allocations']) if a['spool_uuid']==roll)
                if not row or row['remaining_mg']+delta<reserved or row['remaining_mg']+delta>10**10:
                    raise Conflict('Correction conflicts with available stock')
            for roll,delta in deltas.items():
                row=db.execute('SELECT * FROM spools WHERE uuid=?',(roll,)).fetchone()
                data=json.loads(row['data']);data['weight_quality']='estimated'
                remaining=row['remaining_mg']+delta
                if data.get('status')!='archived':data['status']='active' if remaining else 'empty'
                db.execute('UPDATE spools SET data=?,remaining_mg=? WHERE uuid=?',(json.dumps(data),remaining,roll))
            settlement={**old,'quality':request['quality'],'consumption':quantities,'corrected':True}
            db.execute('UPDATE jobs SET settlement=?,updated_at=? WHERE uuid=?',(json.dumps(settlement,sort_keys=True),stamp(),ident))
            self.event(db,'job_consumption_correction',dict(uuid=ident,before=old,settlement=settlement,stock_deltas=deltas))
            result={'accepted':True,'job_uuid':ident,'revision':self._revision(db)}
            db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(result)))
            return result
