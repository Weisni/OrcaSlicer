import asyncio
import tempfile
import unittest
from pathlib import Path

from custom_components.quack_material_demo.store import Store, Conflict
from custom_components.quack_material_demo.slot_assignment import AssignmentCoordinator, CommandNotSent


class AssignmentTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.tmp.name) / 'stock.db')
        self.roll = self.store.snapshot()['spools'][0]
        self.config = {'slots': {s: 'sensor.' + s.lower() for s in ('A1','A2','A3','A4','HT1','EXT')}}
        self.states = {s: {'empty': False, 'available': True} for s in self.config['slots']}
        self.sent = []
        self.respond = True
        async def send(payload):
            self.sent.append(payload)
            if self.respond:
                self.states[self.current_slot].update(filament_id=payload['tray_info_idx'],
                    color='#'+payload['tray_color'], type=payload['tray_type'],
                    nozzle_temp_min=payload['nozzle_temp_min'], nozzle_temp_max=payload['nozzle_temp_max'])
        async def run(fn, *args): return fn(*args)
        self.ready = True
        self.current_slot = 'A1'
        self.coordinator = AssignmentCoordinator(self.store, self.config, send,
            lambda s: self.states[s], lambda: self.ready, run, timeout=0.01, interval=0.001)

    async def asyncTearDown(self): self.tmp.cleanup()

    def request(self, **changes):
        return dict(slot='A1', spool_uuid=self.roll['uuid'], revision=0,
                    inventory_revision=self.store.snapshot()['revision'], request_key='test-operation-0001', **changes)

    async def test_confirmed_assignment_preserves_real_roll(self):
        result = await self.coordinator.assign(self.request())
        self.assertEqual(result['status'], 'confirmed')
        self.assertEqual(self.store.snapshot()['slots'][0]['spool_uuid'], self.roll['uuid'])
        self.assertEqual(self.sent[0]['tray_info_idx'], 'GFA00')
        self.assertEqual(self.sent[0]['tray_color'], self.roll['color'][1:].upper()+'FF')
        self.assertEqual(self.store.snapshot()['spools'][0]['material_preset'], self.roll['material_preset'])

    async def test_subset_reconciles_only_enabled_slots_and_rejects_disabled_target(self):
        self.store=Store(self.store.path,seed_demo=False,settings={'enabled_slots':['A1']})
        self.store.assign('A1',self.roll['uuid'],0)
        reads=[]
        def read(slot):
            reads.append(slot)
            return self.states[slot]
        coordinator=AssignmentCoordinator(self.store,self.config,self.coordinator.send,read,
            lambda:True,self.coordinator.executor,timeout=0.01,interval=0.001)
        await coordinator.reconcile_metadata()
        self.assertEqual(set(reads),{'A1'})
        self.assertEqual(set(coordinator.config['slots']),{'A1'})
        reads.clear();sent=len(self.sent)
        request=self.request();request['slot']='A2'
        with self.assertRaises((Conflict,ValueError)):await coordinator.assign(request)
        self.assertEqual(reads,[])
        self.assertEqual(len(self.sent),sent)

    async def test_timeout_is_durable_and_retry_does_not_resend_implicitly(self):
        self.respond = False
        request = self.request()
        result = await self.coordinator.assign(request)
        self.assertEqual(result['status'], 'pending')
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])
        await self.coordinator.assign(request)
        self.assertEqual(len(self.sent), 1)
        reopened = Store(self.store.path, seed_demo=False)
        self.assertIsNotNone(self.coordinator.pending())
        self.assertEqual(reopened.snapshot()['slots'][0]['spool_uuid'], None)
        self.respond = True
        result = await self.coordinator.assign(dict(request, retry=True))
        self.assertEqual(result['status'], 'confirmed')
        self.assertEqual(len(self.sent), 2)

    async def test_duplicate_request_is_idempotent_and_bound_to_payload(self):
        request = self.request()
        await self.coordinator.assign(request)
        await self.coordinator.assign(request)
        self.assertEqual(len(self.sent), 1)
        with self.assertRaises(Conflict):
            await self.coordinator.assign(dict(request, slot='A2'))

    async def test_busy_or_empty_slot_rejects_without_command(self):
        self.ready = False
        with self.assertRaises(Conflict): await self.coordinator.assign(self.request())
        self.ready = True
        self.states['A1']['empty'] = True
        with self.assertRaises(Conflict): await self.coordinator.assign(self.request())
        self.assertEqual(self.sent, [])

    async def test_stale_revision_and_archived_roll_reject(self):
        request = self.request()
        self.store.assign('A2', self.roll['uuid'], 0)
        with self.assertRaises(Conflict): await self.coordinator.assign(request)
        self.assertEqual(self.sent, [])

    async def test_pending_prevents_other_operation(self):
        self.respond = False
        await self.coordinator.assign(self.request())
        with self.assertRaises(Conflict):
            await self.coordinator.assign(dict(self.request(), request_key='another-operation'))
        self.assertEqual(len(self.sent), 1)

    async def test_wrong_readback_does_not_commit(self):
        self.respond = False
        self.states['A1'].update(filament_id='GFA00', color=self.roll['color'], type='PLA',
                                  nozzle_temp_min=190, nozzle_temp_max=999)
        result = await self.coordinator.assign(self.request())
        self.assertEqual(result['status'], 'pending')
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])

    async def test_restart_recovers_without_implicit_send(self):
        self.respond = False
        request = self.request()
        await self.coordinator.assign(request)
        replacement = AssignmentCoordinator(Store(self.store.path, seed_demo=False), self.config,
            self.coordinator.send, lambda s:self.states[s], lambda:True,
            self.coordinator.executor, timeout=0.01, interval=0.001)
        self.assertEqual(replacement.pending()['request_key'], request['request_key'])
        await replacement.assign(request)
        self.assertEqual(len(self.sent), 1)

    async def test_unrelated_observer_event_does_not_make_recovery_impossible(self):
        self.respond = False
        request = self.request()
        await self.coordinator.assign(request)
        with self.store.connection() as db:
            self.store.event(db, 'observation', {'state':'idle'})
        self.respond = True
        self.assertEqual((await self.coordinator.assign(dict(request, retry=True)))['status'], 'confirmed')

    async def test_roll_change_before_retry_does_not_send_stale_material(self):
        self.respond = False
        request = self.request()
        await self.coordinator.assign(request)
        with self.store.connection() as db:
            import json
            roll = dict(self.roll, color='#000000')
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(roll), self.roll['uuid']))
        with self.assertRaises(Conflict): await self.coordinator.assign(dict(request, retry=True))
        self.assertEqual(len(self.sent), 1)

    async def test_printer_becomes_busy_during_preparation_never_sends(self):
        original = self.coordinator.executor
        async def run(fn, *args):
            result = await original(fn, *args)
            if fn.__name__ == 'prepare': self.ready=False
            return result
        self.coordinator.executor=run
        with self.assertRaises(Conflict): await self.coordinator.assign(self.request())
        self.assertEqual(self.sent, [])

    async def test_cancel_exhausted_pending_roll_clears_binding_without_device_command(self):
        self.respond=False
        request=self.request()
        await self.coordinator.assign(request)
        with self.store.connection() as db:
            db.execute('UPDATE spools SET remaining_mg=0 WHERE uuid=?',(self.roll['uuid'],))
        result=await self.coordinator.assign(dict(request,cancel=True))
        self.assertEqual(result['status'],'cancelled')
        self.assertIsNone(self.coordinator.pending())
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])
        self.assertEqual(len(self.sent),1)
        self.assertEqual((await self.coordinator.assign(request))['status'],'cancelled')
        self.assertEqual(len(self.sent),1)

    async def test_confirmed_replay_works_when_printer_later_busy(self):
        request=self.request()
        await self.coordinator.assign(request)
        self.ready=False
        self.assertEqual((await self.coordinator.assign(request))['status'],'confirmed')

    async def test_cancel_after_observer_has_already_cleared_slot(self):
        self.respond=False
        self.store.assign('A1',self.roll['uuid'],0)
        request=dict(self.request(),revision=1)
        await self.coordinator.assign(request)
        self.store.observe_printer('running','Observed job')
        self.store.observe_printer('finish','Observed job')
        snapshot=self.store.snapshot()
        self.store.reconcile_observed(dict(job_uuid=snapshot['jobs'][0]['uuid'],spool_uuid=self.roll['uuid'],
            slot='A1',consumed_mg=self.roll['remaining_mg'],outcome='completed',quality='measured',
            revision=snapshot['revision'],request_key='consume-bound-roll'))
        result=await self.coordinator.assign(dict(request,cancel=True))
        self.assertEqual(result['status'],'cancelled')
        self.assertEqual(self.store.snapshot()['slots'][0]['revision'],2)
        self.assertIsNone(self.coordinator.pending())

    async def test_fresh_rounded_color_confirms_material_but_preserves_ha_rgb(self):
        self.states['A1'].update(filament_id='GFA00', color='#3366FF', type='PLA',
            nozzle_temp_min=190, nozzle_temp_max=240, observed_at='before')
        async def rounded(payload):
            self.sent.append(payload)
            self.states['A1']['observed_at']='after'
        self.coordinator.send=rounded
        result=await self.coordinator.assign(self.request())
        self.assertEqual(result['status'],'confirmed')
        self.assertEqual(result['color_status'],'approximate')
        self.assertEqual(result['requested_color'],'#367AF5')
        self.assertEqual(result['reported_color'],'#3366FF')
        self.assertEqual(len(self.sent),1)
        self.assertEqual(self.store.snapshot()['spools'][0]['color'],'#367AF5')

    async def test_cached_rounded_color_does_not_claim_fresh_confirmation(self):
        self.states['A1'].update(filament_id='GFA00', color='#3366FF', type='PLA',
            nozzle_temp_min=190, nozzle_temp_max=240, observed_at='before')
        self.respond=False
        result=await self.coordinator.assign(self.request())
        self.assertEqual(result['status'],'pending')
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])

    async def test_assignment_busy_after_journaling_clears_unsent_ack_evidence(self):
        self.states['A1'].update(filament_id='GFA00',color='#3366FF',type='PLA',
            nozzle_temp_min=190,nozzle_temp_max=240,observed_at='before')
        original=self.coordinator.executor
        async def run(fn,*args):
            result=await original(fn,*args)
            if fn.__name__=='save_assignment':self.ready=False
            return result
        self.coordinator.executor=run
        request=self.request()
        result=await self.coordinator.assign(request)
        self.assertEqual(result['status'],'pending')
        self.coordinator.executor=original
        self.ready=True
        self.states['A1']['observed_at']='later'
        result=await self.coordinator.assign(request)
        self.assertEqual(result['status'],'pending')
        self.assertEqual(self.sent,[])
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])

    async def test_metadata_refresh_sends_ha_color_once_and_persists_approximation(self):
        self.store.assign('A1',self.roll['uuid'],0)
        self.states['A1'].update(filament_id='GFA00', color='#3366FF', type='PLA',
            nozzle_temp_min=190, nozzle_temp_max=240, observed_at='before')
        async def rounded(payload):
            self.sent.append(payload)
            self.states['A1']['observed_at']='after'
        self.coordinator.send=rounded
        before=self.store.snapshot()
        await self.coordinator.reconcile_metadata()
        await self.coordinator.reconcile_metadata()
        self.assertEqual(len(self.sent),1)
        state=self.coordinator.metadata_status()['A1']
        self.assertEqual(state['color_status'],'approximate')
        self.assertEqual(state['requested_color'],'#367AF5')
        self.assertEqual(state['reported_color'],'#3366FF')
        self.assertEqual(self.store.snapshot()['slots'],before['slots'])
        self.assertEqual(self.store.snapshot()['spools'],before['spools'])
        replacement=AssignmentCoordinator(Store(self.store.path,seed_demo=False),self.config,
            rounded,lambda slot:self.states[slot],lambda:True,self.coordinator.executor)
        await replacement.reconcile_metadata()
        self.assertEqual(len(self.sent),1)

    async def test_metadata_changes_wait_for_idle_then_send_latest_color_only(self):
        import json
        with self.store.connection() as db:
            db.execute('UPDATE spools SET data=? WHERE uuid=?', (json.dumps(dict(self.roll,status='active')),self.roll['uuid']))
        self.store.assign('A1',self.roll['uuid'],0)
        self.ready=False
        await self.coordinator.reconcile_metadata()
        self.store.lifecycle('edit',dict(spool_uuid=self.roll['uuid'],color='#FF0000'))
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.sent,[])
        self.ready=True
        await self.coordinator.reconcile_metadata()
        self.assertEqual(len(self.sent),1)
        self.assertEqual(self.sent[0]['tray_color'],'FF0000FF')
        self.store.lifecycle('edit',dict(spool_uuid=self.roll['uuid'],color='#00FF00'))
        await self.coordinator.reconcile_metadata()
        self.assertEqual(len(self.sent),2)
        self.assertEqual(self.sent[1]['tray_color'],'00FF00FF')

    async def test_uncertain_metadata_does_not_resend_on_refresh_or_restart(self):
        self.store.assign('A1',self.roll['uuid'],0)
        async def ambiguous(payload):
            self.sent.append(payload)
            raise TimeoutError('No service response')
        self.coordinator.send=ambiguous
        await self.coordinator.reconcile_metadata()
        await self.coordinator.reconcile_metadata()
        replacement=AssignmentCoordinator(Store(self.store.path,seed_demo=False),self.config,
            ambiguous,lambda slot:self.states[slot],lambda:True,self.coordinator.executor)
        await replacement.reconcile_metadata()
        self.assertEqual(len(self.sent),1)
        self.assertEqual(replacement.metadata_status()['A1']['status'],'uncertain')

    async def test_metadata_refresh_never_changes_empty_busy_or_reserved_slot(self):
        self.store.assign('A1',self.roll['uuid'],0)
        self.states['A1']['empty']=True
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.sent,[])
        self.states['A1']['empty']=False
        self.store.start_job('Reserved job',[dict(slot='A1',revision=1,weight_mg=1000)],'reserved-metadata')
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.sent,[])

    async def test_metadata_rechecks_idle_after_final_database_validation(self):
        self.store.assign('A1',self.roll['uuid'],0)
        original=self.coordinator.executor
        snapshots=0
        async def run(fn,*args):
            nonlocal snapshots
            result=await original(fn,*args)
            if fn == self.store.snapshot:
                snapshots+=1
                if snapshots==2:self.ready=False
            return result
        self.coordinator.executor=run
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.sent,[])
        self.ready=True
        await self.coordinator.reconcile_metadata()
        self.assertEqual(len(self.sent),1)

    async def test_metadata_stale_binding_before_send_does_not_actuate(self):
        self.store.assign('A1',self.roll['uuid'],0)
        original=self.coordinator.executor
        async def run(fn,*args):
            result=await original(fn,*args)
            if fn.__name__=='save_metadata':self.store.assign('A1',None,1)
            return result
        self.coordinator.executor=run
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.sent,[])
        self.assertIsNone(self.store.snapshot()['slots'][0]['spool_uuid'])

    async def test_fresh_wrong_profile_never_confirms_metadata(self):
        self.store.assign('A1',self.roll['uuid'],0)
        async def wrong(payload):
            self.sent.append(payload)
            self.states['A1'].update(filament_id='BAD',type='PLA',color='#367AF5',
                nozzle_temp_min=190,nozzle_temp_max=240,observed_at='new')
        self.coordinator.send=wrong
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.coordinator.metadata_status()['A1']['status'],'uncertain')
        self.assertFalse(self.coordinator.metadata_status()['A1']['material_confirmed'])

    async def test_previously_confirmed_metadata_reports_later_material_drift_without_resending(self):
        self.store.assign('A1',self.roll['uuid'],0)
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.coordinator.metadata_status()['A1']['status'],'confirmed')
        self.states['A1'].update(filament_id='BAD',observed_at='later')
        await self.coordinator.reconcile_metadata()
        result=self.coordinator.metadata_status()['A1']
        self.assertEqual(result['status'],'uncertain')
        self.assertFalse(result['material_confirmed'])
        self.assertEqual(len(self.sent),1)

    async def test_uncertain_metadata_can_be_explicitly_reassigned_without_implicit_replay(self):
        self.store.assign('A1',self.roll['uuid'],0)
        send=self.coordinator.send
        async def ambiguous(payload):
            self.sent.append(payload)
            raise TimeoutError()
        self.coordinator.send=ambiguous
        await self.coordinator.reconcile_metadata()
        self.coordinator.send=send
        result=await self.coordinator.assign(dict(self.request(),revision=1))
        self.assertEqual(result['status'],'confirmed')
        await self.coordinator.reconcile_metadata()
        self.assertEqual(len(self.sent),2)

    async def test_adapter_definite_no_send_retries_once_when_ready(self):
        self.store.assign('A1',self.roll['uuid'],0)
        send=self.coordinator.send
        async def rejected(payload):
            raise CommandNotSent('Became busy before service')
        self.coordinator.send=rejected
        await self.coordinator.reconcile_metadata()
        self.assertEqual(self.coordinator.metadata_status()['A1']['status'],'deferred')
        self.coordinator.send=send
        await self.coordinator.reconcile_metadata()
        await self.coordinator.reconcile_metadata()
        self.assertEqual(len(self.sent),1)

    async def test_metadata_releases_lock_between_slots_for_waiting_inventory_write(self):
        self.store.assign('A1',self.roll['uuid'],0)
        self.store.assign('A2',self.store.snapshot()['spools'][1]['uuid'],0)
        entered,release=asyncio.Event(),asyncio.Event()
        order=[]
        async def send(payload):
            order.append(payload['entity_id'])
            if len(order)==1:
                entered.set()
                await release.wait()
        self.coordinator.send=send
        refresh=asyncio.create_task(self.coordinator.reconcile_metadata())
        await entered.wait()
        async def writer():
            async with self.coordinator.lock:order.append('inventory write')
        waiting=asyncio.create_task(writer())
        await asyncio.sleep(0)
        release.set()
        await asyncio.gather(refresh,waiting)
        self.assertEqual(order,['sensor.a1','inventory write','sensor.a2'])
