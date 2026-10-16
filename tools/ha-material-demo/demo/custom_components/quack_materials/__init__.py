"""HA inventory with opt-in coordinated printer material metadata."""


async def async_setup(hass, config):
    from datetime import datetime,timezone
    boot_at=datetime.now(timezone.utc)
    from pathlib import Path
    from homeassistant.components.http import HomeAssistantView, StaticPathConfig
    from homeassistant.helpers.event import async_track_time_interval, async_track_state_change_event
    import asyncio
    import json
    import logging
    import re
    from functools import partial
    from datetime import timedelta
    from .store import Store, Conflict
    from .access import may_read, may_write
    from .read_api import materials, native_page, profile, sync, profile_content
    from .recovery import export_recovery
    from .accounting import accounting
    from .receipts import compact_legacy_receipts
    if 'quack_materials' in config and 'quack_material_demo' in config:
        raise ValueError('Configure only one material inventory integration')
    settings = config.get('quack_materials', config.get('quack_material_demo')) or {}
    if not isinstance(settings, dict):
        raise ValueError('Material inventory configuration must be a mapping')
    settings=dict(settings)
    job_preheat_enabled = settings.get('job_preheat_enabled', False)
    if type(job_preheat_enabled) is not bool:
        raise ValueError('job_preheat_enabled must be boolean')
    manual_preheat_enabled = settings.get('manual_preheat_enabled', False)
    if type(manual_preheat_enabled) is not bool:
        raise ValueError('manual_preheat_enabled must be boolean')
    physical=settings.get('provider_printer_id')
    if physical is not None and (not isinstance(physical,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}',physical)):
        raise ValueError('Invalid provider physical printer identity')
    mode=settings.get('mode','release' if 'quack_materials' in config else 'demo')
    if mode not in ('demo','pilot','release'): raise ValueError('Mode must be demo, pilot or release')
    settings['mode']=mode
    nfc_enabled = settings.get('nfc_enabled', mode != 'demo')
    if not isinstance(nfc_enabled, bool): raise ValueError('nfc_enabled must be boolean')
    database=settings.get('database','quack_material_inventory.sqlite3' if mode!='demo' else 'quack_material_demo.sqlite3')
    if database not in ('quack_material_inventory.sqlite3','quack_material_demo.sqlite3'): raise ValueError('Use a dedicated inventory database filename')
    allowed=settings.get('allowed_sync_users',[])
    if not isinstance(allowed,list) or any(not isinstance(v,str) or not re.fullmatch(r'[a-zA-Z0-9_-]{1,64}',v) for v in allowed): raise ValueError('Invalid synchronization user IDs')
    catalog=Path(__file__).with_name('profile_catalog.json')
    if catalog.is_file():
        settings['profile_catalog']=await hass.async_add_executor_job(lambda: json.loads(catalog.read_text(encoding='utf-8')))
    store = await hass.async_add_executor_job(partial(Store,hass.config.path('.storage',database),seed_demo=mode=='demo',settings=settings))
    if mode in ('pilot','release'):
        settings.setdefault('restart_resume_window_seconds', 180)
        settings.setdefault('restart_resume_unique_daily', True)
        settings.setdefault('restart_timezone', getattr(hass.config, 'time_zone', 'UTC'))
        from .restart_recovery import RestartRecovery
        store.restart_recovery=await hass.async_add_executor_job(partial(RestartRecovery,store,boot_at=boot_at))
    await hass.async_add_executor_job(compact_legacy_receipts, store)
    hass.data['quack_materials'] = store
    hass.data['quack_material_demo'] = store  # One store; legacy in-process compatibility.
    observer_lock = asyncio.Lock()
    assignment = None
    preheat_runtime = None
    metadata_task = None
    metadata_requested = False
    metadata_stopped = False

    async def refresh_metadata():
        nonlocal metadata_requested
        try:
            while metadata_requested and not metadata_stopped:
                metadata_requested = False
                if await assignment.reconcile_presence():
                    await publish()
                await assignment.reconcile_metadata()
        except asyncio.CancelledError:
            raise
        except Exception:
            # The durable receipt owns any uncertain send. A later poll can reconcile it.
            logging.getLogger(__name__).warning('Printer metadata refresh failed; pending receipts are preserved')

    def request_metadata_refresh():
        nonlocal metadata_task, metadata_requested
        if assignment is None or metadata_stopped:
            return
        metadata_requested = True
        if metadata_task is None or metadata_task.done():
            metadata_task = asyncio.create_task(refresh_metadata(), name='quack-printer-metadata')

    async def stop_metadata(_):
        nonlocal metadata_stopped
        metadata_stopped = True
        if metadata_task is not None and not metadata_task.done():
            metadata_task.cancel()
            try:
                await metadata_task
            except asyncio.CancelledError:
                pass

    hass.bus.async_listen_once('homeassistant_stop', stop_metadata)
    status_entity = settings.get('printer_state_entity')
    name_entity = settings.get('printer_name_entity')
    prefix = status_entity[:-len('print_status')] if isinstance(status_entity,str) and status_entity.endswith('_print_status') else None
    telemetry_entities = {key:settings.get(setting, prefix+suffix if prefix else None) for key,setting,suffix in (
        ('progress_percent','printer_progress_entity','print_progress'),
        ('current_layer','printer_current_layer_entity','current_layer'),
        ('total_layers','printer_total_layers_entity','total_layer_count'),
        ('printer_job_id','printer_job_identity_entity','task_id'))}
    if any(value is not None and (not isinstance(value,str) or not value.startswith('sensor.')) for value in telemetry_entities.values()):
        raise ValueError('Print progress requires sensor entity IDs')
    if any(value is not None and (not isinstance(value, str) or not value.startswith('sensor.'))
           for value in (status_entity, name_entity)):
        raise ValueError('The optional read-only observer requires sensor entity IDs')

    class DemoView(HomeAssistantView):
        url = '/api/quack_materials/materials'
        extra_urls = ['/api/quack_material_demo/materials']
        name = 'api:quack_material_demo:materials'
        requires_auth = True

        async def get(self, request):
            if not may_read(request.get('hass_user'),allowed):
                return self.json({'error':'Inventory access denied'},status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(materials, store, dict(getattr(request, 'query', {}))))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid materials query'}, status_code=400)

    class NativeSnapshotView(HomeAssistantView):
        url = '/api/quack_materials/native_snapshot'
        extra_urls = ['/api/quack_material_demo/native_snapshot']
        name = 'api:quack_material_demo:native_snapshot'
        requires_auth = True
        reader = staticmethod(native_page)

        async def get(self, request):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(self.reader, store, dict(getattr(request, 'query', {}))))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid native snapshot query'}, status_code=400)

    class IncrementalSyncView(NativeSnapshotView):
        url = '/api/quack_materials/sync'
        extra_urls = ['/api/quack_material_demo/sync']
        name = 'api:quack_material_demo:sync'
        reader = staticmethod(sync)

    class ActionView(HomeAssistantView):
        url = '/api/quack_materials/action/{action}'
        extra_urls = ['/api/quack_material_demo/action/{action}']
        name = 'api:quack_material_demo:action'
        requires_auth = True

        async def post(self, request, action):
            user=request.get('hass_user')
            if not (may_read(user,allowed) if action=='label' else may_write(user,allowed,action)):
                return self.json({'error': 'Inventory write access denied'}, status_code=403)
            try:
                if mode!='demo' and action in ('start','finish'):
                    raise ValueError('Simulation actions are disabled in the live pilot')
                length=getattr(request,'content_length',None)
                limit=2000000 if action in ('provider_apply','native_apply') else 1000000
                if length is not None and not 0<length<=limit: raise ValueError('Request exceeds limit')
                data = await request.json()
                if len(json.dumps(data).encode())>limit: raise ValueError('Request exceeds limit')
                if assignment and action != 'label':
                    async with assignment.lock:
                        bookkeeping=action in ('order_paid','order_cost_items','restart_confirmation','discard_provider') or (action=='provider_job' and isinstance(data,dict) and data.get('command') in ('dispatch_result','bind_external_id'))
                        replay=await hass.async_add_executor_job(store.provider_request_recorded,action,data)
                        if await hass.async_add_executor_job(assignment.pending) and action not in ('reconcile_observed','reconcile_provider','set_print_status') and not bookkeeping and not replay:
                            raise Conflict('Resolve the pending printer assignment first')
                        if action == 'assign' and data.get('spool_uuid') is not None:
                            raise Conflict('Use the coordinated printer assignment dialog')
                        result = (await preheat_runtime.dispatch(action, data) if preheat_runtime is not None
                                  else await hass.async_add_executor_job(store.dispatch, action, data))
                else:
                    result = (await preheat_runtime.dispatch(action, data) if preheat_runtime is not None
                              else await hass.async_add_executor_job(store.dispatch, action, data))
                await publish()
                return self.json(result)
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid inventory request'}, status_code=400)

    class InventoryView(HomeAssistantView):
        url = '/api/quack_materials/inventory'
        extra_urls = ['/api/quack_material_demo/inventory']
        name = 'api:quack_material_demo:inventory'
        requires_auth = True

        async def get(self,request):
            user=request.get('hass_user')
            if not may_read(user,allowed): return self.json({'error':'Inventory access denied'},status_code=403)
            data=await hass.async_add_executor_job(store.management_snapshot)
            data['can_edit']=bool(user and user.is_admin)
            if preheat_runtime is not None and job_preheat_enabled:
                data['job_preheats'] = await hass.async_add_executor_job(preheat_runtime.core.summaries)
            if assignment and data['can_edit']:
                from .slot_api import assignment_status
                data['printer_assignment'] = await assignment_status(hass, assignment)
            return self.json(data)

    def preheat_path(request, job_uuid):
        if getattr(request, 'query', None):
            raise ValueError('Preheat endpoints do not accept query parameters')
        if not isinstance(job_uuid, str) or not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', job_uuid):
            raise ValueError('Invalid print attempt identity')
        if not job_preheat_enabled or preheat_runtime is None or not preheat_runtime.enabled:
            raise Conflict('Job preheat is disabled')

    class PreheatView(HomeAssistantView):
        url = '/api/quack_materials/preheat/{job_uuid}'
        extra_urls = ['/api/quack_material_demo/preheat/{job_uuid}']
        name = 'api:quack_materials:preheat'
        requires_auth = True

        async def get(self, request, job_uuid):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                preheat_path(request, job_uuid)
                return self.json(await preheat_runtime.status(job_uuid))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid preheat query'}, status_code=400)

    class PreheatCommandView(HomeAssistantView):
        url = '/api/quack_materials/preheat/{job_uuid}/{command}'
        extra_urls = ['/api/quack_material_demo/preheat/{job_uuid}/{command}']
        name = 'api:quack_materials:preheat_command'
        requires_auth = True

        async def post(self, request, job_uuid, command):
            user = request.get('hass_user')
            if not may_read(user, allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            # Quack's configured sync identity may skip its own attempt; the
            # coordinator still checks its instance and version. Extensions are admin-only.
            if command == 'extend' and not getattr(user, 'is_admin', False):
                return self.json({'error': 'Administrator access required'}, status_code=403)
            try:
                preheat_path(request, job_uuid)
                if command not in ('request', 'cancel', 'skip', 'retry', 'extend'):
                    raise ValueError('Unknown preheat command')
                length = getattr(request, 'content_length', None)
                if length is not None and (type(length) is not int or not 0 < length <= 4096):
                    raise ValueError('Request exceeds limit')
                data = await request.json()
                expected = ({'request_key', 'instance_id', 'printer_id', 'parameters'} if command == 'request'
                            else {'request_key', 'instance_id', 'version'} | ({'seconds'} if command == 'extend' else set()))
                if not isinstance(data, dict) or set(data) != expected or len(json.dumps(data, allow_nan=False).encode()) > 4096:
                    raise ValueError('Invalid preheat command body')
                return self.json(await preheat_runtime.command(job_uuid, command, data))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid preheat command'}, status_code=400)

    def manual_preheat_path(request):
        if getattr(request, 'query', None):
            raise ValueError('Manual preheat does not accept query parameters')
        if not manual_preheat_enabled or preheat_runtime is None or not preheat_runtime.enabled:
            raise Conflict('Manual preheat is disabled')

    class ManualPreheatView(HomeAssistantView):
        url = '/api/quack_materials/manual_preheat'
        extra_urls = ['/api/quack_material_demo/manual_preheat']
        name = 'api:quack_materials:manual_preheat'
        requires_auth = True

        async def get(self, request):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                manual_preheat_path(request)
                return self.json(await preheat_runtime.manual_status())
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid manual preheat query'}, status_code=400)

    class ManualPreheatCommandView(HomeAssistantView):
        url = '/api/quack_materials/manual_preheat/{command}'
        extra_urls = ['/api/quack_material_demo/manual_preheat/{command}']
        name = 'api:quack_materials:manual_preheat_command'
        requires_auth = True

        async def post(self, request, command):
            if not getattr(request.get('hass_user'), 'is_admin', False):
                return self.json({'error': 'Administrator access required'}, status_code=403)
            try:
                manual_preheat_path(request)
                if command not in ('start', 'cancel'):
                    raise ValueError('Unknown manual preheat command')
                length = getattr(request, 'content_length', None)
                if length is not None and (type(length) is not int or not 0 < length <= 4096):
                    raise ValueError('Request exceeds limit')
                data = await request.json()
                expected = ({'job_uuid', 'request_key', 'instance_id', 'printer_id', 'parameters'} if command == 'start'
                            else {'job_uuid', 'request_key', 'instance_id', 'version'})
                if not isinstance(data, dict) or set(data) != expected or len(json.dumps(data, allow_nan=False).encode()) > 4096:
                    raise ValueError('Invalid manual preheat body')
                if not isinstance(data['job_uuid'], str) or not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', data['job_uuid']):
                    raise ValueError('Invalid manual preheat identity')
                return self.json(await preheat_runtime.manual_command(command, data))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid manual preheat command'}, status_code=400)

    class ProfileView(HomeAssistantView):
        url = '/api/quack_materials/profile'
        extra_urls = ['/api/quack_material_demo/profile']
        name = 'api:quack_material_demo:profile'
        requires_auth = True
        reader = staticmethod(profile)

        async def get(self, request):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(self.reader, store, dict(getattr(request, 'query', {}))))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid material profile query'}, status_code=400)

    class ProfileContentView(ProfileView):
        url = '/api/quack_materials/profile_content'
        extra_urls = ['/api/quack_material_demo/profile_content']
        name = 'api:quack_material_demo:profile_content'
        reader = staticmethod(profile_content)

    class AccountingView(HomeAssistantView):
        url = '/api/quack_materials/accounting'
        extra_urls = ['/api/quack_material_demo/accounting']
        name = 'api:quack_material_demo:accounting'
        requires_auth = True

        async def get(self, request):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(accounting, store, dict(getattr(request, 'query', {}))))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid accounting query'}, status_code=400)

    class RecoveryView(HomeAssistantView):
        url = '/api/quack_materials/recovery'
        extra_urls = ['/api/quack_material_demo/recovery']
        name = 'api:quack_material_demo:recovery'
        requires_auth = True

        async def get(self, request):
            user = request.get('hass_user')
            if not user or not getattr(user, 'is_admin', False):
                return self.json({'error': 'Administrator access required'}, status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(export_recovery, store))
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Inventory recovery export unavailable'}, status_code=400)

    recovery=getattr(store,'restart_recovery',None)
    known_notices=await hass.async_add_executor_job(recovery.known_notification_ids) if recovery else []
    notified_reviews={ident:None for ident in known_notices}
    async def publish_restart_notifications(reviews):
        if not reviews and not notified_reviews:return
        import inspect
        from homeassistant.components import persistent_notification
        current={}
        reasons={'restart_waiting_fresh':'Warte auf frische Druckerdaten.',
            'awaiting_fresh_state':'Der Drucker meldet noch keine frischen Daten.',
            'name_mismatch':'Der Drucker meldet einen anderen Drucknamen.',
            'printer_job_changed':'Der Drucker meldet eine andere Druckkennung.',
            'progress_regressed':'Der Fortschritt passt nicht zum gespeicherten Druck.',
            'layer_count_changed':'Die Schichtanzahl hat sich geÃƒÂ¤ndert.',
            'identity_unproven':'Der laufende Druck muss dem gespeicherten Druck zugeordnet werden.',
            'terminal_during_restart':'Der Druck wurde wÃƒÂ¤hrend der HA-Unterbrechung beendet.',
            'idle_without_outcome':'Der Drucker ist bereit; das Ergebnis des gespeicherten Drucks ist unklar.',
            'consumption_requires_review':'Ergebnis oder Filamentverbrauch des ursprÃƒÂ¼nglichen Drucks prÃƒÂ¼fen.'}
        for review in reviews:
            ident=review['notification_id']
            message=f"**{review['name']}**: {reasons.get(review['reason'],'Druck nach HA-Neustart prÃƒÂ¼fen.')}\n\n[Zur DruckprÃƒÂ¼fung](/dashboard-filament/prints?restart_review={review['job_uuid']})\n\nDer Bestand bleibt bis zur eindeutigen Zuordnung oder bewussten BestÃƒÂ¤tigung erhalten. Das SchlieÃƒÅ¸en dieses Hinweises bestÃƒÂ¤tigt keinen Verbrauch."
            current[ident]=message
            if notified_reviews.get(ident)!=message:
                result=persistent_notification.async_create(hass,message,title='Material & AuftrÃƒÂ¤ge Ã‚Â· NeustartprÃƒÂ¼fung',notification_id=ident)
                if inspect.isawaitable(result):await result
        for ident in notified_reviews.keys()-current.keys():
            result=persistent_notification.async_dismiss(hass,ident)
            if inspect.isawaitable(result):await result
        notified_reviews.clear();notified_reviews.update(current)

    async def publish(*_):
        data = await hass.async_add_executor_job(store.snapshot)
        recovery=getattr(store,'restart_recovery',None)
        data['restart_reviews']=await hass.async_add_executor_job(recovery.reviews) if recovery else []
        if mode!='demo':
            hass.states.async_set('sensor.quack_material_inventory',len(data['spools']),{
                'friendly_name':'Quack material inventory','mode':mode,'revision':data['revision'],
                'print_attempt_count':len(data['jobs']),'pending_review_count':sum(j['state']=='needs_review' for j in data['jobs']),
                'pending_restart_review_count':len(data['restart_reviews']),
                'assigned_slot_count':sum(bool(s['spool_uuid']) for s in data['slots']),
                'printer_commands_enabled':assignment is not None})
        else:
            hass.states.async_set('sensor.quack_material_demo', len(data['spools']), {
                'friendly_name': 'Quack material demo', 'demo_mode': True,
                'slots': data['slots'], 'spools': data['spools'], 'jobs': data['jobs'],
            })
        # Startup, committed mutations and the existing periodic/observer refresh all
        # converge here. Network metadata writes never delay an inventory acknowledgement.
        request_metadata_refresh()
        await publish_restart_notifications(data['restart_reviews'])

    async def observe(*_):
        if not status_entity:
            return
        async with observer_lock:
            status = hass.states.get(status_entity)
            name = hass.states.get(name_entity) if name_entity else None
            if status is not None:
                telemetry={}
                for key,entity in telemetry_entities.items():
                    reading=hass.states.get(entity) if entity else None
                    if reading is not None and not getattr(reading,'attributes',{}).get('restored'):
                        telemetry[key]=reading.state
                        when=getattr(reading,'last_reported',None) or getattr(reading,'last_updated',None)
                        if when is not None and key in ('progress_percent','current_layer'):
                            telemetry['progress_observed_at' if key=='progress_percent' else 'layer_observed_at']=when.isoformat()
                        if when is not None and key=='printer_job_id':telemetry['identity_observed_at']=when.isoformat()
                await hass.async_add_executor_job(partial(store.observe_printer,status.state,
                    name.state if name and name.state not in ('unknown','unavailable') else 'Observed P2S print','duck-poop',telemetry,
                    status_reported_at=getattr(status,'last_reported',None) or getattr(status,'last_updated',None),
                    name_reported_at=(getattr(name,'last_reported',None) or getattr(name,'last_updated',None)) if name else None,
                    status_restored=bool(getattr(status,'attributes',{}).get('restored')),
                    name_restored=bool(getattr(name,'attributes',{}).get('restored')) if name else False))
            await publish()

    hass.http.register_view(DemoView())
    hass.http.register_view(ActionView())
    hass.http.register_view(InventoryView())
    hass.http.register_view(NativeSnapshotView())
    hass.http.register_view(ProfileView())
    hass.http.register_view(RecoveryView())
    hass.http.register_view(AccountingView())
    hass.http.register_view(IncrementalSyncView())
    hass.http.register_view(ProfileContentView())
    hass.http.register_view(PreheatView())
    hass.http.register_view(PreheatCommandView())
    hass.http.register_view(ManualPreheatView())
    hass.http.register_view(ManualPreheatCommandView())
    await hass.http.async_register_static_paths([
        StaticPathConfig('/quack-materials/card.js', str(Path(__file__).with_name('card.js')), False),
        StaticPathConfig('/quack-materials/inventory.js', str(Path(__file__).with_name('inventory.js')), False),
        StaticPathConfig('/quack-materials/color_picker.js', str(Path(__file__).with_name('color_picker.js')), False),
        StaticPathConfig('/quack-materials/nfc.js', str(Path(__file__).with_name('nfc.js')), False),
        StaticPathConfig('/quack-materials/slot_picker.js', str(Path(__file__).with_name('slot_picker.js')), False),
        StaticPathConfig('/quack-materials/manual_preheat.js', str(Path(__file__).with_name('manual_preheat.js')), False),
        StaticPathConfig('/quack-material-demo/card.js', str(Path(__file__).with_name('card.js')), False),
        StaticPathConfig('/quack-material-demo/inventory.js', str(Path(__file__).with_name('inventory.js')), False),
        StaticPathConfig('/quack-material-demo/color_picker.js', str(Path(__file__).with_name('color_picker.js')), False),
        StaticPathConfig('/quack-material-demo/nfc.js', str(Path(__file__).with_name('nfc.js')), False),
        StaticPathConfig('/quack-material-demo/slot_picker.js', str(Path(__file__).with_name('slot_picker.js')), False),
        StaticPathConfig('/quack-material-demo/manual_preheat.js', str(Path(__file__).with_name('manual_preheat.js')), False),
    ])
    from .slot_api import async_setup_assignment
    assignment = await async_setup_assignment(hass, store, settings, publish)
    if assignment:
        from homeassistant.core import callback
        from .slot_api import reported_slot_state
        entity_slots = {entity:slot for slot,entity in assignment.config['slots'].items()}
        @callback
        def slot_changed(event):
            slot = entity_slots.get(event.data.get('entity_id'))
            if slot:
                assignment.note_presence(slot, reported_slot_state(event.data.get('new_state')))
            request_metadata_refresh()
        cancel_slots = async_track_state_change_event(hass, list(assignment.config['slots'].values()), slot_changed)
        hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel_slots())
    if job_preheat_enabled or manual_preheat_enabled:
        from .preheat_runtime import PreheatRuntime
        preheat_runtime = PreheatRuntime(hass, store, settings, publish)
        await preheat_runtime.start()
        hass.bus.async_listen_once('homeassistant_stop', preheat_runtime.stop)
    if manual_preheat_enabled:
        from homeassistant.components.frontend import add_extra_js_url
        add_extra_js_url(hass, '/quack-materials/manual_preheat.js?v=20261015.3')
    if nfc_enabled:
        from .nfc_api import async_setup_nfc
        await async_setup_nfc(hass, store)
    await publish()
    if status_entity:
        cancel_observer = async_track_state_change_event(hass, list(dict.fromkeys(entity for entity in (status_entity,name_entity,*telemetry_entities.values()) if entity)), observe)
        hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel_observer())
        await observe()
    async def periodic_refresh(*_):
        if status_entity:await observe()
        else:await publish()
    cancel = async_track_time_interval(hass, periodic_refresh, timedelta(seconds=15))
    hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel())
    return True
