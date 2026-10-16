"""Record current order costs as paid and archive without formal invoice fields."""
import copy
import json
from .native_bridge import stamp
from .receipts import canonical_request,digest,matches


def read_payment(db,ident):
    row=db.execute('SELECT data FROM bridge WHERE id=?',('order-payment:'+ident,)).fetchone()
    return json.loads(row['data']) if row else None


def mark_order_paid(store,request):
    from .accounting import _Accounting,_request,_money
    from .store import Conflict
    _request(request,('order_uuid',))
    fingerprint=canonical_request('order_paid',request);key=request['request_key'];ident=request['order_uuid']
    with store.connection() as db:
        db.execute('BEGIN IMMEDIATE')
        receipt=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
        if receipt:
            if not matches(receipt['payload'],fingerprint):raise Conflict('Payment request key reused')
            return json.loads(receipt['result'])
        if request['revision']!=store._revision(db):raise Conflict('Costs changed; reopen the cost statement before marking paid')
        view=_Accounting(store,db)
        if ident not in view.orders:raise ValueError('Unknown order')
        selected=view.order(ident)
        if selected['order'].get('archived'):raise Conflict('Restore the order before marking it paid')
        if selected['payment']:raise Conflict('This order is already paid; its payment snapshot is preserved')
        amount=_money(selected['summary']['calculated_invoice_micros'])
        if amount<0:raise ValueError('A paid cost statement must have a nonnegative total')
        if sum(line['invoice_amount_micros'] for line in selected['invoice_lines'])!=amount:
            raise Conflict('Cost statement lines do not match the calculated total')
        payment=dict(schema_version=1,order_uuid=ident,order_title=selected['order']['title'],
            customer_uuid=selected['customer']['id'],customer_name=selected['customer']['name'],
            currency=selected['summary']['currency'],amount_micros=amount,paid_at=stamp(),
            summary=copy.deepcopy(selected['summary']),lines=copy.deepcopy(selected['invoice_lines']))
        row=db.execute("SELECT data FROM bridge WHERE id='native'").fetchone()
        bundle=json.loads(row['data'])
        order=next(o for o in bundle['tables']['customer_orders'] if o['id']==ident)
        order.update(archived=1,updated_at=payment['paid_at'])
        db.execute("UPDATE bridge SET data=? WHERE id='native'",(json.dumps(bundle),))
        db.execute('INSERT INTO bridge VALUES (?,?)',('order-payment:'+ident,json.dumps(payment)))
        store.event(db,'order_paid',dict(order_uuid=ident,amount_micros=amount,currency=payment['currency'],archived=True))
        result=dict(accepted=True,order_uuid=ident,archived=True,payment=payment,revision=store._revision(db))
        db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,digest(fingerprint),json.dumps(result)))
        return result
