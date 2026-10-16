"""Admin-only HA adapter for coordinated material assignment."""
from .slot_assignment import AssignmentCoordinator, CommandNotSent, PROFILES
from .store import Conflict


def printer_ready(hass, entity, stage_entity):
    state = hass.states.get(entity)
    stage = hass.states.get(stage_entity)
    return bool(state and state.state in ('idle', 'finish', 'failed')
                and stage and stage.state == 'idle'
                and hass.services.has_service('bambu_lab', 'set_filament'))


def slot_state(hass, entity):
    return reported_slot_state(hass.states.get(entity))


def reported_slot_state(state):
    if not state:
        return None
    observed = getattr(state, 'last_reported', None) or getattr(state, 'last_updated', None)
    return dict(state.attributes, available=state.state not in ('unknown', 'unavailable'),
                observed_at=observed.isoformat() if observed else None)


async def async_setup_assignment(hass, store, settings, publish):
    config = settings.get('printer_assignment')
    if config is None:
        return None
    if settings.get('mode') not in ('pilot','release') or not isinstance(config, dict):
        raise ValueError('Printer assignment requires explicit pilot configuration')
    if not isinstance(config.get('stage_entity'), str) or not config['stage_entity'].startswith('sensor.'):
        raise ValueError('Configure the printer current-stage sensor')
    from homeassistant.components.http import HomeAssistantView
    from homeassistant.components.frontend import add_extra_js_url
    from homeassistant.helpers import entity_registry as er

    async def send(payload):
        # Restrict configured targets to the installed Bambu integration.
        entity = er.async_get(hass).async_get(payload['entity_id'])
        if not entity or entity.platform != 'bambu_lab':
            raise CommandNotSent('Configured slot is not a Bambu entity')
        if not printer_ready(hass, settings.get('printer_state_entity'), config['stage_entity']):
            raise CommandNotSent('Printer became busy or unavailable; no metadata command was sent')
        state = slot_state(hass, payload['entity_id'])
        if not state or not state['available'] or (payload['entity_id'] != config['slots'].get('EXT') and state.get('empty') is not False):
            raise CommandNotSent('Printer slot became unavailable or empty; no metadata command was sent')
        await hass.services.async_call('bambu_lab', 'set_filament', payload, blocking=True)

    coordinator = AssignmentCoordinator(store, config, send,
        lambda slot:slot_state(hass, config['slots'][slot]),
        lambda:printer_ready(hass, settings.get('printer_state_entity'), config['stage_entity']),
        hass.async_add_executor_job)
    await hass.async_add_executor_job(coordinator.initialize)

    class AssignmentView(HomeAssistantView):
        url = '/api/quack_materials/slot_assignment'
        extra_urls = ['/api/quack_material_demo/slot_assignment']
        name = 'api:quack_material_demo:slot_assignment'
        requires_auth = True

        async def get(self, request):
            user = request.get('hass_user')
            if not user or not user.is_admin:
                return self.json({'error':'Inventory administrator access required'}, status_code=403)
            return self.json(await assignment_status(hass, coordinator))

        async def post(self, request):
            user = request.get('hass_user')
            if not user or not user.is_admin:
                return self.json({'error':'Inventory administrator access required'}, status_code=403)
            try:
                if request.content_length is None or not 0 < request.content_length <= 2048:
                    raise ValueError('Invalid request length')
                data = await request.json()
                if not isinstance(data, dict):
                    raise ValueError('Invalid assignment request')
                scan_id = data.get('nfc_request')
                broker = getattr(hass, 'data', {}).get('quack_material_demo_nfc')
                if scan_id is not None:
                    from uuid import UUID
                    scan_id = str(UUID(scan_id))
                    if data.get('request_key') != scan_id.replace('-', ''):
                        raise ValueError('NFC operation key mismatch')
                    if broker:
                        broker._route_prune()
                    if broker and scan_id in broker.requests:
                        broker.validate_assignment(user.id, scan_id, data)
                    else:
                        # Core restarts discard scan delivery, not durable operations.
                        fields = coordinator.request_fields(data)
                        existing = await hass.async_add_executor_job(coordinator.existing, fields)
                        if existing is None:
                            raise ValueError('NFC request unavailable')
                result = await coordinator.assign(data)
                if scan_id is not None and broker and result.get('status') == 'confirmed':
                    # Completion bookkeeping must not fail an already durable commit.
                    try:
                        broker.route_complete(user.id, scan_id)
                    except ValueError:
                        pass
                await publish()
                return self.json(result)
            except Conflict as error:
                return self.json({'error':str(error)}, status_code=409)
            except (KeyError, ValueError, TypeError):
                return self.json({'error':'Invalid assignment request'}, status_code=400)

    hass.http.register_view(AssignmentView())
    add_extra_js_url(hass, '/quack-materials/slot_picker.js?v=20261007.3')
    return coordinator


async def assignment_status(hass, coordinator):
    return dict(enabled=True, slots=coordinator.config['slots'], profiles=PROFILES,
                slot_states={slot:coordinator.read_slot(slot) for slot in coordinator.config['slots']},
                ready=coordinator.ready(), pending=await hass.async_add_executor_job(coordinator.pending),
                metadata_sync=await hass.async_add_executor_job(coordinator.metadata_status))
