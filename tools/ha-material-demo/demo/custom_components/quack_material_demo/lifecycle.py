"""Roll lifecycle commands, sharing Store transactions and identity."""
import json
import re
import uuid
from datetime import datetime, timezone


def stamp(): return datetime.now(timezone.utc).isoformat()


class Lifecycle:
    def _assert_unreserved(self, db, ident):
        from .store import Conflict
        for j in db.execute('SELECT allocations FROM jobs WHERE settlement IS NULL'):
            if any(a['spool_uuid']==ident for a in json.loads(j['allocations'])):
                raise Conflict('Roll has an unsettled print job')

    def lifecycle(self, action, request):
        from .store import Conflict, weight
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            key=request.get('request_key')
            fingerprint=json.dumps(dict(action=action,data=request),sort_keys=True)
            if key:
                prior=db.execute('SELECT * FROM receipts WHERE request_key=?',(key,)).fetchone()
                if prior:
                    if prior['payload']!=fingerprint: raise Conflict('Request key reused with different data')
                    return json.loads(prior['result'])
            if 'revision' in request and request['revision']!=self._revision(db): raise Conflict('HA changed; reload before saving')
            if action=='create':
                ident=str(uuid.uuid4())  # Client never supplies a manually entered UUID.
                data=dict(request); data.pop('request_key',None); data.pop('uuid',None); data.pop('revision',None)
                for field in ('product','manufacturer','material_type'):
                    if not isinstance(data.get(field),str) or not data[field].strip() or len(data[field])>256:
                        raise ValueError('Provide '+field)
                preset=data.get('material_preset','')
                if not isinstance(preset,str) or len(preset)>256:raise ValueError('Invalid material_preset')
                data.update(material_preset=preset.strip(),profile_unresolved=not bool(preset.strip()))
                if self.settings.get('mode')!='pilot' and data['material_type'] not in ('PLA','PETG'): raise ValueError('Demo supports PLA/PETG')
                if not re.fullmatch(r'#[0-9a-fA-F]{6}',data.get('color','')): raise ValueError('Use #RRGGBB color')
                amount=weight(data.pop('remaining_mg'))
                data['nominal_mg']=weight(data.get('nominal_mg',1000000))
                data['material_price_per_kg_micros']=weight(data.get('material_price_per_kg_micros',20000000))
                if not data['nominal_mg'] or amount>data['nominal_mg']: raise ValueError('Fill must fit nominal capacity')
                if not 0.1<=float(data.get('diameter_mm',1.75))<=10 or not 0.1<=float(data.get('density_g_cm3',1.26))<=10:
                    raise ValueError('Invalid diameter or density')
                data.update(uuid=ident,demo=self.settings.get('mode')!='pilot',bambu_material='Bambu '+data['material_type'] if data['material_type'] in ('PLA','PETG') else None,
                    status='active' if amount else 'empty',weight_quality='estimated',preset_revision='installed-local',created_at=stamp())
                db.execute('INSERT INTO spools VALUES (?,?,?)',(ident,json.dumps(data),amount))
            else:
                ident=request['spool_uuid']; row=db.execute('SELECT * FROM spools WHERE uuid=?',(ident,)).fetchone()
                if not row: raise ValueError('Unknown roll UUID')
                self._assert_unreserved(db,ident)
                data=json.loads(row['data']); amount=row['remaining_mg']
                if action=='archive': data['status']='archived'
                elif action=='restore': data['status']='active' if amount else 'empty'
                elif action=='weigh':
                    amount=weight(request['remaining_mg'])
                    if amount>data.get('nominal_mg',1000000): raise ValueError('Measured fill exceeds nominal capacity')
                    if data.get('status')=='archived': raise Conflict('Restore archived roll before weighing')
                    data.update(status='active' if amount else 'empty',weight_quality='measured')
                elif action=='edit':
                    for field in ('product','manufacturer','material_preset'):
                        if field in request:
                            if not isinstance(request[field],str) or (field!='material_preset' and not request[field].strip()) or len(request[field])>256: raise ValueError('Invalid '+field)
                            data[field]=request[field].strip()
                    if 'color' in request:
                        if not re.fullmatch(r'#[0-9a-fA-F]{6}',request['color']):raise ValueError('Invalid color')
                        data['color']=request['color']
                    for field in ('density_g_cm3','diameter_mm'):
                        if field in request:
                            value=float(request[field])
                            if not 0.1<=value<=10:raise ValueError('Invalid '+field)
                            data[field]=value
                    for field in ('material_price_per_kg_micros','nominal_mg'):
                        if field in request:
                            value=weight(request[field])
                            if field=='nominal_mg' and (value<amount or not value):raise ValueError('Nominal fill below remaining stock')
                            data[field]=value
                    if 'material_type' in request:
                        value=request['material_type']
                        if not isinstance(value,str) or not value.strip() or len(value)>40:raise ValueError('Invalid material type')
                        data['material_type']=value.strip()
                        data['bambu_material']='Bambu '+value if value in ('PLA','PETG') else None
                    data['profile_unresolved']=not bool(data.get('material_preset'))
                else: raise ValueError('Unknown lifecycle action')
                db.execute('UPDATE spools SET data=?,remaining_mg=? WHERE uuid=?',(json.dumps(data),amount,ident))
                if data.get('status') in ('archived','empty'):
                    db.execute('UPDATE slots SET spool_uuid=NULL,revision=revision+1 WHERE spool_uuid=?',(ident,))
                else: db.execute('UPDATE slots SET revision=revision+1 WHERE spool_uuid=?',(ident,))
            event=dict(spool_uuid=ident,remaining_mg=amount)
            if action in ('create','weigh'):
                event.update(delta_mg=amount-(row['remaining_mg'] if action=='weigh' else 0),quality='measured' if action=='weigh' else 'estimated')
            self.event(db,action,event)
            result=dict(uuid=ident,qr_payload='quackslicer://spool/'+ident,status=data['status'])
            if key: db.execute('INSERT INTO receipts VALUES (?,?,?)',(key,fingerprint,json.dumps(result)))
            return result
