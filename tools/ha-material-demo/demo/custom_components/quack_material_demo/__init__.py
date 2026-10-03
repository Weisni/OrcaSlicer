"""HA inventory with opt-in coordinated printer material metadata."""


async def async_setup(hass, config):
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
    from .read_api import materials, native_page, profile
    from .recovery import export_recovery
    from .accounting import accounting
    from .receipts import compact_legacy_receipts
    settings = config.get('quack_material_demo') or {}
    if not isinstance(settings, dict):
        raise ValueError('quack_material_demo configuration must be a mapping')
    settings=dict(settings)
    physical=settings.get('provider_printer_id')
    if physical is not None and (not isinstance(physical,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}',physical)):
        raise ValueError('Invalid provider physical printer identity')
    mode=settings.get('mode','demo')
    if mode not in ('demo','pilot'): raise ValueError('Mode must be demo or pilot')
    nfc_enabled = settings.get('nfc_enabled', mode == 'pilot')
    if not isinstance(nfc_enabled, bool): raise ValueError('nfc_enabled must be boolean')
    database=settings.get('database','quack_material_inventory.sqlite3' if mode=='pilot' else 'quack_material_demo.sqlite3')
    if database not in ('quack_material_inventory.sqlite3','quack_material_demo.sqlite3'): raise ValueError('Use a dedicated inventory database filename')
    allowed=settings.get('allowed_sync_users',[])
    if not isinstance(allowed,list) or any(not isinstance(v,str) or not re.fullmatch(r'[a-zA-Z0-9_-]{1,64}',v) for v in allowed): raise ValueError('Invalid synchronization user IDs')
    catalog=Path(__file__).with_name('profile_catalog.json')
    if catalog.is_file():
        settings['profile_catalog']=await hass.async_add_executor_job(lambda: json.loads(catalog.read_text(encoding='utf-8')))
    store = await hass.async_add_executor_job(partial(Store,hass.config.path('.storage',database),seed_demo=mode=='demo',settings=settings))
    await hass.async_add_executor_job(compact_legacy_receipts, store)
    hass.data['quack_material_demo'] = store
    observer_lock = asyncio.Lock()
    assignment = None
    metadata_task = None
    metadata_requested = False
    metadata_stopped = False

    async def refresh_metadata():
        nonlocal metadata_requested
        try:
            while metadata_requested and not metadata_stopped:
                metadata_requested = False
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
    if any(value is not None and (not isinstance(value, str) or not value.startswith('sensor.'))
           for value in (status_entity, name_entity)):
        raise ValueError('The optional read-only observer requires sensor entity IDs')

    class DemoView(HomeAssistantView):
        url = '/api/quack_material_demo/materials'
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
        url = '/api/quack_material_demo/native_snapshot'
        name = 'api:quack_material_demo:native_snapshot'
        requires_auth = True

        async def get(self, request):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(native_page, store, dict(getattr(request, 'query', {}))))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid native snapshot query'}, status_code=400)

    class ActionView(HomeAssistantView):
        url = '/api/quack_material_demo/action/{action}'
        name = 'api:quack_material_demo:action'
        requires_auth = True

        async def post(self, request, action):
            user=request.get('hass_user')
            if not (may_read(user,allowed) if action=='label' else may_write(user,allowed,action)):
                return self.json({'error': 'Inventory write access denied'}, status_code=403)
            try:
                if mode=='pilot' and action in ('start','finish'):
                    raise ValueError('Simulation actions are disabled in the live pilot')
                length=getattr(request,'content_length',None)
                limit=2000000 if action in ('provider_apply','native_apply') else 1000000
                if length is not None and not 0<length<=limit: raise ValueError('Request exceeds limit')
                data = await request.json()
                if len(json.dumps(data).encode())>limit: raise ValueError('Request exceeds limit')
                if assignment and action != 'label':
                    async with assignment.lock:
                        bookkeeping=action=='provider_job' and isinstance(data,dict) and data.get('command') in ('dispatch_result','bind_external_id')
                        replay=await hass.async_add_executor_job(store.provider_request_recorded,action,data)
                        if await hass.async_add_executor_job(assignment.pending) and action not in ('reconcile_observed','reconcile_provider') and not bookkeeping and not replay:
                            raise Conflict('Resolve the pending printer assignment first')
                        if action == 'assign' and data.get('spool_uuid') is not None:
                            raise Conflict('Use the coordinated printer assignment dialog')
                        result = await hass.async_add_executor_job(store.dispatch, action, data)
                else:
                    result = await hass.async_add_executor_job(store.dispatch, action, data)
                await publish()
                return self.json(result)
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid demo request'}, status_code=400)

    class InventoryView(HomeAssistantView):
        url = '/api/quack_material_demo/inventory'
        name = 'api:quack_material_demo:inventory'
        requires_auth = True

        async def get(self,request):
            user=request.get('hass_user')
            if not may_read(user,allowed): return self.json({'error':'Inventory access denied'},status_code=403)
            data=await hass.async_add_executor_job(store.management_snapshot)
            data['can_edit']=bool(user and user.is_admin)
            if assignment and data['can_edit']:
                from .slot_api import assignment_status
                data['printer_assignment'] = await assignment_status(hass, assignment)
            return self.json(data)

    class ProfileView(HomeAssistantView):
        url = '/api/quack_material_demo/profile'
        name = 'api:quack_material_demo:profile'
        requires_auth = True

        async def get(self, request):
            if not may_read(request.get('hass_user'), allowed):
                return self.json({'error': 'Inventory access denied'}, status_code=403)
            try:
                return self.json(await hass.async_add_executor_job(profile, store, dict(getattr(request, 'query', {}))))
            except Conflict as error:
                return self.json({'error': str(error)}, status_code=409)
            except (ValueError, KeyError, TypeError):
                return self.json({'error': 'Invalid material profile query'}, status_code=400)

    class AccountingView(HomeAssistantView):
        url = '/api/quack_material_demo/accounting'
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
        url = '/api/quack_material_demo/recovery'
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

    async def publish(*_):
        data = await hass.async_add_executor_job(store.snapshot)
        if mode=='pilot':
            hass.states.async_set('sensor.quack_material_inventory',len(data['spools']),{
                'friendly_name':'Quack material inventory','mode':'pilot','revision':data['revision'],
                'print_attempt_count':len(data['jobs']),'pending_review_count':sum(j['state']=='needs_review' for j in data['jobs']),
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

    async def observe(*_):
        if not status_entity:
            return
        async with observer_lock:
            status = hass.states.get(status_entity)
            name = hass.states.get(name_entity) if name_entity else None
            if status is not None:
                await hass.async_add_executor_job(store.observe_printer, status.state,
                    name.state if name and name.state not in ('unknown', 'unavailable') else 'Observed P2S print')
            await publish()

    hass.http.register_view(DemoView())
    hass.http.register_view(ActionView())
    hass.http.register_view(InventoryView())
    hass.http.register_view(NativeSnapshotView())
    hass.http.register_view(ProfileView())
    hass.http.register_view(RecoveryView())
    hass.http.register_view(AccountingView())
    await hass.http.async_register_static_paths([
        StaticPathConfig('/quack-material-demo/card.js', str(Path(__file__).with_name('card.js')), False),
        StaticPathConfig('/quack-material-demo/inventory.js', str(Path(__file__).with_name('inventory.js')), False),
        StaticPathConfig('/quack-material-demo/nfc.js', str(Path(__file__).with_name('nfc.js')), False),
        StaticPathConfig('/quack-material-demo/slot_picker.js', str(Path(__file__).with_name('slot_picker.js')), False),
    ])
    from .slot_api import async_setup_assignment
    assignment = await async_setup_assignment(hass, store, settings, publish)
    if nfc_enabled:
        from .nfc_api import async_setup_nfc
        await async_setup_nfc(hass, store)
    await publish()
    if status_entity:
        cancel_observer = async_track_state_change_event(hass, list(dict.fromkeys(entity for entity in (status_entity,name_entity) if entity)), observe)
        hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel_observer())
        await observe()
    cancel = async_track_time_interval(hass, publish, timedelta(seconds=15))
    hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel())
    return True
