"""Serialized HA adapter for the durable job-bound preheat policy.

No printer commands are sent without an admitted attempt. Stopping the adapter
only revokes refreshes; the printer's enabled idle protection remains responsible
for cooling after loss of HA. Dispatch owns temperatures after atomic handoff.
"""
import asyncio
import logging

from .preheat import Preheat
from .preheat_telemetry import PreheatTelemetry
from .store import Conflict


class PreheatRuntime:
    def __init__(self, hass, store, settings, publish=None):
        self.hass, self.store, self.settings = hass, store, settings
        self.publish = publish
        self.lock = asyncio.Lock()
        self.core = None
        self.enabled = False
        self.stopped = False
        self._cancel_timer = None
        self._published_signature = ()
        self._claim_veto = True
        self.physical = settings.get('provider_printer_id')
        self.telemetry = PreheatTelemetry(hass, self.physical)
        status = settings.get('printer_state_entity', '')
        prefix = status[len('sensor.'):-len('print_status')] if status.startswith('sensor.') and status.endswith('_print_status') else None
        if not prefix or not self.physical:
            raise ValueError('Job preheat requires a paired printer and print-status sensor')
        self.status_entity = status
        self.stage_entity = settings.get('printer_assignment', {}).get('stage_entity', 'sensor.' + prefix + 'current_stage')
        self.bed_entity = settings.get('preheat_bed_entity', 'number.' + prefix + 'bed_target_temperature')
        self.air_entity = settings.get('preheat_air_entity', 'select.' + prefix + 'airduct_mode')
        if not isinstance(self.stage_entity, str) or not self.stage_entity.startswith('sensor.'):
            raise ValueError('Preheat stage entity must be a sensor')
        if not isinstance(self.bed_entity, str) or not self.bed_entity.startswith('number.'):
            raise ValueError('Preheat bed target must be a number entity')
        if not isinstance(self.air_entity, str) or not self.air_entity.startswith('select.'):
            raise ValueError('Preheat air mode must be a select entity')

    async def start(self):
        from datetime import timedelta
        from homeassistant.helpers.event import async_track_time_interval
        self.core = await self.hass.async_add_executor_job(
            lambda: Preheat(self.store, observation_reader=self._claim_observation))
        await self.hass.async_add_executor_job(self.core.reconcile_modes,
            self.settings.get('job_preheat_enabled', False), self.settings.get('manual_preheat_enabled', False))
        self.store.preheat_guard = self.core
        self.store.preheat = self.core if self.settings.get('job_preheat_enabled', False) else None
        self.telemetry.attach()
        await self.telemetry.refresh_if_needed()
        self.enabled = True
        self._cancel_timer = async_track_time_interval(self.hass, self.tick, timedelta(seconds=5))

    def _observation(self):
        result = self.telemetry.read()
        # Cached entity values may only veto an idle raw packet, never grant it.
        status = self.hass.states.get(self.status_entity)
        stage = self.hass.states.get(self.stage_entity)
        if (not status or status.state not in ('idle', 'finish', 'failed') or
                getattr(status, 'attributes', {}).get('restored') or not stage or
                stage.state != 'idle' or getattr(stage, 'attributes', {}).get('restored') or
                result.get('current_stage') != 255):
            result['status'] = 'unknown_or_busy'
        return result

    def _binding(self, entity_id, domain, service):
        from homeassistant.helpers import entity_registry as er
        entity = er.async_get(self.hass).async_get(entity_id)
        if not entity or entity.platform != 'bambu_lab' or not self.hass.services.has_service(domain, service):
            raise Conflict('Configured preheat control is not an available Bambu entity')
        matches = []
        for key, coordinator in self.hass.data.get('bambu_lab', {}).items():
            try:
                model = coordinator.get_model()
                device = coordinator.get_ha_printer_device()
                if (model.info.serial == self.physical and model.info.device_type == 'P2S' and device and
                        entity.device_id == device.id and entity.config_entry_id == key):
                    matches.append(coordinator)
            except (AttributeError, TypeError):
                continue
        if len(matches) != 1:
            raise Conflict('Preheat control does not belong to the paired P2S')
        state = self.hass.states.get(entity_id)
        if not state or state.state in ('unknown', 'unavailable') or getattr(state, 'attributes', {}).get('restored'):
            raise Conflict('Preheat control is unavailable')
        return state

    def _claim_observation(self):
        # Called inside the inventory transaction. The packet tap is thread-safe;
        # HA state access stays on the event loop and can only add a veto.
        result = self.telemetry.read()
        if self.stopped or self._claim_veto or result.get('current_stage') != 255:
            result['status'] = 'unknown_or_busy'
        return result

    async def _send(self, intent):
        success = False
        try:
            if intent['kind'] == 'bed_target':
                entity, domain, service = self.bed_entity, 'number', 'set_value'
                state = self._binding(entity, domain, service)
                minimum, maximum = state.attributes.get('min'), state.attributes.get('max')
                if (type(intent['value']) is not int or type(minimum) not in (int, float) or
                        type(maximum) not in (int, float) or not minimum <= intent['value'] <= min(maximum, 110)):
                    raise Conflict('Bed target exceeds the paired printer control range')
                data = {'entity_id': entity, 'value': intent['value']}
            elif intent['kind'] == 'air_mode' and intent['value'] == 'internal':
                entity, domain, service = self.air_entity, 'select', 'select_option'
                state = self._binding(entity, domain, service)
                if 'heating' not in state.attributes.get('options', []):
                    raise Conflict('Internal heating air mode is unavailable')
                data = {'entity_id': entity, 'option': 'heating'}
            else:
                raise Conflict('Unsupported preheat actuator intent')
            before = self._observation()
            if self.stopped or not await self.hass.async_add_executor_job(self.core.authorize, intent, before):
                return
            after = self._observation()
            # Fresh known door openings and temperature updates revoke readiness,
            # but do not revoke this owner's heater refresh. Unknown/busy states
            # still fail the safety check, and actuator changes remain guarded.
            values = ('printer_id', 'status', 'bed_target', 'air_mode',
                      'idle_protection', 'maintenance_mode')
            if (self.stopped or self.core._safe(after, self.physical, True, allow_open_door=True) or
                    any(before.get(key) != after.get(key) for key in values)):
                return
            await asyncio.wait_for(self.hass.services.async_call(domain, service, data, blocking=True), timeout=10)
            success = True
        except asyncio.CancelledError:
            raise
        except Exception:
            # A timeout is an uncertain send, never proof that nothing changed.
            logging.getLogger(__name__).warning('Job preheat command failed; explicit retry is required')
        finally:
            await self.hass.async_add_executor_job(self.core.acknowledge, intent, success, self._observation())

    async def _tick_locked(self):
        if self.stopped or not self.enabled:
            return
        self.telemetry.attach()
        await self.telemetry.refresh_if_needed()
        intents = await self.hass.async_add_executor_job(self.core.tick, self._observation())
        for intent in intents:
            if self.stopped:
                await self.hass.async_add_executor_job(self.core.acknowledge, intent, False)
            else:
                await self._send(intent)

    async def tick(self, *_):
        async with self.lock:
            await self._tick_locked()
            summaries = await self.hass.async_add_executor_job(self.core.summaries) if self.enabled else []
            signature = tuple((row['job_uuid'], row['phase'], row['reason'], row['version']) for row in summaries)
            changed = signature != self._published_signature
            self._published_signature = signature
        if changed and self.publish and not self.stopped:
            await self.publish()

    async def status(self, job_uuid):
        async with self.lock:
            if self.stopped or not self.enabled:
                raise Conflict('Job preheat is not active')
            return await self.hass.async_add_executor_job(self.core.status, job_uuid, self._observation())

    async def command(self, job_uuid, command, data):
        if command not in ('request', 'cancel', 'skip', 'retry', 'extend'):
            raise ValueError('Unknown preheat command')
        async with self.lock:
            if self.stopped or not self.enabled:
                raise Conflict('Job preheat is not active')
            await self.hass.async_add_executor_job(getattr(self.core, command), job_uuid, data, self._observation())
            await self._tick_locked()
            result = await self.hass.async_add_executor_job(self.core.status, job_uuid, self._observation())
        if self.publish:
            await self.publish()
        return result

    def _manual_available(self):
        if self.stopped or not self.enabled or not self.settings.get('manual_preheat_enabled', False):
            raise Conflict('Manual preheat is not active')

    async def _manual_snapshot_locked(self):
        observation = self._observation()
        result = await self.hass.async_add_executor_job(self.core.manual_status, observation)
        reason = await self.hass.async_add_executor_job(self.core.manual_admission_reason, observation)
        return {'schema_version': 1, 'enabled': True, 'preheat': result,
                'ready_to_start': reason is None, 'blocked_reason': reason, 'telemetry': observation}

    async def manual_status(self):
        async with self.lock:
            self._manual_available()
            return await self._manual_snapshot_locked()

    async def manual_command(self, command, data):
        if command not in ('start', 'cancel'):
            raise ValueError('Unknown manual preheat command')
        expected = ({'job_uuid', 'request_key', 'instance_id', 'printer_id', 'parameters'} if command == 'start'
                    else {'job_uuid', 'request_key', 'instance_id', 'version'})
        if not isinstance(data, dict) or set(data) != expected:
            raise ValueError('Invalid manual preheat command')
        payload = {name: value for name, value in data.items() if name != 'job_uuid'}
        if command == 'start':
            parameters = data['parameters']
            if (not isinstance(parameters, dict) or parameters.get('enabled') is not True or
                    parameters.get('print_bed_target') != parameters.get('preheat_bed_target')):
                raise ValueError('Manual preheat must hold its requested bed target')
        async with self.lock:
            self._manual_available()
            observation = self._observation()
            if command == 'start':
                await self.hass.async_add_executor_job(self.core.request_manual, data['job_uuid'], payload, observation)
            else:
                current = await self.hass.async_add_executor_job(self.core.manual_status, observation)
                if not current or current.get('job_uuid') != data['job_uuid']:
                    raise Conflict('The manual preheat attempt changed; refresh first')
                await self.hass.async_add_executor_job(self.core.cancel, data['job_uuid'], payload, observation)
            await self._tick_locked()
            result = await self._manual_snapshot_locked()
        if self.publish:
            await self.publish()
        return result

    async def dispatch(self, action, data):
        async with self.lock:
            if self.stopped:
                raise Conflict('Job preheat is stopping; retry after HA is available')
            observation = self._observation()
            self._claim_veto = observation.get('status') == 'unknown_or_busy'
            self.core.observation_reader = self._claim_observation
            def commit():
                result = self.store.dispatch(action, data)
                # A terminal manual accounting state also revokes persisted
                # preheat readiness; evaluating never produces device commands.
                for summary in self.core.summaries():
                    self.core.status(summary['job_uuid'], observation)
                return result
            return await self.hass.async_add_executor_job(commit)

    async def stop(self, *_):
        self.stopped = True
        if self._cancel_timer:
            self._cancel_timer()
            self._cancel_timer = None
        async with self.lock:
            self.telemetry.close()
            self.enabled = False
            if getattr(self.store, 'preheat', None) is self.core:
                self.store.preheat = None
            if getattr(self.store, 'preheat_guard', None) is self.core:
                self.store.preheat_guard = None
