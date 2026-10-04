"""Central pilot management, preserving the existing schema-8 mirror."""
import copy
import json
import uuid
from urllib.parse import urlsplit
from .native_bridge import TABLES, stamp


def empty_bundle():
    return {'schema_version':8,'tables':{k:[] for k in TABLES}}


class Pilot:
    def management_snapshot(self):
        with self.connection() as db:
            db.execute('BEGIN')
            data=self.snapshot(db);bundle=data.pop('native_bundle',None) or empty_bundle()
            tables=bundle['tables']
            data.update(customers=tables['customers'],orders=tables['customer_orders'],stock_events=tables['stock_events'],
                        inventory_settings=tables['inventory_settings'],mode=self.settings.get('mode','demo'),
                        printer_commands_enabled=False,physical_printer_id=self.settings.get('provider_printer_id'),profile_catalog=self.settings.get('profile_catalog',[]),
                        observer_status=self.settings.get('printer_state_entity','Not configured'))
            imported=db.execute("SELECT payload FROM receipts WHERE request_key='production-import'").fetchone()
            last=db.execute("SELECT created_at FROM events WHERE kind IN ('native_sync','native_apply','provider_apply','provider_delta') ORDER BY id DESC LIMIT 1").fetchone()
            data['import_source']=json.loads(imported['payload']) if imported else None
            data['last_quack_sync']=last['created_at'] if last else None
            terminal={}
            for event in db.execute("SELECT data FROM events WHERE kind='observed_terminal' ORDER BY id"):
                observed=json.loads(event['data']);terminal[observed.get('uuid')]=observed.get('outcome')
            dispatch={r['job_uuid']:r['status'] for r in db.execute('SELECT job_uuid,status FROM provider_dispatch')}
            for job in data['jobs']:
                job['observed_outcome']=terminal.get(job['uuid'])
                job['can_reconcile_provider']=bool(job['source']=='quack_provider' and job['state']=='needs_review' and
                    not job['settlement'] and job['allocations'] and dispatch.get(job['uuid'])=='terminal' and
                    job['observed_outcome'] in ('failed','finish'))
        return data

    def import_inventory(self,bundle,fingerprint,profiles=None):
        from .store import Conflict
        if self.settings.get('mode')!='pilot': raise Conflict('Production import requires pilot mode')
        with self.connection() as db:
            prior=db.execute("SELECT payload FROM receipts WHERE request_key='production-import'").fetchone()
            if prior:
                if json.loads(prior['payload'])['fingerprint']==fingerprint: return self.management_snapshot()
                raise Conflict('Another inventory was imported; preserve it and reconcile')
            if db.execute('SELECT 1 FROM spools').fetchone() or db.execute('SELECT 1 FROM jobs').fetchone() or db.execute('SELECT 1 FROM bridge').fetchone():
                raise Conflict('Import requires a fresh staging database')
        # This is an offline staging operation; activate only after complete audit.
        self.sync_native({'bundle':copy.deepcopy(bundle),'revision':self.snapshot()['revision']})
        profiles=profiles or {}
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            for row in db.execute('SELECT uuid,data FROM spools').fetchall():
                item=json.loads(row['data']);legacy=item['material_preset']
                item.update(legacy_preset_id=legacy,material_preset=profiles.get(legacy,''),profile_unresolved=not bool(profiles.get(legacy)),demo=False)
                db.execute('UPDATE spools SET data=? WHERE uuid=?',(json.dumps(item),row['uuid']))
            info={'fingerprint':fingerprint,'schema_version':8,'imported_at':stamp(),'counts':{t:len(bundle['tables'][t]) for t in TABLES}}
            db.execute('INSERT INTO receipts VALUES (?,?,?)',('production-import',json.dumps(info),'{}'))
            self.event(db,'production_import',info)
        return self.management_snapshot()

    def manage_record(self,kind,request):
        from .store import Conflict
        if kind not in ('customer','order'): raise ValueError('Invalid record kind')
        key=request.get('request_key');fingerprint=json.dumps({'kind':kind,'data':request},sort_keys=True)
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            if key:
                prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
                if prior:
                    if prior['payload']!=fingerprint: raise Conflict('Request key reused for another record')
                    return json.loads(prior['result'])
            if 'revision' in request and request['revision']!=self._revision(db): raise Conflict('HA changed; reload before saving')
            bridge=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
            bundle=json.loads(bridge['data']) if bridge else empty_bundle();t=bundle['tables'];table='customers' if kind=='customer' else 'customer_orders'
            ident=request.get('uuid') or str(uuid.uuid4());record=next((r for r in t[table] if r['id']==ident),None)
            if request.get('uuid') and record is None: raise ValueError('Unknown record UUID')
            created=record is None
            if created:
                record={'id':ident,'archived':0,'created_at':stamp(),'updated_at':stamp()}
                if kind=='customer':record.update(name='',contact_name='',email='',phone='',notes='')
                else:
                    record.update(customer_id='',order_number='',title='',notes='',quoted_price_micros=None,invoice_amount_micros=None,currency='EUR',
                        design_time_seconds=0,design_hourly_rate_micros=45000000,other_cost_micros=0,discount_basis_points=0,
                        **{'bill_'+k:1 for k in ('material','electricity','machine_wear','maintenance','repair_reserve','design','other')},status='active')
                t[table].append(record)
            strings=('name','contact_name','email','phone','notes') if kind=='customer' else ('customer_id','order_number','title','notes','currency')
            for field in strings:
                if field in request:
                    value=request[field]
                    if not isinstance(value,str) or len(value)>(4000 if field=='notes' else 256):raise ValueError('Invalid '+field)
                    record[field]=value.strip()
            if request.get('archive') is not None:record['archived']=int(bool(request['archive']))
            if request.get('restore'):record['archived']=0
            if kind=='customer':
                if not record['name']:raise ValueError('Customer name required')
            else:
                customer=next((c for c in t['customers'] if c['id']==record['customer_id']),None)
                if customer is None or (customer.get('archived') and created):raise Conflict('Select an existing active customer')
                if not record['title']:raise ValueError('Order title required')
                if 'status' in request:
                    if request['status'] not in ('draft','active','completed','cancelled'):raise ValueError('Invalid order status')
                    record['status']=request['status']
                for field in ('quoted_price_micros','invoice_amount_micros','design_time_seconds','design_hourly_rate_micros','other_cost_micros','discount_basis_points'):
                    if field in request:
                        value=request[field]
                        if value is None and field in ('quoted_price_micros','invoice_amount_micros'):record[field]=None;continue
                        if type(value)!=int or value<0 or value>10**12:raise ValueError('Invalid '+field)
                        if field=='discount_basis_points' and value>10000:raise ValueError('Discount exceeds 100%')
                        record[field]=value
                for field in ('bill_material','bill_electricity','bill_machine_wear','bill_maintenance','bill_repair_reserve','bill_design','bill_other'):
                    if field in request:record[field]=int(bool(request[field]))
            record['updated_at']=stamp()
            db.execute("INSERT INTO bridge VALUES ('native',?) ON CONFLICT(id) DO UPDATE SET data=excluded.data",(json.dumps(bundle),))
            self.event(db,kind,{'uuid':ident,'created':created,'archived':record['archived']})
            result={'uuid':ident,'revision':self._revision(db)}
            if key:db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,fingerprint,json.dumps(result)))
            return result

    def reconcile_observed(self,request):
        from .store import Conflict,weight
        key=request['request_key'];fingerprint=json.dumps(request,sort_keys=True)
        amount=weight(request['consumed_mg']);outcome=request['outcome'];quality=request['quality']
        if outcome not in ('completed','failed') or quality not in ('estimated','measured'):raise ValueError('Explicit outcome and quantity quality required')
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
            if prior:
                if prior['payload']!=fingerprint:raise Conflict('Reconciliation key reused')
                return json.loads(prior['result'])
            if request.get('revision')!=self._revision(db):raise Conflict('HA changed; reload before reconciliation')
            job=db.execute('SELECT * FROM jobs WHERE uuid=?',(request['job_uuid'],)).fetchone()
            origin=db.execute('SELECT source FROM origins WHERE job_uuid=?',(request['job_uuid'],)).fetchone()
            if not job or not origin or origin['source']!='printer_observation' or job['state']!='needs_review' or job['settlement']:raise Conflict('Only an unsettled terminal observed attempt can be reconciled')
            roll=db.execute('SELECT * FROM spools WHERE uuid=?',(request['spool_uuid'],)).fetchone()
            if not roll:raise ValueError('Unknown roll UUID')
            meta=json.loads(roll['data']);self._assert_unreserved(db,roll['uuid'])
            if meta.get('status')=='archived' or amount>roll['remaining_mg']:raise Conflict('Archived roll or insufficient stock')
            order=request.get('customer_order_uuid')
            if order:
                row=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
                if not row or not any(o['id']==order for o in json.loads(row['data'])['tables']['customer_orders']):raise ValueError('Unknown order')
                db.execute('INSERT OR REPLACE INTO joblinks VALUES (?,?)',(job['uuid'],order))
            allocation={'slot':request.get('slot','unassigned'),'spool_uuid':roll['uuid'],'weight_mg':amount,'material_preset':meta['material_preset'],'color':meta['color'],'binding_revision':0}
            settlement={'outcome':outcome,'quality':quality,'consumption':{roll['uuid']:amount}}
            remaining=roll['remaining_mg']-amount;meta['weight_quality']=quality
            if not remaining:meta['status']='empty';db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?',(roll['uuid'],))
            db.execute('UPDATE spools SET data=?,remaining_mg=? WHERE uuid=?',(json.dumps(meta),remaining,roll['uuid']))
            db.execute('UPDATE jobs SET allocations=?,settlement=?,state=?,updated_at=? WHERE uuid=?',(json.dumps([allocation]),json.dumps(settlement),outcome,stamp(),job['uuid']))
            self.event(db,'job_settlement',{'uuid':job['uuid'],'settlement':settlement})
            result={'uuid':job['uuid'],'state':outcome};db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,fingerprint,json.dumps(result)))
            return result

    def label_payload(self,spool_uuid,target='web'):
        if target=='app':
            return 'homeassistant://navigate/dashboard-filament/rolls?spool='+spool_uuid
        if target!='web':raise ValueError('Invalid label target')
        base=self.settings.get('base_url','').rstrip('/')
        if base:
            parts=urlsplit(base)
            if parts.scheme not in ('http','https') or not parts.netloc or parts.username or parts.password or parts.query or parts.fragment:raise ValueError('Invalid label base URL')
            return base+'/dashboard-filament/rolls?spool='+spool_uuid
        return 'quackslicer://spool/'+spool_uuid
