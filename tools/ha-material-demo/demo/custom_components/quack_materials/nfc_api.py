"""Authenticated HA adapter for phone enrollment and short-lived NFC scans."""
import asyncio
from uuid import UUID
from .nfc import NfcBroker, AccountNfcBroker

MODULE_URL = '/quack-materials/nfc.js?v=20261007.3'


async def async_setup_nfc(hass, inventory):
    from homeassistant.components.http import HomeAssistantView
    from homeassistant.components.frontend import add_extra_js_url
    from homeassistant.helpers.storage import Store
    from homeassistant.helpers import device_registry as dr

    storage = Store(hass, 1, 'quack_material_nfc_bindings')
    preferences = Store(hass, 1, 'quack_material_nfc_preferences')
    broker = AccountNfcBroker(bindings=await storage.async_load(), preferences=await preferences.async_load())
    lock = asyncio.Lock()
    hass.data['quack_material_demo_nfc'] = broker

    def devices_for(user_id):
        registry = dr.async_get(hass)
        devices = {}
        for entry in hass.config_entries.async_entries('mobile_app'):
            if entry.data.get('user_id') != user_id:
                continue
            native_id = entry.data.get('device_id')
            if not native_id:
                continue
            device = registry.async_get_device(identifiers={('mobile_app', native_id)})
            if device is not None:
                devices[device.id] = {
                    'id': device.id,
                    'name': device.name_by_user or device.name or entry.data.get('device_name') or 'Companion phone',
                }
        return devices

    class NfcView(HomeAssistantView):
        url = '/api/quack_materials/nfc/{action}'
        extra_urls = ['/api/quack_material_demo/nfc/{action}']
        name = 'api:quack_material_demo:nfc'
        requires_auth = True

        async def post(self, request, action):
            user = request.get('hass_user')
            if not user or not user.is_admin:
                return self.json({'error': 'Inventory administrator access required'}, status_code=403)
            if action not in ('devices', 'bind', 'unbind', 'poll', 'ack', 'route_poll',
                              'route_select', 'route_ack', 'route_status', 'preferences', 'labels', 'label_mark'):
                return self.json({'error': 'Unknown NFC operation'}, status_code=404)
            try:
                if request.content_length is None or not 0 < request.content_length <= 2048:
                    raise ValueError('Invalid NFC request')
                data = await request.json()
                if not isinstance(data, dict):
                    raise ValueError('Invalid NFC request')
                key = data.get('client_key')
                devices = devices_for(user.id)
                if action == 'label_mark':
                    roll = str(UUID(data.get('spool_uuid')))
                    snapshot = await hass.async_add_executor_job(inventory.management_snapshot)
                    if not any(row['uuid'] == roll for row in snapshot['spools']):
                        raise ValueError('Unknown roll')
                async with lock:
                    if action in ('labels', 'label_mark'):
                        if action == 'label_mark':
                            candidate = AccountNfcBroker(preferences=broker.dump_preferences())
                            candidate.mark_label(roll, data.get('kind'), data.get('confirmed'),
                                                 data.get('source', 'user_confirmation'))
                            await preferences.async_save(candidate.dump_preferences())
                            broker.labels = candidate.labels
                        return self.json({'labels': broker.dump_preferences()['labels']})
                    if action == 'preferences':
                        if data.get('confirmed') is not True:
                            raise ValueError('Confirm the default recipient')
                        device = data.get('device_id')
                        if device is not None and device not in devices:
                            raise ValueError('Choose your registered Companion device')
                        candidate = AccountNfcBroker(preferences=broker.dump_preferences())
                        candidate.set_default(user.id, device)
                        await preferences.async_save(candidate.dump_preferences())
                        broker.defaults = candidate.defaults
                        return self.json(broker.route_poll(user.id, key, list(devices.values())))
                    if action == 'route_select':
                        from copy import deepcopy
                        ident = data.get('request_id')
                        device = data.get('device_id')
                        # Save the preference first, leaving the request unchanged on disk failure.
                        row = broker._request(user.id, ident)
                        if not row['needs_choice']:
                            return self.json(broker.route_poll(user.id, key, list(devices.values())))
                        if row['expires_at'] <= broker.clock():
                            raise ValueError('Scan expired')
                        if device is not None and device not in devices:
                            raise ValueError('Choose your registered Companion device')
                        candidate = AccountNfcBroker(bindings=broker.dump_bindings(), preferences=broker.dump_preferences(), clock=broker.clock)
                        candidate.requests = deepcopy(broker.requests)
                        candidate.select_route(user.id, key, ident, device, list(devices.values()))
                        if data.get('remember') is True:
                            candidate.set_default(user.id, device)
                        await storage.async_save(candidate.dump_bindings())
                        if data.get('remember') is True:
                            await preferences.async_save(candidate.dump_preferences())
                        broker.bindings = candidate.bindings
                        broker.defaults = candidate.defaults
                        broker.requests[ident] = candidate.requests[ident]
                        return self.json(broker.route_poll(user.id, key, list(devices.values())))
                    if action == 'route_status':
                        return self.json(broker.route_status(user.id, data.get('request_id')))
                    if action == 'route_ack':
                        return self.json({'acknowledged': broker.route_ack(user.id, key, data.get('request_id'))})
                    if action == 'route_poll':
                        return self.json(broker.route_poll(user.id, key, list(devices.values()),
                                                           companion=data.get('companion') is True))
                    current = broker.poll(user.id, key)
                    if current['paired'] and current['device_id'] not in devices:
                        broker.unbind(user.id, key)
                        await storage.async_save(broker.dump_bindings())
                        current = {'paired': False, 'scan': None}
                    if action == 'devices':
                        return self.json({'devices': list(devices.values()),
                                          'device_id': current.get('device_id'),
                                          'default_defined': user.id in broker.defaults,
                                          'default_device': broker.defaults.get(user.id)})
                    if action == 'bind':
                        if data.get('confirmed') is not True:
                            raise ValueError('Confirm this phone selection')
                        device_id = data.get('device_id')
                        if not isinstance(device_id, str) or device_id not in devices:
                            return self.json({'error': 'Choose your registered Companion device'}, status_code=403)
                        candidate = NfcBroker(bindings=broker.dump_bindings())
                        candidate.bind(user.id, key, device_id)
                        await storage.async_save(candidate.dump_bindings())
                        broker.bind(user.id, key, device_id)
                        return self.json({'paired': True, 'device_id': device_id, 'scan': None})
                    if action == 'unbind':
                        candidate = NfcBroker(bindings=broker.dump_bindings())
                        candidate.unbind(user.id, key)
                        await storage.async_save(candidate.dump_bindings())
                        broker.unbind(user.id, key)
                        return self.json({'paired': False, 'scan': None})
                    if action == 'ack':
                        request_id = data.get('request_id')
                        if not isinstance(request_id, str) or len(request_id) > 64:
                            raise ValueError('Invalid scan acknowledgement')
                        return self.json({'acknowledged': broker.ack(user.id, key, request_id)})
                    if current['scan']:
                        current['scan']['expires_in'] = max(0, current['scan']['expires_at'] - broker.clock())
                    return self.json(current)
            except (ValueError, TypeError, KeyError):
                return self.json({'error': 'Invalid NFC request'}, status_code=400)
            except OSError:
                return self.json({'error': 'Phone link could not be saved; try again'}, status_code=503)

    async def on_scan(event):
        data = event.data
        device_id = data.get('device_id')
        owners = {entry.data.get('user_id') for entry in hass.config_entries.async_entries('mobile_app')
                  if device_id in devices_for(entry.data.get('user_id'))}
        owners.discard(None)
        if len(owners) != 1:
            return
        owner = next(iter(owners))
        # Some tag sources omit context.user_id. If present, it must match enrollment.
        context_user = getattr(event.context, 'user_id', None)
        if context_user and context_user != owner:
            return
        try:
            roll = str(UUID(data.get('tag_id')))
        except (ValueError, TypeError, AttributeError):
            return
        snapshot = await hass.async_add_executor_job(inventory.management_snapshot)
        if not any(row['uuid'] == roll for row in snapshot['spools']):
            return  # Leave unrelated HA tags and automations alone.
        async with lock:
            broker.scan(device_id, roll, event_id=event.context.id,
                        fired_at=event.time_fired.timestamp())
            broker.route_scan(owner, device_id, roll, list(devices_for(owner).values()),
                              event_id=event.context.id, fired_at=event.time_fired.timestamp())

    hass.http.register_view(NfcView())
    cancel = hass.bus.async_listen('tag_scanned', on_scan)
    hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel())
    add_extra_js_url(hass, MODULE_URL)
