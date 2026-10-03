"""Native-compatible accounting from HA history; invoices are immutable snapshots."""
import copy
import json
import uuid
from datetime import date

from .native_bridge import stamp, booked_material_cost
from .receipts import canonical_request, digest, matches

CATEGORIES=('material','electricity','machine_wear','maintenance','repair_reserve','design','other')
TOTAL_FIELDS=tuple(k+'_cost_micros' for k in CATEGORIES)+(
    'estimated_material_cost_micros','total_cost_micros','billable_subtotal_micros','discount_micros','calculated_invoice_micros')
DETAIL_FIELDS=('seller_name','seller_address','seller_contact','tax_identifier','customer_name','customer_address',
    'invoice_number','invoice_date','service_date','due_date','small_business','vat_basis_points')


def _money(value):
    if type(value) is not int or abs(value)>2**63-1:raise ValueError('Cost exceeds the supported money range')
    return value


def _currency(value):
    if not isinstance(value,str) or len(value.strip())!=3:raise ValueError('Invalid cost currency')
    return value.strip().upper()


def _summary(currency):
    return dict(currency=_currency(currency),**{k:0 for k in TOTAL_FIELDS},actual_material_cost_micros=0,
        quoted_price_micros=None,invoice_amount_micros=None)


def summarize_jobs(jobs,allocations,currency):
    from .store import Conflict
    result=_summary(currency)
    for job in jobs:
        if job['state']=='discarded':continue
        if _currency(job.get('cost_currency','EUR'))!=result['currency']:
            raise Conflict('Costs in different currencies cannot be combined')
        for category in CATEGORIES[1:5]:
            field=category+'_cost_micros';result[field]=_money(result[field]+job.get(field,0))
        for allocation in allocations.get(job['id'],[]):
            if _currency(allocation.get('cost_currency','EUR'))!=result['currency']:
                raise Conflict('Allocation and print-job currencies do not match')
            estimated=allocation.get('estimated_material_cost_micros',0)
            actual=allocation.get('actual_material_cost_micros')
            result['estimated_material_cost_micros']=_money(result['estimated_material_cost_micros']+estimated)
            result['material_cost_micros']=_money(result['material_cost_micros']+(estimated if actual is None else actual))
            if result['actual_material_cost_micros'] is not None:
                result['actual_material_cost_micros']=None if actual is None else _money(result['actual_material_cost_micros']+actual)
    result['total_cost_micros']=_money(sum(result[k+'_cost_micros'] for k in CATEGORIES))
    result['billable_subtotal_micros']=result['calculated_invoice_micros']=result['total_cost_micros']
    return result


def order_summary(order,jobs,allocations):
    result=summarize_jobs(jobs,allocations,order['currency'])
    result['design_cost_micros']=_money(order.get('design_hourly_rate_micros',45000000)*order.get('design_time_seconds',0)//3600)
    result['other_cost_micros']=order.get('other_cost_micros',0)
    result['total_cost_micros']=_money(sum(result[k+'_cost_micros'] for k in CATEGORIES))
    result['billable_subtotal_micros']=_money(sum(result[k+'_cost_micros'] for k in CATEGORIES if order.get('bill_'+k,1)))
    result['discount_micros']=_money(result['billable_subtotal_micros']*order.get('discount_basis_points',0)//10000)
    result['calculated_invoice_micros']=result['billable_subtotal_micros']-result['discount_micros']
    for field in ('quoted_price_micros','invoice_amount_micros'):result[field]=order.get(field)
    return result


def invoice_lines(order,jobs,allocations,summary):
    groups={}
    for job in jobs:
        if job['state']=='discarded':continue
        for row in allocations.get(job['id'],[]):
            maker=row.get('manufacturer','');material=row.get('material_type','');preset=row.get('filament_preset_id','')
            fallback=row.get('spool_name','') if not (maker or material or preset) else ''
            key=(maker,material,preset,row.get('color_hex','#FFFFFF'),_currency(row.get('cost_currency','EUR')),fallback)
            group=groups.setdefault(key,dict(weight=0,cost=0))
            weight=row.get('estimated_weight_mg',0) if row.get('actual_weight_mg') is None else row['actual_weight_mg']
            group['weight']=_money(group['weight']+weight)
            group['cost']=_money(group['cost']+(row.get('estimated_material_cost_micros',0) if row.get('actual_material_cost_micros') is None else row['actual_material_cost_micros']))
    lines=[]
    def add(category,description,detail,amount,color=''):
        included=bool(order.get('bill_'+category,1))
        lines.append(dict(category=category,description=description,detail=detail,color_hex=color,
            internal_amount_micros=amount,invoice_amount_micros=amount if included else 0,included=included))
    for (maker,material,preset,color,_,fallback),group in sorted(groups.items()):
        description=' '.join(part for part in (maker,material) if part)
        if preset:description+=(' - ' if description else '')+preset
        add('material',description or fallback or 'Filament',f"{group['weight']/1000:.1f} g",group['cost'],color)
    for category,label,detail in (
        ('electricity','Electricity','Calculated from print runtime'),('machine_wear','Machine wear','Runtime-based allowance'),
        ('maintenance','Maintenance','Runtime-based allowance'),('repair_reserve','Repair reserve','Runtime-based allowance'),
        ('design','Design work',f"{order.get('design_time_seconds',0)/3600:.2f} h"),('other','Other costs','Order-specific costs')):
        add(category,label,detail,summary[category+'_cost_micros'])
    if summary['discount_micros']:
        lines.append(dict(category='discount',description='Discount',detail=f"{order.get('discount_basis_points',0)/100:.2f} %",
            color_hex='',internal_amount_micros=0,invoice_amount_micros=-summary['discount_micros'],included=True))
    return lines


class _Accounting:
    def __init__(self,store,db):
        self.snapshot=store.snapshot(db);self.revision=self.snapshot['revision']
        tables=self.snapshot['native_bundle']['tables']
        self.jobs={r['id']:r for r in tables['print_jobs']};self.orders={r['id']:r for r in tables['customer_orders']}
        self.customers={r['id']:r for r in tables['customers']};self.allocations={};self.jobs_by_order={}
        self.canonical_jobs={r['uuid']:r for r in self.snapshot['jobs']}
        spools={row['id']:row for row in tables['spools']}
        for booked in tables['allocations']:
            # Match native Store::read_allocation: deliberate compatible-price
            # corrections affect previews, never the ledger or saved invoices.
            row=dict(booked);spool=spools[row['spool_id']]
            price=row.get('material_price_per_kg_micros',0)
            if _currency(spool.get('price_currency','EUR'))==_currency(row.get('cost_currency','EUR')):
                price=spool.get('material_price_per_kg_micros',0)
            row['material_price_per_kg_micros']=price
            row['estimated_material_cost_micros']=_money(booked_material_cost(price,row['estimated_weight_mg']))
            row['actual_material_cost_micros']=(None if row.get('actual_weight_mg') is None else
                _money(booked_material_cost(price,row['actual_weight_mg'])))
            self.allocations.setdefault(row['job_id'],[]).append(row)
        for row in self.jobs.values():self.jobs_by_order.setdefault(row.get('customer_order_id'),[]).append(row)
        settings=tables['inventory_settings'];self.currency=settings[0]['currency'] if settings else 'EUR'
        self.invoices=[json.loads(r['data']) for r in db.execute("SELECT data FROM bridge WHERE id LIKE 'invoice:%' ORDER BY rowid")]
        defaults=db.execute("SELECT data FROM bridge WHERE id='invoice_defaults'").fetchone()
        self.defaults=json.loads(defaults['data']) if defaults else {}

    def job(self,ident):
        row=self.jobs[ident];canonical=self.canonical_jobs.get(ident,{})
        return dict(uuid=ident,customer_order_uuid=row.get('customer_order_id'),name=row['job_name'],state=row['state'],
            summary=summarize_jobs([row],self.allocations,row.get('cost_currency','EUR')),
            consumption_quality=(canonical.get('settlement') or {}).get('quality','unknown'))

    def order(self,ident):
        row=self.orders[ident];jobs=self.jobs_by_order.get(ident,[])
        summary=order_summary(row,jobs,self.allocations)
        return dict(revision=self.revision,scope='order',uuid=ident,customer_uuid=row['customer_id'],summary=summary,
            order=row,customer=self.customers[row['customer_id']],jobs=[self.job(j['id']) for j in jobs],
            invoice_lines=invoice_lines(row,jobs,self.allocations,summary),
            invoices=[i for i in self.invoices if i['order_uuid']==ident],invoice_defaults=self.defaults)

    def customer(self,ident):
        from .store import Conflict
        orders=sorted((o for o in self.orders.values() if o['customer_id']==ident),key=lambda o:(o.get('created_at',''),o['id']))
        result=_summary(orders[0]['currency'] if orders else self.currency)
        for order in orders:
            if _currency(order['currency'])!=result['currency']:raise Conflict('Customer orders in different currencies cannot be combined')
            current=order_summary(order,self.jobs_by_order.get(order['id'],[]),self.allocations)
            for field in TOTAL_FIELDS:result[field]=_money(result[field]+current[field])
            for field in ('actual_material_cost_micros',):
                if result[field] is not None:result[field]=None if current[field] is None else _money(result[field]+current[field])
            for field in ('quoted_price_micros','invoice_amount_micros'):
                if current[field] is not None:result[field]=_money((result[field] or 0)+current[field])
        return dict(revision=self.revision,scope='customer',uuid=ident,customer=self.customers[ident],summary=result)


def accounting(store,query):
    from .store import Conflict
    if set(query)-{'job_uuid','order_uuid','customer_uuid','invoice_uuid'} or len(query)>1:raise ValueError('Select one accounting record')
    with store.connection() as db:
        db.execute('BEGIN')
        if 'invoice_uuid' in query:
            from .provider import identity
            row=db.execute('SELECT data FROM bridge WHERE id=?',('invoice:'+identity(query['invoice_uuid']),)).fetchone()
            if not row:raise ValueError('Unknown invoice')
            return json.loads(row['data'])
        view=_Accounting(store,db)
        if 'job_uuid' in query:return dict(revision=view.revision,scope='job',**view.job(query['job_uuid']))
        if 'order_uuid' in query:return view.order(query['order_uuid'])
        if 'customer_uuid' in query:return view.customer(query['customer_uuid'])
        result=dict(revision=view.revision,jobs=[],orders=[],customers=[])
        for target,records,build in (('jobs',view.jobs,view.job),('orders',view.orders,view.order),('customers',view.customers,view.customer)):
            for ident,row in records.items():
                try:
                    value=build(ident)
                    result[target].append({k:value[k] for k in ('uuid','customer_order_uuid','customer_uuid','summary','consumption_quality') if k in value})
                except (Conflict,ValueError) as error:
                    result[target].append(dict(uuid=ident,summary=None,error=str(error)))
        return result


def _request(request,keys):
    from .provider import identity,text
    if not isinstance(request,dict) or set(request)!=set(keys)|{'request_key','revision'}:raise ValueError('Invalid accounting request')
    text(request['request_key'],128)
    if type(request['revision']) is not int or request['revision']<0:raise ValueError('A current revision is required')
    for field in ('job_uuid','order_uuid','customer_order_uuid','expected_order_uuid'):
        if request.get(field) is not None:identity(request[field])


def job_order(store,request):
    from .provider import indexed
    from .store import Conflict
    _request(request,('job_uuid','expected_order_uuid','customer_order_uuid'))
    fingerprint=canonical_request('job_order',request)
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(request['request_key'],)).fetchone()
        if prior:
            if not matches(prior['payload'],fingerprint):raise Conflict('Accounting request key reused')
            return json.loads(prior['result'])
        if request['revision']!=store._revision(db):raise Conflict('HA changed; reload before changing the order')
        bundle=store.snapshot(db)['native_bundle'];before=indexed(copy.deepcopy(bundle),max_rows=None)
        job=next((j for j in bundle['tables']['print_jobs'] if j['id']==request['job_uuid']),None)
        if not job:raise ValueError('Unknown print job')
        previous=job.get('customer_order_id');target=request['customer_order_uuid']
        if previous!=request['expected_order_uuid']:raise Conflict('Print job order changed; reload before saving')
        if previous!=target:
            old_order=next((o for o in bundle['tables']['customer_orders'] if o['id']==previous),None)
            if old_order and old_order.get('archived'):raise Conflict('Restore the archived customer order before editing its print jobs')
            job['customer_order_id']=target;job['updated_at']=stamp()
            overrides=bundle['tables']['print_job_manual_overrides']
            if not any(o['job_id']==job['id'] for o in overrides):overrides.append(dict(job_id=job['id'],created_at=stamp()))
            store._commit_provider_graph(db,before,bundle)
            store.event(db,'job_order',dict(job_uuid=job['id'],previous_order_uuid=previous,customer_order_uuid=target))
        result=dict(job_uuid=job['id'],customer_order_uuid=target,revision=store._revision(db))
        db.execute('INSERT INTO receipts VALUES (?,?,?)',(request['request_key'],digest(fingerprint),json.dumps(result)))
        return result


def _details(value):
    if not isinstance(value,dict) or set(value)!=set(DETAIL_FIELDS):raise ValueError('Provide all invoice details and explicit tax treatment')
    result={}
    for field in DETAIL_FIELDS:
        entry=value[field]
        if field=='small_business':
            if type(entry) is not bool:raise ValueError('Select the invoice tax treatment')
        elif field=='vat_basis_points':
            if type(entry) is not int or not 0<=entry<=10000:raise ValueError('Invalid VAT rate')
        else:
            if not isinstance(entry,str) or len(entry)>4000 or '\x00' in entry:raise ValueError('Invalid invoice '+field)
            entry=entry.strip()
            if field not in ('seller_contact','tax_identifier') and not entry:raise ValueError('Required invoice '+field)
            if field.endswith('_date'):
                if date.fromisoformat(entry).isoformat()!=entry:raise ValueError('Use ISO invoice dates')
        result[field]=entry
    if len(result['invoice_number'])>256:raise ValueError('Invoice number is too long')
    return result


def create_invoice(store,request):
    from .store import Conflict
    _request(request,('order_uuid','details'))
    details=_details(request['details']);fingerprint=canonical_request('create_invoice',request)
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(request['request_key'],)).fetchone()
        if prior:
            if not matches(prior['payload'],fingerprint):raise Conflict('Invoice request key reused')
            ident=json.loads(prior['result'])['invoice_uuid']
            return json.loads(db.execute('SELECT data FROM bridge WHERE id=?',('invoice:'+ident,)).fetchone()['data'])
        if request['revision']!=store._revision(db):raise Conflict('HA changed; reload before creating the invoice')
        view=_Accounting(store,db)
        if any(i['details']['invoice_number']==details['invoice_number'] for i in view.invoices):raise Conflict('Invoice number already exists')
        selected=view.order(request['order_uuid']);lines=selected['invoice_lines']
        net=_money(sum(line['invoice_amount_micros'] for line in lines))
        tax=0 if details['small_business'] else _money((net*details['vat_basis_points']+5000)//10000)
        ident=str(uuid.uuid4());created=stamp()
        store.event(db,'create_invoice',dict(invoice_uuid=ident,order_uuid=request['order_uuid']))
        result=dict(id=ident,created_at=created,revision=store._revision(db),order_uuid=request['order_uuid'],
            order_title=selected['order']['title'],customer_uuid=selected['customer']['id'],details=details,
            currency=selected['summary']['currency'],lines=lines,jobs=selected['jobs'],
            totals=dict(internal_cost_micros=_money(sum(line['internal_amount_micros'] for line in lines)),
                        net_micros=net,tax_micros=tax,gross_micros=_money(net+tax)))
        db.execute('INSERT INTO bridge VALUES (?,?)',('invoice:'+ident,json.dumps(result)))
        defaults={key:details[key] for key in ('seller_name','seller_address','seller_contact','tax_identifier','small_business','vat_basis_points')}
        db.execute("INSERT INTO bridge VALUES ('invoice_defaults',?) ON CONFLICT(id) DO UPDATE SET data=excluded.data",(json.dumps(defaults),))
        db.execute('INSERT INTO receipts VALUES (?,?,?)',(request['request_key'],digest(fingerprint),json.dumps(dict(invoice_uuid=ident))))
        return result
