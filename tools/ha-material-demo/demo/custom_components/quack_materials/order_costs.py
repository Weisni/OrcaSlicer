"""Named HA order costs with a schema-8-compatible aggregate and audit history."""
import copy
import json

from .native_bridge import stamp
from .receipts import canonical_request, digest, matches

LIMIT=10**12


def cost_key(ident):return 'order-costs:'+ident


def normalize_items(items):
    from .provider import identity
    if not isinstance(items,list) or len(items)>100:raise ValueError('Provide at most 100 cost items')
    result=[];seen=set()
    for item in items:
        if not isinstance(item,dict) or set(item)!={'id','description','quantity_milli','unit_price_micros'}:
            raise ValueError('Invalid cost item fields')
        ident=identity(item['id']);description=item['description']
        if ident in seen:raise ValueError('Duplicate cost item identity')
        seen.add(ident)
        if not isinstance(description,str) or not description.strip() or len(description.strip())>256:
            raise ValueError('Provide a cost item description up to 256 characters')
        quantity=item['quantity_milli'];price=item['unit_price_micros']
        if type(quantity) is not int or not 1<=quantity<=10**9:raise ValueError('Invalid cost item quantity')
        if type(price) is not int or not 0<=price<=LIMIT:raise ValueError('Invalid cost item unit value')
        # Per-line cent rounding, including half cents, using integer arithmetic.
        amount=((quantity*price+5000000)//10000000)*10000
        if amount>LIMIT:raise ValueError('Cost item total exceeds the supported range')
        result.append(dict(id=ident,description=description.strip(),quantity_milli=quantity,
            unit_price_micros=price,total_micros=amount))
    if sum(row['total_micros'] for row in result)>LIMIT:raise ValueError('Cost items exceed the supported range')
    return result


def read_order_costs(db,order):
    row=db.execute('SELECT data FROM bridge WHERE id=?',(cost_key(order['id']),)).fetchone()
    if row:
        result=json.loads(row['data'])
    else:
        result=dict(schema_version=1,order_uuid=order['id'],currency=order['currency'],
            legacy_cost_micros=order.get('other_cost_micros',0),items=[])
    result['total_cost_micros']=result['legacy_cost_micros']+sum(item['total_micros'] for item in result['items'])
    return result


def sync_order_cost_totals(db,orders):
    """Keep named positions when native clients adjust the aggregate lump sum."""
    from .store import Conflict
    by_id={order['id']:order for order in orders}
    for row in db.execute("SELECT id,data FROM bridge WHERE id LIKE 'order-costs:%'").fetchall():
        costs=json.loads(row['data']);order=by_id.get(costs['order_uuid'])
        if not order:raise Conflict('Archive orders with named costs instead of deleting them')
        if order['currency']!=costs['currency']:raise Conflict('Named order costs cannot be reinterpreted in another currency')
        total=order.get('other_cost_micros',0);positions=sum(item['total_micros'] for item in costs['items'])
        if type(total) is not int or total<positions or total>LIMIT:
            raise Conflict('Other cost total cannot be below its named positions; edit the positions in HA')
        if costs['legacy_cost_micros']!=total-positions:
            costs['legacy_cost_micros']=total-positions
            db.execute('UPDATE bridge SET data=? WHERE id=?',(json.dumps(costs),row['id']))


def save_order_cost_items(store,request):
    from .store import Conflict
    from .provider import identity,text
    if not isinstance(request,dict) or set(request)!={'request_key','revision','order_uuid','items'}:
        raise ValueError('Invalid order cost request')
    key=text(request['request_key'],128);ident=identity(request['order_uuid'])
    if type(request['revision']) is not int or request['revision']<0:raise ValueError('A fresh revision is required')
    items=normalize_items(request['items']);fingerprint=canonical_request('order_cost_items',request)
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        receipt=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
        if receipt:
            if not matches(receipt['payload'],fingerprint):raise Conflict('Order cost request key reused')
            return json.loads(receipt['result'])
        if request['revision']!=store._revision(db):raise Conflict('HA changed; reload before editing order costs')
        native=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
        if not native:raise ValueError('Unknown order')
        bundle=json.loads(native['data'])
        order=next((row for row in bundle['tables']['customer_orders'] if row['id']==ident),None)
        if not order:raise ValueError('Unknown order')
        if order.get('archived'):raise Conflict('Restore the order before editing its costs')
        before=read_order_costs(db,order);after=copy.deepcopy(before);after['items']=items
        amount=before['legacy_cost_micros']+sum(item['total_micros'] for item in items)
        if not 0<=amount<=LIMIT:raise ValueError('Order cost total exceeds the supported range')
        after.pop('total_cost_micros',None)
        order['other_cost_micros']=amount;order['updated_at']=stamp()
        db.execute('INSERT INTO bridge VALUES (?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data',
            (cost_key(ident),json.dumps(after)))
        db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
        store.event(db,'order_cost_items',dict(order_uuid=ident,before=before,after={**after,'total_cost_micros':amount}))
        result=dict(accepted=True,order_uuid=ident,revision=store._revision(db))
        db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(result)))
        return result
