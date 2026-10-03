"""Authenticated HA adapter for phone enrollment and short-lived NFC scans."""
import asyncio
from uuid import UUID
from .nfc import NfcBroker

MODULE_URL = '/quack-material-demo/nfc.js?v=0.6.0'


async def async_setup_nfc(hass, inventory):
    from homeassistant.components.http import HomeAssistantView
    from homeassistant.components.frontend import add_extra_js_url
    from homeassistant.helpers.storage import Store
    from homeassistant.helpers import device_registry as dr

    storage = Store(hass, 1, 'quack_material_nfc_bindings')
    broker = NfcBroker(bindings=await storage.async_load())
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
        url = '/api/quack_material_demo/nfc/{action}'
        name = 'api:quack_material_demo:nfc'
        requires_auth = True

        async def post(self, request, action):
            user = request.get('hass_user')
            if not user or not user.is_admin:
                return self.json({'error': 'Inventory administrator access required'}, status_code=403)
            if action not in ('devices', 'bind', 'unbind', 'poll', 'ack'):
                return self.json({'error': 'Unknown NFC operation'}, status_code=404)
            try:
                if request.content_length is None or not 0 < request.content_length <= 2048:
                    raise ValueError('Invalid NFC request')
                data = await request.json()
                if not isinstance(data, dict):
                    raise ValueError('Invalid NFC request')
                key = data.get('client_key')
                devices = devices_for(user.id)
                async with lock:
                    current = broker.poll(user.id, key)
                    if current['paired'] and current['device_id'] not in devices:
                        broker.unbind(user.id, key)
                        await storage.async_save(broker.dump_bindings())
                        current = {'paired': False, 'scan': None}
                    if action == 'devices':
                        return self.json({'devices': list(devices.values()),
                                          'device_id': current.get('device_id')})
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
        binding = next((row for row in broker.bindings.values() if row['device_id'] == device_id), None)
        if not binding or device_id not in devices_for(binding['user_id']):
            return
        # Some tag sources omit context.user_id. If present, it must match enrollment.
        context_user = getattr(event.context, 'user_id', None)
        if context_user and context_user != binding['user_id']:
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

    hass.http.register_view(NfcView())
    cancel = hass.bus.async_listen('tag_scanned', on_scan)
    hass.bus.async_listen_once('homeassistant_stop', lambda event: cancel())
    add_extra_js_url(hass, MODULE_URL)
