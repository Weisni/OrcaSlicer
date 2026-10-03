"""Versioned native demo mirror. HA remains the sole stock authority."""
import copy
import hashlib
import json
import uuid
from datetime import datetime, timezone
from .receipts import digest, matches

TABLES=('inventory_settings','spools','spool_identifiers','customers','customer_orders','print_jobs','allocations','job_identifiers','stock_events','print_job_manual_overrides')
def stamp(): return datetime.now(timezone.utc).isoformat()
def uid(value): return str(uuid.uuid5(uuid.NAMESPACE_URL,'quack-ha-mirror/'+value))


def booked_material_cost(price_micros, weight_mg):
    """Use the native ledger's per-allocation cent rounding and minimum."""
    if not weight_mg:
        return 0
    return max(1, (price_micros * weight_mg + 5000000000) // 10000000000) * 10000


class NativeBridge:
    def _revision(self,db): return db.execute('SELECT COALESCE(MAX(id),0) FROM events').fetchone()[0]

    def sync_native(self,request):
        from .store import Conflict,weight
        bundle=request['bundle']
        if bundle.get('schema_version')!=8 or set(bundle.get('tables',{}))!=set(TABLES): raise ValueError('Native schema mismatch')
        if len(json.dumps(bundle))>900000: raise ValueError('Demo native snapshot exceeds size limit')
        tables=bundle['tables']
        if any(not isinstance(rows,list) or len(rows)>2000 for rows in tables.values()): raise ValueError('Invalid native rows')
        fingerprint=json.dumps(request,sort_keys=True)
        receipt='native:'+hashlib.sha256(fingerprint.encode()).hexdigest()
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior=db.execute('SELECT payload FROM receipts WHERE request_key=?',(receipt,)).fetchone()
            if prior and matches(prior['payload'],fingerprint): return self.snapshot(db)
            if type(request.get('revision')) is not int or request['revision']!=self._revision(db):
                raise Conflict('HA changed since last sync; local edits retained. Reconcile before retrying.')
            previous=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
            previous=json.loads(previous['data'])['tables'] if previous else {t:[] for t in TABLES}
            # Validate stock and reservations before replacing any canonical rows.
            balances={s['id']:0 for s in tables['spools']}
            for e in tables['stock_events']:
                if e['spool_id'] not in balances or type(e['delta_mg']) is not int: raise ValueError('Invalid stock event')
                balances[e['spool_id']]+=e['delta_mg']
            for value in balances.values(): weight(value)
            jobs={j['id']:j for j in tables['print_jobs']}
            reserved={key:0 for key in balances}
            for a in tables['allocations']:
                amount=weight(a['estimated_weight_mg'])
                if a['spool_id'] not in balances or a['job_id'] not in jobs or not amount: raise ValueError('Invalid allocation')
                if jobs[a['job_id']]['state'] in ('reserved','printing','needs_review'): reserved[a['spool_id']]+=amount
            if any(reserved[k]>balances[k] for k in balances): raise Conflict('Native reservations exceed remaining stock')
            # Never delete a roll by omission. Archive is an explicit stable-UUID status.
            if any(s['id'] not in balances for s in previous['spools']): raise Conflict('Roll deletion is not supported; archive it')
            oldspools={s['id']:s for s in previous['spools']}
            oldevents=previous['stock_events']
            for s in tables['spools']:
                ident=s['id']; uuid.UUID(ident)
                row=db.execute('SELECT * FROM spools WHERE uuid=?',(ident,)).fetchone()
                old=oldspools.get(ident)
                oldbalance=sum(e['delta_mg'] for e in oldevents if e['spool_id']==ident)
                changed=not row or old!=s or oldbalance!=balances[ident]
                if not changed: continue
                existing=json.loads(row['data']) if row else {}
                existing.update(uuid=ident,product=s['name'],manufacturer=s['manufacturer'],material_type=s['material_type'],
                    material_preset=s['filament_preset_id'],color=s['color_hex'],diameter_mm=s['diameter_mm'],density_g_cm3=s['density_g_cm3'],
                    nominal_mg=s['nominal_capacity_mg'],status=s['status'],bambu_material='Bambu '+s['material_type'] if s['material_type'] in ('PLA','PETG') else None,demo=self.settings.get('mode')!='pilot',
                    preset_revision='installed-local',weight_quality=existing.get('weight_quality','estimated') if row and row['remaining_mg']==balances[ident] else 'estimated',
                    material_price_per_kg_micros=s.get('material_price_per_kg_micros',20000000),price_currency=s.get('price_currency','EUR'))
                existing['profile_unresolved']=not bool(s['filament_preset_id'].strip())
                if s['status'] in ('archived','empty'):
                    db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?',(ident,))
                elif row: db.execute('UPDATE slots SET revision=revision+1 WHERE spool_uuid=?',(ident,))
                db.execute('INSERT INTO spools VALUES (?,?,?) ON CONFLICT(uuid) DO UPDATE SET data=excluded.data,remaining_mg=excluded.remaining_mg',
                    (ident,json.dumps(existing),balances[ident]))
            oldjobs={j['id']:j for j in previous['print_jobs']}
            for j in tables['print_jobs']:
                if oldjobs.get(j['id'])==j and [a for a in previous['allocations'] if a['job_id']==j['id']]==[a for a in tables['allocations'] if a['job_id']==j['id']]: continue
                allocations=[]; consumption={}
                for a in tables['allocations']:
                    if a['job_id']!=j['id']: continue
                    slot=db.execute('SELECT id,revision FROM slots WHERE spool_uuid=?',(a['spool_id'],)).fetchone()
                    allocations.append(dict(slot=slot['id'] if slot else 'unassigned',spool_uuid=a['spool_id'],weight_mg=a['estimated_weight_mg'],
                        binding_revision=slot['revision'] if slot else 0,material_preset=a.get('filament_preset_id',''),color=a.get('color_hex','#FFFFFF')))
                    if a.get('actual_weight_mg') is not None: consumption[a['spool_id']]=consumption.get(a['spool_id'],0)+weight(a['actual_weight_mg'])
                state=j['state']; settlement=None
                if state in ('completed','discarded'):
                    settlement=json.dumps(dict(outcome='completed' if state=='completed' else 'failed',quality='estimated',consumption=consumption))
                # The native schema cannot represent failure or measurement quality.
                # A projected settlement returning through Quack must retain HA evidence.
                canonical=db.execute('SELECT state,settlement,allocations FROM jobs WHERE uuid=?',(j['id'],)).fetchone()
                if canonical and canonical['settlement']:
                    recorded=json.loads(canonical['settlement'])
                    if {k:v for k,v in recorded['consumption'].items() if v}!={k:v for k,v in consumption.items() if v}:
                        raise Conflict('Settled consumption differs from HA; reconcile explicitly')
                    state=canonical['state']; settlement=canonical['settlement']
                    if not allocations and not any(recorded['consumption'].values()):
                        allocations=json.loads(canonical['allocations'])
                db.execute('INSERT INTO jobs VALUES (?,?,?,?,?,?,?,?) ON CONFLICT(uuid) DO UPDATE SET name=excluded.name,state=excluded.state,allocations=excluded.allocations,settlement=excluded.settlement,updated_at=excluded.updated_at',
                    (j['id'],'native:'+j['id'],j['job_name'],state,json.dumps(allocations),settlement,j.get('created_at',stamp()),j.get('updated_at',stamp())))
                if not canonical: db.execute('INSERT OR IGNORE INTO origins VALUES (?,?)',(j['id'],'quack_native'))
                if j.get('customer_order_id'): db.execute('INSERT OR REPLACE INTO joblinks VALUES (?,?)',(j['id'],j['customer_order_id']))
            db.execute("INSERT INTO bridge VALUES ('native',?) ON CONFLICT(id) DO UPDATE SET data=excluded.data",(json.dumps(bundle),))
            self.event(db,'native_sync',dict(spools=len(tables['spools']),jobs=len(tables['print_jobs']),orders=len(tables['customer_orders'])))
            db.execute('INSERT INTO receipts VALUES (?,?,?)',(receipt,digest(fingerprint),'{}'))
        return self.snapshot()

    def project_native(self,db,spools,jobs,revision):
        row=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
        if not row: return None
        bundle=copy.deepcopy(json.loads(row['data'])); t=bundle['tables']
        # Synthesized legacy rows need stable creation identity between reads of
        # the same revision. Capture time belongs to the snapshot envelope only.
        first=db.execute('SELECT created_at FROM events ORDER BY id LIMIT 1').fetchone()
        ts=first['created_at'] if first else '1970-01-01T00:00:00+00:00'
        byid={s['id']:s for s in t['spools']}
        for s in spools:
            ident=s['uuid']; n=byid.get(ident)
            if n is None:
                n=dict(id=ident,warning_mode='none',warning_value=0,created_at=s.get('created_at',ts),updated_at=ts)
                t['spools'].append(n)
                t['spool_identifiers'].append(dict(kind='quack_ndef_uuid',value=ident,spool_id=ident,created_at=ts))
            n.update(name=s['product'],manufacturer=s['manufacturer'],material_type=s['material_type'],filament_preset_id=s['material_preset'],
                color_hex=s['color'],diameter_mm=s.get('diameter_mm',1.75),density_g_cm3=s.get('density_g_cm3',1.26),nominal_capacity_mg=s.get('nominal_mg',1000000),
                status=s.get('status','active' if s['remaining_mg'] else 'empty'),material_price_per_kg_micros=s.get('material_price_per_kg_micros',20000000),price_currency=s.get('price_currency','EUR'))
            balance=sum(e['delta_mg'] for e in t['stock_events'] if e['spool_id']==ident)
            bookings=[]
            keys={e['operation_key'] for e in t['stock_events']}
            for event in db.execute("SELECT id,kind,data,created_at FROM events WHERE kind IN ('job_settlement','create','weigh') ORDER BY id"):
                event_data=json.loads(event['data'])
                if event['kind']=='job_settlement':
                    amount=event_data['settlement']['consumption'].get(ident,0)
                    key='ha-consumption:'+event_data['uuid']+':'+ident
                    if amount and key not in keys and not any(e.get('job_id')==event_data['uuid'] and e['spool_id']==ident and e['event_type']=='consumption' for e in t['stock_events']):
                        bookings.append((key,event_data['uuid'],-amount,event['created_at'],'HA '+event_data['settlement']['quality']+' consumption','consumption'))
                elif event_data.get('spool_uuid')==ident and 'delta_mg' in event_data:
                    key='ha-lifecycle:'+str(event['id'])+':'+ident
                    if key not in keys:
                        bookings.append((key,None,event_data['delta_mg'],event['created_at'],'HA '+event_data['quality']+' '+event['kind'],'initial' if event['kind']=='create' else 'adjustment'))
            target=s['remaining_mg']-sum(b[2] for b in bookings)
            if balance!=target:
                key=f'ha-stock:{ident}:{revision}'
                latest=db.execute('SELECT created_at FROM events ORDER BY id DESC LIMIT 1').fetchone()
                t['stock_events'].append(dict(id=uid(key),spool_id=ident,job_id=None,allocation_id=None,event_type='adjustment',delta_mg=target-balance,
                    balance_after_mg=target,operation_key=key,note='HA authoritative stock reconciliation; quantity provenance unknown',created_at=latest['created_at'] if latest else s.get('created_at',ts)))
                balance=target
            for key,job,delta,created,note,event_type in bookings:
                balance+=delta
                t['stock_events'].append(dict(id=uid(key),spool_id=ident,job_id=job,allocation_id=None,event_type=event_type,delta_mg=delta,
                    balance_after_mg=balance,operation_key=key,note=note,created_at=created))
        # Job updated_at can change after a later metadata/cost edit. Preserve
        # the actual lifecycle event times instead of moving print completion.
        starts, finishes, terminals = {}, {}, {}
        for event in db.execute("SELECT kind,data,created_at FROM events WHERE kind IN ('observed_provider_start','observed_start','observed_terminal','job_settlement') ORDER BY id"):
            data=json.loads(event['data'])
            ident=data.get('uuid')
            if not ident: continue
            target=(finishes if event['kind']=='job_settlement' else
                    terminals if event['kind']=='observed_terminal' else starts)
            target.setdefault(ident,event['created_at'])
        byjob={j['id']:j for j in t['print_jobs']}
        for j in jobs:
            native=byjob.get(j['uuid'])
            is_new=native is None
            if native is None:
                native=dict(id=j['uuid'],idempotency_key='ha:'+j['uuid'],job_name=j['name'],printer_id='duck-poop-demo',created_at=j['created_at'],updated_at=j['updated_at'])
                t['print_jobs'].append(native)
            native['customer_order_id']=j.get('customer_order_uuid')
            native['state']={'paused':'printing','failed':'completed' if j['settlement'] else 'needs_review'}.get(j['state'],j['state'])
            if j['settlement'] and not native.get('completed_at'):
                completion = finishes.get(j['uuid'],j['updated_at'])
                if j.get('source') == 'quack_provider':
                    completion = terminals.get(j['uuid'], completion)
                native['completed_at']=completion
            if not native.get('started_at') and j['uuid'] in starts:
                native['started_at']=starts[j['uuid']]
            if (j['settlement'] and j.get('source')=='quack_provider' and native.get('started_at')
                    and native.get('actual_runtime_seconds') is None):
                try:
                    start=datetime.fromisoformat(native['started_at'].replace('Z','+00:00'))
                    finish=datetime.fromisoformat(native['completed_at'].replace('Z','+00:00'))
                    elapsed=int((finish-start).total_seconds())
                    if 0 <= elapsed <= 366*86400:
                        native['actual_runtime_seconds']=elapsed
                        native['electricity_cost_micros']=(native.get('electricity_price_per_kwh_micros',0)
                            *native.get('machine_power_watts',0)*elapsed)//3600000
                        for category in ('machine_wear','maintenance','repair_reserve'):
                            native[category+'_cost_micros']=native.get(category+'_per_hour_micros',0)*elapsed//3600
                except (ValueError,TypeError,OverflowError):
                    pass  # Unknown timestamps must not manufacture a duration.
            native['updated_at']=j['updated_at']
            allocations=[a for a in t['allocations'] if a['job_id']==j['uuid']]
            for index,a in enumerate(a for a in j['allocations'] if a['weight_mg']>0):
                n=allocations[index] if index<len(allocations) else None
                new_allocation=n is None
                if n is None:
                    n=dict(id=uid(j['uuid']+':'+str(index)),job_id=j['uuid'],filament_index=index)
                    s=next(s for s in spools if s['uuid']==a['spool_uuid'])
                    n.update(spool_id=s['uuid'],spool_name=s['product'],manufacturer=s['manufacturer'],material_type=s['material_type'],
                        filament_preset_id=a['material_preset'],color_hex=a['color'],estimated_weight_mg=a['weight_mg'],
                        material_price_per_kg_micros=s.get('material_price_per_kg_micros',20000000),cost_currency=s.get('price_currency','EUR'))
                    n['estimated_material_cost_micros']=booked_material_cost(n['material_price_per_kg_micros'],a['weight_mg'])
                    t['allocations'].append(n)
                # Historical product/profile/price snapshots are immutable. Only
                # the explicit settlement quantity changes for an existing allocation.
                if j['settlement']:
                    # One physical roll may feed multiple project indices. Preserve
                    # explicit native allocation amounts when they sum correctly;
                    # otherwise distribute HA's aggregate by reserved estimates.
                    ident=a['spool_uuid'];total=j['settlement']['consumption'].get(ident)
                    related=[x for x in allocations if x['spool_id']==ident]
                    if total is not None and related and all(x.get('actual_weight_mg') is not None for x in related) and sum(x['actual_weight_mg'] for x in related)==total:
                        pass
                    elif total is not None:
                        same=[x for x in j['allocations'] if x['spool_uuid']==ident and x['weight_mg']>0]
                        denominator=sum(x['weight_mg'] for x in same)
                        prior=sum(x['weight_mg'] for x in j['allocations'][:index] if x['spool_uuid']==ident and x['weight_mg']>0)
                        n['actual_weight_mg']=(total*(prior+a['weight_mg'])//denominator)-(total*prior//denominator)
                    else:n['actual_weight_mg']=None
                    if n.get('actual_weight_mg') is not None and (new_allocation or j.get('source')=='quack_provider'):
                        n['actual_material_cost_micros']=booked_material_cost(n.get('material_price_per_kg_micros',0),n['actual_weight_mg'])
        return bundle

    def order_action(self,request):
        from .store import Conflict
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
            if not row: raise Conflict('Open Quack Filament Manager once to initialize the shared demo')
            bundle=json.loads(row['data']); tables=bundle['tables']
            if request.get('uuid'):
                order=next((o for o in tables['customer_orders'] if o['id']==request['uuid']),None)
                if not order: raise ValueError('Unknown order UUID')
                if request.get('archive'): order['archived']=1
                else:
                    if request.get('title'): order['title']=str(request['title'])[:256]
                    if request.get('status') in ('draft','active','completed','cancelled'): order['status']=request['status']
            else:
                title=request.get('title','').strip()
                if not title or len(title)>256: raise ValueError('Order title is required')
                customer=str(uuid.uuid4()); order=dict(id=str(uuid.uuid4()),customer_id=customer,title=title,currency='EUR',status='active',archived=0,created_at=stamp(),updated_at=stamp())
                tables['customers'].append(dict(id=customer,name=str(request.get('customer','Demo customer'))[:256]))
                tables['customer_orders'].append(order)
            db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
            self.event(db,'order',dict(order_uuid=order['id']))
            return dict(uuid=order['id'])
