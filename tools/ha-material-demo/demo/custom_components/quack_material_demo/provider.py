"""Authoritative native transactions; dispatch intents never send device commands."""
import copy
import json
import math
import uuid

from .native_bridge import TABLES, stamp
from .explicit_sync import FIELDS, CANONICAL, validate_fields
from .receipts import canonical_request, digest, matches
from .read_api import provider_view
from .profiles import detach_incompatible_profile


OPEN = {'reserved', 'printing', 'needs_review'}
TERMINAL = {'completed', 'discarded'}
KEYS = {'spool_identifiers': ('kind','value'), 'job_identifiers': ('provider','kind','value'),
        'print_job_manual_overrides': ('job_id',)}
DEFAULTS = {
    'spools': dict(manufacturer='',filament_preset_id='',material_price_per_kg_micros=0,price_currency='EUR'),
    'customers': dict(contact_name='',email='',phone='',notes='',archived=0),
    'customer_orders': dict(order_number='',notes='',quoted_price_micros=None,invoice_amount_micros=None,
        archived=0,design_time_seconds=0,design_hourly_rate_micros=45000000,other_cost_micros=0,
        discount_basis_points=0,**{'bill_'+k:1 for k in ('material','electricity','machine_wear','maintenance','repair_reserve','design','other')}),
    'print_jobs': dict(project_path='',printer_id='',completed_at='',started_at='',customer_order_id=None,
        cost_currency='EUR',electricity_price_per_kwh_micros=400000,machine_power_watts=0,
        estimated_runtime_seconds=0,electricity_cost_micros=0,actual_runtime_seconds=None,
        **{k:0 for k in ('machine_wear_per_hour_micros','maintenance_per_hour_micros','repair_reserve_per_hour_micros',
                        'machine_wear_cost_micros','maintenance_cost_micros','repair_reserve_cost_micros')}),
    'allocations': dict(actual_weight_mg=None,material_price_per_kg_micros=0,cost_currency='EUR',
        estimated_material_cost_micros=0,actual_material_cost_micros=None,spool_name='',manufacturer='',
        material_type='',filament_preset_id='',color_hex='#FFFFFF'),
    'stock_events': dict(job_id=None,allocation_id=None,note=''),
}
COLUMNS = {
    'inventory_settings': 'id currency electricity_price_per_kwh_micros default_machine_power_watts updated_at machine_wear_per_hour_micros maintenance_per_hour_micros repair_reserve_per_hour_micros design_per_hour_micros',
    'spools': 'id manufacturer material_type name filament_preset_id color_hex diameter_mm density_g_cm3 nominal_capacity_mg warning_mode warning_value status created_at updated_at material_price_per_kg_micros price_currency',
    'spool_identifiers': 'kind value spool_id created_at',
    'customers': 'id name contact_name email phone notes archived created_at updated_at',
    'customer_orders': 'id customer_id order_number title notes quoted_price_micros invoice_amount_micros currency status created_at updated_at archived design_time_seconds design_hourly_rate_micros other_cost_micros discount_basis_points bill_material bill_electricity bill_machine_wear bill_maintenance bill_repair_reserve bill_design bill_other',
    'print_jobs': 'id idempotency_key job_name project_path printer_id state created_at updated_at completed_at customer_order_id cost_currency electricity_price_per_kwh_micros machine_power_watts estimated_runtime_seconds electricity_cost_micros started_at actual_runtime_seconds machine_wear_per_hour_micros maintenance_per_hour_micros repair_reserve_per_hour_micros machine_wear_cost_micros maintenance_cost_micros repair_reserve_cost_micros',
    'allocations': 'id job_id spool_id filament_index estimated_weight_mg actual_weight_mg material_price_per_kg_micros cost_currency estimated_material_cost_micros actual_material_cost_micros spool_name manufacturer material_type filament_preset_id color_hex',
    'job_identifiers': 'provider kind value job_id created_at',
    'stock_events': 'id spool_id job_id allocation_id event_type delta_mg balance_after_mg operation_key note created_at',
    'print_job_manual_overrides': 'job_id created_at',
}
REQUIRED = {
    'inventory_settings': 'id currency electricity_price_per_kwh_micros default_machine_power_watts',
    'spools': 'id material_type name color_hex diameter_mm density_g_cm3 nominal_capacity_mg warning_mode warning_value status',
    'spool_identifiers': 'kind value spool_id',
    'customers': 'id name',
    'customer_orders': 'id customer_id title currency status',
    'print_jobs': 'id idempotency_key job_name state',
    'allocations': 'id job_id spool_id filament_index estimated_weight_mg',
    'job_identifiers': 'provider kind value job_id',
    'stock_events': 'id spool_id event_type delta_mg balance_after_mg operation_key',
    'print_job_manual_overrides': 'job_id',
}
NULLABLE = {
    'customer_orders': {'quoted_price_micros','invoice_amount_micros'},
    'print_jobs': {'customer_order_id','actual_runtime_seconds'},
    'allocations': {'actual_weight_mg','actual_material_cost_micros'},
    'stock_events': {'job_id','allocation_id'},
}
NUMBERS = {'nominal_capacity_mg','warning_value','filament_index','estimated_weight_mg',
           'actual_weight_mg','delta_mg','balance_after_mg','archived'}


def text(value, maximum=256):
    if not isinstance(value,str) or not value.strip() or len(value)>maximum:
        raise ValueError('Invalid provider text')
    return value


def identity(value):
    if not isinstance(value,str) or str(uuid.UUID(value)) != value:
        raise ValueError('Use canonical UUIDs')
    return value


def indexed(bundle, max_rows=2000):
    if not isinstance(bundle,dict) or set(bundle)!={'schema_version','tables'} or bundle['schema_version']!=8 or set(bundle['tables'])!=set(TABLES):
        raise ValueError('Provider requires schema 8')
    result={}
    for table in TABLES:
        rows=bundle['tables'][table]
        if not isinstance(rows,list) or max_rows is not None and len(rows)>max_rows:raise ValueError('Invalid provider rows')
        result[table]={}
        for row in rows:
            if not isinstance(row,dict) or not row or set(row)-set(COLUMNS[table].split()):
                raise ValueError('Unknown provider columns')
            if set(REQUIRED[table].split())-set(row):raise ValueError('Missing required native columns')
            for field,value in row.items():
                if isinstance(value,(dict,list,bool)) or isinstance(value,float) and not math.isfinite(value):
                    raise ValueError('Invalid native scalar')
                if value is None:
                    if field not in NULLABLE.get(table,set()):raise ValueError('Null native field')
                    continue
                numeric=field in NUMBERS or field.endswith(('_micros','_seconds','_watts','_basis_points')) or field.startswith('bill_') or table=='inventory_settings' and field=='id'
                if field in ('diameter_mm','density_g_cm3'):
                    if type(value) not in (int,float):raise ValueError('Invalid native real')
                elif numeric:
                    if type(value) is not int:raise ValueError('Invalid native integer')
                elif not isinstance(value,str):raise ValueError('Invalid native text')
                if isinstance(value,str) and len(value)>16000:raise ValueError('Provider text exceeds limit')
            key=tuple(row[k] for k in KEYS.get(table,('id',)))
            if key in result[table]:raise ValueError('Duplicate provider row')
            result[table][key]=row
    return result


def normalized(table,row, baseline=False):
    value={**DEFAULTS.get(table,{}),**row}
    # HA projects updated_at for newly surfaced rows at snapshot time.
    if baseline:value.pop('updated_at',None)
    return value


def same_graph(left,right):
    a,b=indexed(left),indexed(right)
    return all(set(a[t])==set(b[t]) and all(normalized(t,a[t][k],True)==normalized(t,b[t][k],True) for k in a[t]) for t in TABLES)


class Provider:
    def provider_request_recorded(self,action,request):
        """An already accepted request may be replayed during a pending assignment."""
        if action not in ('native_apply','provider_apply','provider_delta','provider_job') or not isinstance(request,dict):return False
        with self.connection() as db:
            row=db.execute('SELECT payload FROM receipts WHERE request_key=?',(request.get('request_key'),)).fetchone()
            return bool(row and matches(row['payload'],canonical_request(action,request)))

    def _settle_provider_finish(self,db,ident,name):
        """A correlated successful print settles its reserved estimate exactly once."""
        intent=db.execute("SELECT * FROM provider_dispatch WHERE job_uuid=? AND status='observing' AND print_name=?",(ident,name)).fetchone()
        job=db.execute('SELECT * FROM jobs WHERE uuid=?',(ident,)).fetchone()
        if not intent or not job or job['settlement']:return
        allocations=json.loads(job['allocations']);consumption={}
        for a in allocations:consumption[a['spool_uuid']]=consumption.get(a['spool_uuid'],0)+a['weight_mg']
        if not consumption:return
        for spool,amount in consumption.items():
            row=db.execute('SELECT remaining_mg FROM spools WHERE uuid=?',(spool,)).fetchone()
            reserved=sum(a['weight_mg'] for other in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL AND uuid<>?',(ident,))
                         for a in json.loads(other['allocations']) if a['spool_uuid']==spool)
            if not row or amount>row['remaining_mg']-reserved:
                self.event(db,'provider_settlement_review',dict(job_uuid=ident,reason='estimated consumption conflicts with stock'))
                return
        for spool,amount in consumption.items():
            row=db.execute('SELECT * FROM spools WHERE uuid=?',(spool,)).fetchone()
            metadata=json.loads(row['data']);remaining=row['remaining_mg']-amount;metadata['weight_quality']='estimated'
            if not remaining:
                metadata['status']='empty'
                db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?',(spool,))
            db.execute('UPDATE spools SET data=?,remaining_mg=? WHERE uuid=?',(json.dumps(metadata),remaining,spool))
        settlement=dict(outcome='completed',quality='estimated',consumption=consumption)
        db.execute("UPDATE jobs SET state='completed',settlement=?,updated_at=? WHERE uuid=?",(json.dumps(settlement,sort_keys=True),stamp(),ident))
        self.event(db,'job_settlement',dict(uuid=ident,settlement=settlement))

    def provider_apply(self, request):
        from .store import Conflict, weight
        if set(request)!={'request_key','revision','before','bundle'}:raise ValueError('Invalid provider request')
        key=text(request['request_key'],128)
        fingerprint=json.dumps(dict(action='provider_apply',data=request),sort_keys=True,allow_nan=False)
        if len(fingerprint.encode())>2000000:raise ValueError('Provider request exceeds limit')
        if len(json.dumps(request['bundle'],separators=(',',':'),ensure_ascii=False).encode())>1024*1024:raise ValueError('Native bundle exceeds import limit')
        before,new=indexed(request['before']),indexed(request['bundle'])
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior=db.execute('SELECT payload FROM receipts WHERE request_key=?',(key,)).fetchone()
            if prior:
                if not matches(prior['payload'],fingerprint):raise Conflict('Provider request key reused')
                return self.snapshot(db)
            if type(request['revision']) is not int or request['revision']!=self._revision(db):raise Conflict('HA changed; refresh provider before editing')
            snapshot=self.snapshot(db)
            current=snapshot['native_bundle'] or dict(schema_version=8,tables={t:[] for t in TABLES})
            if not same_graph(request['before'],current):raise Conflict('Provider baseline does not match HA authority')
            self._commit_provider_graph(db,before,request['bundle'])
            self.event(db,'provider_apply',dict(request_key=key))
            db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),'{}'))
            result=self.snapshot(db)
            if len(json.dumps(result['native_bundle'],separators=(',',':'),ensure_ascii=False).encode())>1024*1024:raise ValueError('Native projection exceeds import limit')
            return result

    def provider_delta(self, request):
        """Merge explicitly compared rows, then validate and commit the complete graph."""
        from .store import Conflict
        if not isinstance(request,dict) or set(request)!={'request_key','revision','changes'}:
            raise ValueError('Invalid provider delta request')
        key=text(request['request_key'],128)
        if type(request['revision']) is not int or request['revision']<0:
            raise ValueError('A nonnegative revision is required')
        changes=request['changes']
        if not isinstance(changes,list) or not 1<=len(changes)<=1000:
            raise ValueError('Select between one and 1000 changed rows')
        fingerprint=json.dumps(dict(action='provider_delta',data=request),sort_keys=True,allow_nan=False)
        if len(fingerprint.encode())>1000000:raise ValueError('Provider delta exceeds limit')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
            if prior:
                if not matches(prior['payload'],fingerprint):raise Conflict('Provider request key reused')
                receipt=json.loads(prior['result'])
                return dict(accepted=True,request_key=key,accepted_revision=receipt['accepted_revision'],revision=self._revision(db))
            if request['revision']>self._revision(db):raise Conflict('Provider revision is ahead of HA')
            snapshot=self.snapshot(db)
            current=snapshot['native_bundle'] or dict(schema_version=8,tables={t:[] for t in TABLES})
            if len(json.dumps(current).encode())>16*1024*1024:raise ValueError('Provider projection exceeds limit')
            before=indexed(current,max_rows=None)
            bundle=copy.deepcopy(current)
            seen=set()
            for change in changes:
                if not isinstance(change,dict) or set(change)!={'table','before','after'} or change['table'] not in TABLES:
                    raise ValueError('Invalid provider row change')
                table=change['table']; old=change['before']; new=change['after']
                if old is None and new is None:raise ValueError('Empty provider change')
                keys=[]
                for row in (old,new):
                    if row is None:continue
                    probe=dict(schema_version=8,tables={t:[] for t in TABLES})
                    probe['tables'][table]=[row]
                    keys.append(next(iter(indexed(probe)[table])))
                row_key=keys[0]
                if any(k!=row_key for k in keys):raise Conflict('Provider row identity cannot change')
                token=(table,row_key)
                if token in seen:raise ValueError('Duplicate provider row change')
                seen.add(token)
                actual=before[table].get(row_key)
                if old is None:
                    if actual is not None:raise Conflict('Provider row already exists')
                elif actual is None or normalized(table,old,True)!=normalized(table,actual,True):
                    raise Conflict('Selected provider row changed; refresh before editing')
                rows=bundle['tables'][table]
                position=next((i for i,row in enumerate(rows) if tuple(row[k] for k in KEYS.get(table,('id',)))==row_key),None)
                if new is None:rows.pop(position)
                elif position is None:rows.append(copy.deepcopy(new))
                else:rows[position]=copy.deepcopy(new)
            if len(json.dumps(bundle).encode())>16*1024*1024:raise ValueError('Provider projection exceeds limit')
            self._commit_provider_graph(db,before,bundle)
            self.event(db,'provider_delta',dict(request_key=key,changed_rows=len(changes)))
            revision=self._revision(db)
            db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(dict(accepted_revision=revision))))
            return dict(accepted=True,request_key=key,accepted_revision=revision,revision=revision)

    def _commit_provider_graph(self,db,before,bundle):
        """Shared conservation/history checks and writes for full and scoped transactions."""
        new=indexed(bundle,max_rows=None)
        balances=self._validate_provider_graph(db,before,new,bundle)
        tables=bundle['tables']
        for s in tables['spools']:
            ident=s['id']; row=db.execute('SELECT * FROM spools WHERE uuid=?',(ident,)).fetchone()
            old=before['spools'].get((ident,))
            if old and normalized('spools',old)==normalized('spools',s) and row['remaining_mg']==balances[ident]:continue
            metadata=json.loads(row['data']) if row else dict(uuid=ident,created_at=s.get('created_at',stamp()),demo=self.settings.get('mode')!='pilot')
            for field in FIELDS:
                if field in s:metadata[CANONICAL.get(field,field)]=s[field]
            metadata.update(preset_revision='installed-local',profile_unresolved=not bool(s['filament_preset_id'].strip()),
                bambu_material='Bambu '+s['material_type'] if s['material_type'] in ('PLA','PETG') else None)
            if not row or row['remaining_mg']!=balances[ident]:metadata['weight_quality']='estimated'
            db.execute('INSERT INTO spools VALUES (?,?,?) ON CONFLICT(uuid) DO UPDATE SET data=excluded.data,remaining_mg=excluded.remaining_mg',
                (ident,json.dumps(metadata),balances[ident]))
            detach_incompatible_profile(db,ident,s)
            if s['status'] in ('archived','empty'):
                db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?',(ident,))
            elif not old or any(old.get(f)!=s.get(f) for f in ('material_type','filament_preset_id','color_hex')):
                db.execute('UPDATE slots SET revision=revision+1 WHERE spool_uuid=?',(ident,))
        for j in tables['print_jobs']:
            ident=j['id']; canonical=db.execute('SELECT * FROM jobs WHERE uuid=?',(ident,)).fetchone()
            allocations=[]; consumption={}
            for a in tables['allocations']:
                if a['job_id']!=ident:continue
                slot=db.execute('SELECT id,revision FROM slots WHERE spool_uuid=?',(a['spool_id'],)).fetchone()
                allocations.append(dict(slot=slot['id'] if slot else 'unassigned',spool_uuid=a['spool_id'],weight_mg=a['estimated_weight_mg'],
                    binding_revision=slot['revision'] if slot else 0,material_preset=a.get('filament_preset_id',''),color=a.get('color_hex','#FFFFFF'),filament_index=a['filament_index']))
                if a.get('actual_weight_mg') is not None:consumption[a['spool_id']]=consumption.get(a['spool_id'],0)+a['actual_weight_mg']
            state=j['state']; settlement=None
            if canonical and canonical['settlement']:
                state=canonical['state'];settlement=canonical['settlement']
                allocations=json.loads(canonical['allocations'])
            elif state in TERMINAL:
                settlement=json.dumps(dict(outcome='completed' if state=='completed' else 'failed',quality='estimated',consumption=consumption),sort_keys=True)
            elif canonical:
                old_native=before['print_jobs'].get((ident,))
                old_alloc=[a for a in before['allocations'].values() if a['job_id']==ident]
                new_alloc=[a for a in tables['allocations'] if a['job_id']==ident]
                if old_native and old_native['state']==state and all(any(normalized('allocations',a)==normalized('allocations',b) for b in new_alloc) for a in old_alloc) and len(old_alloc)==len(new_alloc):
                    allocations=json.loads(canonical['allocations'])
                    # Paused is projected as printing; retain HA's finer state.
                    if canonical['state']=='paused' and state=='printing':state='paused'
            db.execute('INSERT INTO jobs VALUES (?,?,?,?,?,?,?,?) ON CONFLICT(uuid) DO UPDATE SET name=excluded.name,state=excluded.state,allocations=excluded.allocations,settlement=excluded.settlement,updated_at=excluded.updated_at',
                (ident,'provider:'+ident,j['job_name'],state,json.dumps(allocations),settlement,j.get('created_at',stamp()),j.get('updated_at',stamp())))
            if settlement:db.execute("UPDATE provider_dispatch SET status='terminal',updated_at=? WHERE job_uuid=?",(stamp(),ident))
            if not canonical:db.execute('INSERT INTO origins VALUES (?,?)',(ident,'quack_provider'))
            if j.get('customer_order_id'):db.execute('INSERT OR REPLACE INTO joblinks VALUES (?,?)',(ident,j['customer_order_id']))
            else:db.execute('DELETE FROM joblinks WHERE job_uuid=?',(ident,))
        db.execute("INSERT INTO bridge VALUES ('native',?) ON CONFLICT(id) DO UPDATE SET data=excluded.data",(json.dumps(bundle),))

    def _validate_provider_graph(self,db,before,new,bundle):
        from .store import Conflict,weight
        for table in TABLES:
            missing=set(before[table])-set(new[table])
            if table=='allocations':
                missing={k for k in missing if before['print_jobs'][(before[table][k]['job_id'],)]['state']!='reserved' or
                         new['print_jobs'].get((before[table][k]['job_id'],),{}).get('state')!='reserved'}
            elif table=='spool_identifiers':
                missing={k for k in missing if before[table][k]['kind']=='quack_ndef_uuid'}
            elif table=='customer_orders':
                missing={k for k in missing if before[table][k].get('archived',0) or
                         any(j.get('customer_order_id')==k[0] for j in before['print_jobs'].values()) or
                         any(j.get('customer_order_id')==k[0] for j in new['print_jobs'].values())}
            if missing:raise Conflict('Existing inventory or history cannot be omitted')
        for table in ('stock_events','job_identifiers'):
            for k,old in before[table].items():
                if normalized(table,old)!=normalized(table,new[table][k]):raise Conflict('Historical events and identifiers are immutable')
        for k,old in before['spool_identifiers'].items():
            current=new['spool_identifiers'].get(k)
            if current and (old['spool_id']!=current['spool_id'] or old['kind']=='quack_ndef_uuid' and normalized('spool_identifiers',old)!=normalized('spool_identifiers',current)):
                raise Conflict('Existing identifier ownership is immutable')
        spools={s['id']:s for s in new['spools'].values()};jobs={j['id']:j for j in new['print_jobs'].values()}
        balances={ident:0 for ident in spools};reserved={ident:0 for ident in spools}
        for s in spools.values():
            identity(s['id']);validate_fields({f:s[f] for f in FIELDS if f in s})
            for required in ('name','material_type','filament_preset_id','color_hex','nominal_capacity_mg','status','diameter_mm','density_g_cm3'):
                if required not in s:raise ValueError('Incomplete physical roll')
        event_keys=set();consumption={}
        for e in bundle['tables']['stock_events']:
            identity(e['id'])
            if e['spool_id'] not in spools or type(e['delta_mg']) is not int or abs(e['delta_mg'])>10**10:raise ValueError('Invalid stock event')
            if e['operation_key'] in event_keys:raise Conflict('Duplicate stock operation')
            event_keys.add(text(e['operation_key'],256))
            if e['event_type'] not in ('initial','set_remaining','adjustment','refill','consumption'):raise ValueError('Invalid stock event type')
            balances[e['spool_id']]+=e['delta_mg']
            if weight(e['balance_after_mg'])!=balances[e['spool_id']]:raise Conflict('Stock event balance does not conserve quantity')
            if e.get('job_id') and e['job_id'] not in jobs:raise ValueError('Unknown stock job')
            if e.get('allocation_id') and (e['allocation_id'],) not in new['allocations']:raise ValueError('Unknown stock allocation')
            if e.get('allocation_id'):
                allocation=new['allocations'][(e['allocation_id'],)]
                if allocation['job_id']!=e.get('job_id') or allocation['spool_id']!=e['spool_id']:raise Conflict('Consumption allocation does not match job and roll')
            if (e['id'],) not in before['stock_events'] and e['event_type']=='consumption':
                job=jobs.get(e.get('job_id'));old=before['print_jobs'].get((e.get('job_id'),))
                if not job or job['state']!='completed' or not old or old['state'] in TERMINAL or e['delta_mg']>0:raise Conflict('Consumption requires a newly settled job')
                token=(job['id'],e['spool_id']);consumption[token]=consumption.get(token,0)-e['delta_mg']
        for ident,s in spools.items():
            old=before['spools'].get((ident,))
            old_balance=sum(e['delta_mg'] for e in before['stock_events'].values() if e['spool_id']==ident)
            lifecycle_changed=not old or old_balance!=balances[ident] or any(old.get(f)!=s.get(f) for f in ('status','nominal_capacity_mg'))
            if lifecycle_changed and (weight(balances[ident])>s['nominal_capacity_mg'] or s['status']=='empty' and balances[ident] or s['status']=='active' and not balances[ident]):
                raise ValueError('Roll stock contradicts capacity or status')
        job_keys=set()
        for j in jobs.values():
            identity(j['id']);text(j['job_name']);text(j['idempotency_key'],256)
            if j['idempotency_key'] in job_keys:raise Conflict('Duplicate job request identity')
            job_keys.add(j['idempotency_key'])
            if j['state'] not in OPEN|TERMINAL:raise ValueError('Invalid native job state')
            old=before['print_jobs'].get((j['id'],))
            if old:
                if old['idempotency_key']!=j['idempotency_key']:raise Conflict('Job identity is immutable')
                if old['state'] in TERMINAL and old['state']!=j['state']:raise Conflict('Settled jobs cannot reopen')
                if old['state'] in ('printing','needs_review') and j['state']=='reserved':raise Conflict('Started jobs cannot return to reservation')
            elif j['state']!='reserved':raise Conflict('New jobs must start with a central reservation')
            if old and old.get('customer_order_id')!=j.get('customer_order_id'):
                previous_order=new['customer_orders'].get((old.get('customer_order_id'),))
                if previous_order and previous_order.get('archived',0):
                    raise Conflict('Restore the archived customer order before editing its print jobs')
            if j.get('customer_order_id'):
                order=new['customer_orders'].get((j['customer_order_id'],))
                if order is None:raise ValueError('Unknown customer order')
                # A chooser's order can close while its reservation is pending.
                # Validate the current authority inside this transaction, while
                # preserving unchanged links on historical jobs and retries.
                if (not old or old.get('customer_order_id')!=j['customer_order_id']) and (
                        order.get('archived',0) or order.get('status') in ('completed','cancelled')):
                    raise Conflict('A closed customer order cannot receive another print job')
                if (not old or old.get('customer_order_id')!=j['customer_order_id']) and order['currency']!=j.get('cost_currency','EUR'):
                    raise Conflict('Customer-order currency does not match the print-job currency')
        allocation_keys=set();actual={}
        for a in new['allocations'].values():
            identity(a['id'])
            if a['job_id'] not in jobs or a['spool_id'] not in spools:raise ValueError('Unknown allocation identity')
            if type(a['filament_index']) is not int or a['filament_index']<0 or (a['job_id'],a['filament_index']) in allocation_keys:raise ValueError('Invalid allocation filament')
            allocation_keys.add((a['job_id'],a['filament_index']))
            amount=weight(a['estimated_weight_mg'])
            if not amount:raise ValueError('Positive reservation required')
            job=jobs[a['job_id']]; old=before['allocations'].get((a['id'],))
            if old and old['job_id']!=a['job_id']:raise Conflict('Allocation job identity is immutable')
            old_job=before['print_jobs'].get((a['job_id'],))
            if not old and old_job and old_job['state']!='reserved':raise Conflict('Cannot append allocations to a started or settled job')
            if job['state'] in OPEN:
                if spools[a['spool_id']]['status']!='active':raise Conflict('Cannot reserve inactive stock')
                reserved[a['spool_id']]+=amount
                if a.get('actual_weight_mg') is not None:raise Conflict('Unsettled allocation cannot contain consumption')
            if old and before['print_jobs'][(old['job_id'],)]['state']!='reserved':
                immutable=('job_id','spool_id','filament_index','estimated_weight_mg','spool_name','manufacturer','material_type','filament_preset_id','color_hex')
                if any(normalized('allocations',old).get(k)!=normalized('allocations',a).get(k) for k in immutable):raise Conflict('Started allocation identity and estimate are immutable')
                if before['print_jobs'][(a['job_id'],)]['state'] in TERMINAL and old.get('actual_weight_mg')!=a.get('actual_weight_mg'):raise Conflict('Settled consumption is immutable')
            if a.get('actual_weight_mg') is not None:
                token=(a['job_id'],a['spool_id']);actual[token]=actual.get(token,0)+weight(a['actual_weight_mg'])
        for ident,amount in reserved.items():
            if amount>balances[ident]:raise Conflict('Reservations exceed available stock')
        for j in jobs.values():
            old=before['print_jobs'].get((j['id'],))
            if old and old['state'] not in TERMINAL and j['state'] in TERMINAL:
                expected={k:v for k,v in actual.items() if k[0]==j['id'] and v}
                booked={k:v for k,v in consumption.items() if k[0]==j['id'] and v}
                if expected!=booked or j['state']=='discarded' and expected:raise Conflict('Settlement must match exactly the new consumption bookings')
                if j['state']=='completed' and any(a.get('actual_weight_mg') is None for a in new['allocations'].values() if a['job_id']==j['id']):raise Conflict('Complete every allocation explicitly')
        for row in new['spool_identifiers'].values():
            if row['spool_id'] not in spools or row['kind'] not in ('quack_ndef_uuid','nfc_uid','bambu_tag_uid'):raise ValueError('Invalid spool identifier')
            text(row['value'])
            if row['kind']=='quack_ndef_uuid':identity(row['value'])
        for row in new['job_identifiers'].values():
            if row['job_id'] not in jobs:raise ValueError('Invalid job identifier')
            for field in ('provider','kind','value'):text(row[field])
        for row in new['print_job_manual_overrides'].values():
            if row['job_id'] not in jobs:raise ValueError('Invalid job override')
        for table in ('inventory_settings','customers','customer_orders','print_jobs','allocations'):
            for row in new[table].values():
                for field,value in row.items():
                    if field.endswith(('_micros','_seconds','_watts','_basis_points')) or field in ('archived',) or field.startswith('bill_'):
                        if value is not None and (type(value) is not int or not 0<=value<=10**15):raise ValueError('Invalid cost or lifecycle value')
                    if field in ('currency','cost_currency') and (not isinstance(value,str) or len(value)!=3 or not value.isupper()):raise ValueError('Invalid currency')
                    if (field=='archived' or field.startswith('bill_')) and value not in (0,1):raise ValueError('Invalid boolean flag')
                if row.get('discount_basis_points',0)>10000:raise ValueError('Invalid discount')
        for row in new['customers'].values():
            identity(row['id']);text(row['name'])
        for key,row in new['customer_orders'].items():
            identity(row['id']);text(row['title'])
            if row.get('status','active') not in ('draft','active','completed','cancelled'):raise ValueError('Invalid order state')
            if 'customer_id' in row and (row['customer_id'],) not in new['customers']:raise ValueError('Unknown order customer')
            if key not in before['customer_orders'] and 'customer_id' not in row:raise ValueError('New orders require a customer')
        if any(row.get('id')!=1 for row in new['inventory_settings'].values()):raise ValueError('Invalid inventory settings identity')
        if any(row['default_machine_power_watts']<=0 for row in new['inventory_settings'].values()):raise ValueError('Default machine power must be positive')
        return balances

    def reconcile_provider(self,request):
        """Explicit quantities settle only an observed terminal provider attempt."""
        from .store import Conflict,weight
        if not isinstance(request,dict) or set(request)!={'request_key','revision','confirmed','job_uuid','outcome','quality','consumption'}:
            raise ValueError('Invalid provider reconciliation request')
        key=text(request['request_key'],128);ident=identity(request['job_uuid'])
        if request['confirmed'] is not True or type(request['revision']) is not int or request['revision']<0:
            raise ValueError('Explicit confirmation and a current revision are required')
        outcome=request['outcome'];quality=request['quality'];consumption=request['consumption']
        if outcome not in ('completed','failed') or quality not in ('measured','estimated') or not isinstance(consumption,dict):
            raise ValueError('Explicit outcome, quantities and provenance are required')
        if not 1<=len(consumption)<=100:raise ValueError('Provide the allocated roll quantities')
        consumption={identity(spool):weight(amount) for spool,amount in consumption.items()}
        fingerprint=canonical_request('reconcile_provider',request)
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
            if prior:
                if not matches(prior['payload'],fingerprint):raise Conflict('Reconciliation key reused')
                return json.loads(prior['result'])
            if request['revision']!=self._revision(db):raise Conflict('HA changed; reload before reconciliation')
            job=db.execute('SELECT * FROM jobs WHERE uuid=?',(ident,)).fetchone()
            source=db.execute('SELECT source FROM origins WHERE job_uuid=?',(ident,)).fetchone()
            intent=db.execute('SELECT status FROM provider_dispatch WHERE job_uuid=?',(ident,)).fetchone()
            if not job or not source or source['source']!='quack_provider' or job['state']!='needs_review' or job['settlement'] or not intent or intent['status']!='terminal':
                raise Conflict('Only an unsettled observed terminal provider attempt can be reconciled')
            terminal=next((data for event in db.execute("SELECT data FROM events WHERE kind='observed_terminal' ORDER BY id DESC")
                           if (data:=json.loads(event['data'])).get('uuid')==ident),None)
            if not terminal or terminal.get('outcome') not in ('failed','finish'):
                raise Conflict('A known finished or failed printer outcome is required; uncertain dispatches stay pending')
            allocations=json.loads(job['allocations'])
            if not allocations or set(consumption)!={a['spool_uuid'] for a in allocations}:
                raise ValueError('Consumption must cover exactly the known allocated rolls')
            for spool,amount in consumption.items():
                row=db.execute('SELECT * FROM spools WHERE uuid=?',(spool,)).fetchone()
                other_reserved=sum(a['weight_mg'] for other in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL AND uuid<>?',(ident,))
                                   for a in json.loads(other['allocations']) if a['spool_uuid']==spool)
                if not row or amount>row['remaining_mg']-other_reserved:
                    raise Conflict('Consumption conflicts with remaining stock or other reservations')
                remaining=row['remaining_mg']-amount
                metadata=json.loads(row['data']);metadata['weight_quality']=quality
                if not remaining:
                    if metadata.get('status')!='archived':metadata['status']='empty'
                    db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?',(spool,))
                db.execute('UPDATE spools SET data=?,remaining_mg=? WHERE uuid=?',(json.dumps(metadata),remaining,spool))
            settlement=dict(outcome=outcome,quality=quality,consumption=consumption)
            db.execute('UPDATE jobs SET state=?,settlement=?,updated_at=? WHERE uuid=?',(outcome,json.dumps(settlement,sort_keys=True),stamp(),ident))
            self.event(db,'job_settlement',dict(uuid=ident,settlement=settlement))
            result=dict(uuid=ident,state=outcome)
            db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(result)))
            return result

    def provider_job(self,request):
        from .store import Conflict
        if set(request)-{'request_key','command','job_uuid','data','response'} or not {'request_key','command','job_uuid','data'}<=set(request):raise ValueError('Invalid provider job request')
        response=request.get('response','snapshot')
        if response not in ('snapshot','provider'):raise ValueError('Invalid provider job response format')
        key=text(request['request_key'],128);ident=identity(request['job_uuid']);data=request['data'];command=request['command']
        if not isinstance(data,dict):raise ValueError('Invalid job data')
        fingerprint=canonical_request('provider_job',request)
        if len(fingerprint)>20000:raise ValueError('Job request exceeds limit')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
            if prior:
                if not matches(prior['payload'],fingerprint):raise Conflict('Job request key reused')
                result=self.snapshot(db);result['provider_job']=json.loads(prior['result'])
                current=db.execute('SELECT status FROM provider_dispatch WHERE job_uuid=?',(ident,)).fetchone()
                if current:result['provider_job']['status']=current['status']
                return provider_view(result) if response=='provider' else result
            job=db.execute('SELECT * FROM jobs WHERE uuid=?',(ident,)).fetchone()
            if not job:raise Conflict('Reserve the job in HA first')
            intent=db.execute('SELECT * FROM provider_dispatch WHERE job_uuid=?',(ident,)).fetchone()
            if command=='prepare_print':
                if set(data)-{'printer_id','allocations','print_name'}:raise ValueError('Invalid prepare data')
                physical=self.settings.get('provider_printer_id')
                if not physical or data.get('printer_id')!=physical:raise Conflict('Physical printer identity does not match HA configuration')
                native_job=next(j for j in self.snapshot(db)['native_bundle']['tables']['print_jobs'] if j['id']==ident)
                if native_job.get('printer_id')!=physical:raise Conflict('Reserved job belongs to another physical printer')
                if job['state']!='reserved' or job['settlement']:raise Conflict('Only a reserved job may be prepared')
                if intent:raise Conflict('Dispatch is already prepared; retry its original request')
                if db.execute("SELECT 1 FROM provider_dispatch WHERE status IN ('prepared','accepted','uncertain','observing')").fetchone():raise Conflict('Another dispatch requires completion or review')
                observed=db.execute("SELECT state,job_uuid FROM observations WHERE provider='duck-poop'").fetchone()
                if observed and (observed['job_uuid'] or observed['state'] in ('prepare','running','pause')):raise Conflict('Printer observer is busy')
                mapped=json.loads(job['allocations']);bindings=data.get('allocations')
                if not mapped or not isinstance(bindings,list) or len(bindings)!=len(mapped):raise Conflict('Provide every reserved filament binding')
                seen=set()
                for binding in bindings:
                    if set(binding)!={'filament_index','spool_uuid','slot','revision'} or type(binding['filament_index']) is not int or type(binding['revision']) is not int:raise ValueError('Invalid filament binding')
                    index=binding['filament_index']
                    allocation=next((a for a in mapped if a.get('filament_index')==index),None)
                    if index in seen or not allocation or allocation['spool_uuid']!=binding['spool_uuid']:raise Conflict('Reserved roll differs from project binding')
                    seen.add(index)
                    slot=db.execute('SELECT * FROM slots WHERE id=?',(binding['slot'],)).fetchone()
                    spool=db.execute('SELECT * FROM spools WHERE uuid=?',(binding['spool_uuid'],)).fetchone()
                    if not slot or slot['spool_uuid']!=binding['spool_uuid'] or slot['revision']!=binding['revision']:raise Conflict('HA slot changed before dispatch')
                    meta=json.loads(spool['data']) if spool else {}
                    if not spool or spool['remaining_mg']<=0 or meta.get('status','active')!='active' or meta['color']!=allocation['color'] or meta['material_preset']!=allocation['material_preset']:raise Conflict('Reserved material no longer matches HA roll')
                print_name=data.get('print_name',job['name']);text(print_name)
                db.execute('INSERT INTO provider_dispatch VALUES (?,?,?,?,?,?)',(ident,physical,'prepared',print_name,json.dumps(bindings),stamp()))
                result=dict(job_uuid=ident,status='prepared',printer_id=physical)
            elif command=='dispatch_result':
                if set(data)!={'outcome'} or data['outcome'] not in ('accepted','rejected','uncertain'):raise ValueError('Invalid dispatch outcome')
                if not intent or intent['status'] not in ('prepared','observing','terminal'):raise Conflict('Dispatch outcome is already recorded or no intent exists')
                if intent['status']!='prepared':
                    if data['outcome']=='rejected':raise Conflict('Printer observation already confirms the dispatch')
                    status=intent['status']
                else:status=data['outcome']
                db.execute('UPDATE provider_dispatch SET status=?,updated_at=? WHERE job_uuid=?',(status,stamp(),ident))
                if status=='rejected':
                    settlement=json.dumps(dict(outcome='failed',quality='estimated',consumption={}),sort_keys=True)
                    db.execute('UPDATE jobs SET state=?,settlement=?,updated_at=? WHERE uuid=?',('discarded',settlement,stamp(),ident))
                elif status in ('accepted','uncertain'):db.execute('UPDATE jobs SET state=?,updated_at=? WHERE uuid=? AND settlement IS NULL',('printing' if status=='accepted' else 'needs_review',stamp(),ident))
                result=dict(job_uuid=ident,status=status,printer_id=intent['printer_id'])
            elif command=='bind_external_id':
                if set(data)!={'provider','kind','value'}:raise ValueError('Invalid external job identifier')
                for value in data.values():text(value,256)
                if not intent or intent['status'] not in ('accepted','uncertain','observing','terminal'):raise Conflict('Dispatch must be accepted before external correlation')
                snapshot=self.snapshot(db);bundle=snapshot['native_bundle'];rows=bundle['tables']['job_identifiers']
                old=next((r for r in rows if all(r.get(k)==data[k] for k in ('provider','kind','value'))),None)
                if old and old['job_id']!=ident:raise Conflict('External identity already belongs to another job')
                if not old:rows.append({**data,'job_id':ident,'created_at':stamp()})
                db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
                result=dict(job_uuid=ident,status=intent['status'],printer_id=intent['printer_id'])
            else:raise ValueError('Unknown provider job command')
            self.event(db,'provider_job',dict(job_uuid=ident,command=command,result=result))
            db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(result)))
            snapshot=self.snapshot(db);snapshot['provider_job']=result
            return provider_view(snapshot) if response=='provider' else snapshot
