import unittest
from datetime import datetime, timezone
import tempfile
import types
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace
from custom_components.quack_material_demo.slot_api import printer_ready, slot_state, async_setup_assignment
from custom_components.quack_material_demo.store import Store


class SlotAdapterTests(unittest.TestCase):
    def test_only_known_idle_states_and_available_service_allow_commands(self):
        states = {'sensor.print':SimpleNamespace(state='idle'), 'sensor.stage':SimpleNamespace(state='idle')}
        hass = SimpleNamespace(states=SimpleNamespace(get=states.get),
                               services=SimpleNamespace(has_service=lambda *a:True))
        self.assertTrue(printer_ready(hass, 'sensor.print', 'sensor.stage'))
        states['sensor.stage'].state='filament_loading'
        self.assertFalse(printer_ready(hass, 'sensor.print', 'sensor.stage'))
        states['sensor.stage'].state='idle'
        for value in ('running','pause','prepare','unknown','unavailable',None):
            states['sensor.print'] = SimpleNamespace(state=value)
            self.assertFalse(printer_ready(hass, 'sensor.print', 'sensor.stage'))

    def test_slot_attributes_never_override_unavailable(self):
        state = SimpleNamespace(state='unavailable', attributes={'available':True,'empty':False})
        hass = SimpleNamespace(states=SimpleNamespace(get=lambda _:state))
        self.assertFalse(slot_state(hass, 'sensor.slot')['available'])

    def test_slot_freshness_uses_ha_report_time_not_untrusted_attributes(self):
        updated=datetime(2026,10,3,10,0,tzinfo=timezone.utc)
        reported=datetime(2026,10,3,10,1,tzinfo=timezone.utc)
        state=SimpleNamespace(state='ok',attributes={'observed_at':'invented'},last_updated=updated,last_reported=reported)
        hass=SimpleNamespace(states=SimpleNamespace(get=lambda _:state))
        self.assertEqual(slot_state(hass,'sensor.slot')['observed_at'],reported.isoformat())


class SlotHttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.store=Store(Path(self.tmp.name)/'stock.db');self.views=[];self.sent=[]
        self.entities={s:'sensor.'+s.lower() for s in ('A1','A2','A3','A4','HT1','EXT')}
        self.states={e:SimpleNamespace(state='ok',attributes={'empty':False}) for e in self.entities.values()}
        self.states.update({'sensor.print':SimpleNamespace(state='idle'), 'sensor.stage':SimpleNamespace(state='idle')})
        async def executor(fn,*args):return fn(*args)
        async def send(domain,service,data,blocking):
            self.sent.append((domain,service,data))
            self.states[data['entity_id']].attributes.update(filament_id=data['tray_info_idx'],type=data['tray_type'],
                color='#'+data['tray_color'],nozzle_temp_min=data['nozzle_temp_min'],nozzle_temp_max=data['nozzle_temp_max'])
        async def publish():pass
        class View:
            def json(self,data,status_code=200):return status_code,data
        self.hass=SimpleNamespace(states=SimpleNamespace(get=self.states.get),
            services=SimpleNamespace(has_service=lambda *a:True,async_call=send),
            http=SimpleNamespace(register_view=self.views.append),async_add_executor_job=executor)
        modules={'homeassistant':types.ModuleType('homeassistant'),
            'homeassistant.components':types.ModuleType('homeassistant.components'),
            'homeassistant.components.http':SimpleNamespace(HomeAssistantView=View),
            'homeassistant.components.frontend':SimpleNamespace(add_extra_js_url=lambda *a:None),
            'homeassistant.helpers':types.ModuleType('homeassistant.helpers'),
            'homeassistant.helpers.entity_registry':SimpleNamespace(async_get=lambda _:SimpleNamespace(async_get=lambda _:SimpleNamespace(platform='bambu_lab')))}
        p=patch.dict('sys.modules',modules);p.start();self.addCleanup(p.stop)
        await async_setup_assignment(self.hass,self.store,{'mode':'pilot','printer_state_entity':'sensor.print',
            'printer_assignment':{'slots':self.entities,'stage_entity':'sensor.stage'}},publish)
        self.view=self.views[0]

    def request(self,body,admin=True,length=500):
        class Request(dict):
            content_length=length
            async def json(self):return body
        return Request(hass_user=SimpleNamespace(is_admin=admin))

    async def test_nonadmin_cannot_read_or_write_and_invalid_body_rejects(self):
        self.assertTrue(self.view.requires_auth)
        self.assertEqual((await self.view.get(self.request({},False)))[0],403)
        self.assertEqual((await self.view.post(self.request({},False)))[0],403)
        self.assertEqual((await self.view.post(self.request([])))[0],400)
        self.assertEqual((await self.view.post(self.request({},length=99999)))[0],400)
        self.assertEqual(self.sent,[])

    async def test_http_uses_backend_resolved_target_and_profile(self):
        snapshot=self.store.snapshot()
        body=dict(slot='A1',spool_uuid=snapshot['spools'][0]['uuid'],revision=0,
            inventory_revision=snapshot['revision'],request_key='http-operation-1',
            entity_id='sensor.attacker',tray_info_idx='BAD')
        code,result=await self.view.post(self.request(body))
        self.assertEqual(code,200);self.assertEqual(result['status'],'confirmed')
        self.assertEqual(self.sent[0][2]['entity_id'],'sensor.a1')
        self.assertEqual(self.sent[0][2]['tray_info_idx'],'GFA00')

    async def test_becoming_busy_when_send_is_scheduled_does_not_call_service(self):
        snapshot=self.store.snapshot()
        body=dict(slot='A1',spool_uuid=snapshot['spools'][0]['uuid'],revision=0,
            inventory_revision=snapshot['revision'],request_key='busy-before-service')
        # Inject a device update at adapter entry, after the coordinator's last check.
        from homeassistant.helpers import entity_registry
        def lookup(_):
            self.states['sensor.stage'].state='printing'
            return SimpleNamespace(async_get=lambda _:SimpleNamespace(platform='bambu_lab'))
        with patch.object(entity_registry,'async_get',side_effect=lookup):
            code,result=await self.view.post(self.request(body))
        self.assertEqual(self.sent,[])
        self.assertEqual(code,200)
        self.assertEqual(result['status'],'pending')
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])
