"""Read-only, instance-scoped tap of received Bambu P2S push-status frames.

HA state timestamps do not establish packet freshness when a value is unchanged.
Only successful incoming model updates advance this cache. A throttled read-only
status request may recover its full baseline; heater and actuator writes are never
issued here. Unknown/incomplete protocol state cannot authorize heat.
"""
import asyncio
import math
import re
import threading
import time


_FIELDS = ('status', 'current_stage', 'chamber_temp', 'bed_temp', 'bed_target', 'door_closed',
           'air_mode', 'idle_protection', 'maintenance_mode')
_STATES = {'idle', 'finish', 'failed', 'running', 'prepare', 'pause'}


def _mapping(value):
    if not isinstance(value, dict):
        raise ValueError('Expected status object')
    return value


def _integer(value, maximum=(1 << 64) - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError('Invalid status integer')
    return value


def _stage_integer(value):
    # The installed integration maps signed -1 and unsigned 255 to idle.
    if (type(value) is int and value == -1) or (isinstance(value, str) and value == '-1'):
        return 255
    if isinstance(value, str) and re.fullmatch(r'[0-9]{1,3}', value):
        value = int(value)
    return _integer(value, 255)


def _hex(value):
    if isinstance(value, str):
        if not value or len(value) > 18:
            raise ValueError('Invalid status bits')
        value = int(value, 16)
    return _integer(value)


def _temperature(value):
    if type(value) not in (int, float) or not math.isfinite(value) or not -273 <= value <= 1000:
        raise ValueError('Invalid temperature')
    return value


def _decode(data):
    values = {}
    if 'gcode_state' in data:
        state = data['gcode_state']
        if not isinstance(state, str) or state.lower() not in _STATES:
            raise ValueError('Unknown print status')
        values['status'] = state.lower()
    for raw, name in (('chamber_temper', 'chamber_temp'), ('bed_temper', 'bed_temp'),
                      ('bed_target_temper', 'bed_target')):
        if raw in data:
            values[name] = _temperature(data[raw])
    # Match the installed integration's StageAction fields and precedence.
    # mc_print_stage describes another protocol value and is never stage evidence.
    if 'stage' in data and data['stage'] is not None:
        stage = _mapping(data['stage'])
        if '_id' in stage:
            values['current_stage'] = _stage_integer(stage['_id'])
    if 'stg_cur' in data:
        values['current_stage'] = _stage_integer(data['stg_cur'])
    if data.get('print_type') == 'idle' and values.get('current_stage') == 0:
        values['current_stage'] = 255
    if 'stat' in data and data['stat'] is not None:
        values['door_closed'] = not bool(_hex(data['stat']) & (1 << 23))
    if 'cfg' in data and data['cfg'] is not None:
        mode = (_hex(data['cfg']) >> 32) & 3
        if mode == 3:
            raise ValueError('Unknown idle protection mode')
        values['idle_protection'] = mode == 1
        values['maintenance_mode'] = mode == 2
    if 'device' in data:
        device = _mapping(data['device'])
        packed = None
        if 'bed' in device:
            bed = _mapping(device['bed'])
            if 'info' in bed:
                info = _mapping(bed['info'])
                if 'temp' in info:
                    packed = _integer(info['temp'], (1 << 32) - 1)
        if packed is None and 'bed_temp' in device:
            packed = _integer(device['bed_temp'], (1 << 32) - 1)
        if packed is not None:
            values['bed_temp'] = _temperature(packed & 0xffff)
            values['bed_target'] = _temperature(packed >> 16)
        if 'ctc' in device:
            ctc = _mapping(device['ctc'])
            if 'info' in ctc:
                info = _mapping(ctc['info'])
                if 'temp' in info:
                    values['chamber_temp'] = _temperature(_integer(info['temp'], (1 << 32) - 1) & 0xffff)
        if 'airduct' in device:
            airduct = _mapping(device['airduct'])
            if 'modeCur' in airduct:
                mode = _integer(airduct['modeCur'], 1)
                values['air_mode'] = 'internal' if mode == 1 else 'external'
    return values


_PROTOCOL_PATHS = ('gcode_state', 'msg', 'stage._id', 'stg_cur', 'print_type', 'stat', 'cfg',
    'chamber_temper', 'bed_temper', 'bed_target_temper', 'device.bed_temp',
    'device.bed.info.temp', 'device.ctc.info.temp', 'device.airduct.modeCur')
_SAFE_ERRORS = {'Expected status object', 'Invalid status integer', 'Invalid status bits',
    'Invalid temperature', 'Unknown print status', 'Unknown idle protection mode',
    'Unknown push-status frame type', 'Invalid receive clock',
    'Delta baseline expired; require a fresh full frame', 'Unknown safety flags'}


def _diagnostic_scalar(value):
    if value is None or type(value) is bool:
        return value
    if type(value) in (int, float):
        return value if abs(value) <= (1 << 64) and math.isfinite(value) else '<invalid-number>'
    if isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_+.-]{0,32}', value):
        return value
    return '<' + type(value).__name__[:24] + '>'


def _protocol_diagnostics(data):
    if not isinstance(data, dict):
        return [], {'payload_type': type(data).__name__[:24]}
    keys = sorted(key for key in data if isinstance(key, str)
                  and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,63}', key)
                  and not re.search(r'password|secret|token|access|credential|ssid', key, re.I))[:48]
    fields = {}
    for path in _PROTOCOL_PATHS:
        value = data
        for part in path.split('.'):
            if not isinstance(value, dict) or part not in value:
                break
            value = value[part]
        else:
            fields[path] = _diagnostic_scalar(value)
    return keys, fields


class PreheatTelemetry:
    """Capture raw observations and detach without altering other instances."""
    def __init__(self, hass, printer_id, clock=time.time):
        self.hass = hass
        self.printer_id = printer_id
        self.clock = clock
        self._lock = threading.RLock()
        self._values = {}
        self._observations = {}
        self._base_valid = False
        self._received_at = None
        self._door_closed = None
        self._door_open_count = 0
        self._model = self._original = self._wrapper = None
        self._had_instance_method = False
        self._attach_reason = 'not_attached'
        self._last_received_at = None
        self._last_command = None
        self._decode_error = None
        self._packet_keys = []
        self._protocol_fields = {}
        self._coordinator = None
        self._refresh_requested_at = None
        self._refresh_succeeded_at = None
        self._refresh_error = None
        self._refresh_pending = False

    def _invalidate(self):
        self._values.clear()
        self._observations.clear()
        self._base_valid = False
        self._received_at = None
        self._door_closed = None

    def _record_packet(self, data):
        now = self.clock()
        self._last_received_at = now if type(now) in (int, float) and math.isfinite(now) else None
        self._last_command = _diagnostic_scalar(data.get('command')) if isinstance(data, dict) else None
        self._packet_keys, self._protocol_fields = _protocol_diagnostics(data)

    def ingest(self, data):
        """Observe one already decoded incoming print object without mutating it."""
        with self._lock:
            self._record_packet(data)
            if isinstance(data, dict) and data.get('command') != 'push_status':
                return
            try:
                data = _mapping(data)
                msg = data.get('msg', 0)
                if type(msg) is not int or msg not in (0, 1):
                    raise ValueError('Unknown push-status frame type')
                values = _decode(data)
                if msg == 1 and not self._base_valid:
                    self._decode_error = 'Delta requires a complete full baseline'
                    return
                now = self.clock()
                if type(now) not in (int, float) or not math.isfinite(now):
                    raise ValueError('Invalid receive clock')
                if msg == 1 and (self._received_at is None or not 0 <= now - self._received_at <= 30):
                    raise ValueError('Delta baseline expired; require a fresh full frame')
                self._received_at = now
                if msg == 0:
                    self._values = values
                    self._base_valid = all(name in values for name in _FIELDS)
                else:
                    # Null safety fields are unknown, not unchanged deltas.
                    if any(name in data and data[name] is None for name in ('stat', 'cfg')):
                        raise ValueError('Unknown safety flags')
                    self._values.update(values)
                door_closed = self._values.get('door_closed')
                if door_closed is False and self._door_closed is not False:
                    self._door_open_count += 1
                self._door_closed = door_closed
                self._observations = dict(self._values)
                self._observations.update({name + '_at': now for name in self._values})
                self._decode_error = None if self._base_valid else 'Incomplete full status'
            except (ValueError, TypeError, OverflowError) as error:
                message = str(error)
                self._decode_error = message if message in _SAFE_ERRORS else type(error).__name__ + ' while decoding protocol fields'
                self._invalidate()

    def read(self):
        """Return a stable copy; reading cannot renew packet timestamps."""
        with self._lock:
            attached = self._model is not None and self._model.print_update is self._wrapper
            diagnostic = {'attached': attached,
                'attach_reason': self._attach_reason if attached or self._attach_reason != 'attached' else 'hook_replaced',
                'last_received_at': self._last_received_at, 'last_command': self._last_command,
                'decode_error': self._decode_error, 'packet_keys': list(self._packet_keys),
                'protocol_fields': dict(self._protocol_fields), 'base_valid': self._base_valid,
                'missing_fields': [name for name in _FIELDS if name not in self._values],
                'refresh_requested_at': self._refresh_requested_at,
                'refresh_succeeded_at': self._refresh_succeeded_at,
                'refresh_pending': self._refresh_pending, 'refresh_error': self._refresh_error}
            return dict(self._observations, printer_id=self.printer_id,
                        door_open_count=self._door_open_count, telemetry_diagnostics=diagnostic)

    def attach(self):
        """Find the exact configured P2S and wrap only its print-update method."""
        with self._lock:
            entries = getattr(self.hass, 'data', {}).get('bambu_lab', {})
            if not isinstance(entries, dict):
                self.close()
                self._attach_reason = 'invalid_integration_registry'
                return False
            candidates = []
            for coordinator in list(entries.values()):
                try:
                    model = coordinator.get_model()
                    if (model.info.serial == self.printer_id and
                            model.info.device_type == 'P2S' and callable(model.print_update)):
                        candidates.append((coordinator, model))
                except (AttributeError, TypeError):
                    continue
            if len(candidates) != 1:
                self.close()
                self._attach_reason = 'integration_unavailable' if not entries else 'matching_p2s_not_found'
                return False
            coordinator, model = candidates[0]
            if self._model is model and model.print_update is self._wrapper:
                return True
            self.close()
            original = model.print_update

            def observed_update(data, *args, **kwargs):
                try:
                    result = original(data, *args, **kwargs)
                except Exception as error:
                    with self._lock:
                        try:
                            self._record_packet(data)
                        except Exception:
                            pass  # Diagnostics cannot replace the original observer error.
                        self._decode_error = 'Original update failed: ' + type(error).__name__
                        self._invalidate()
                    raise
                try:
                    with self._lock:
                        if self._model is model and self._wrapper is observed_update:
                            self.ingest(data)
                except Exception as error:
                    # Capture must never break the integration's normal observer.
                    with self._lock:
                        self._decode_error = 'Capture failed: ' + type(error).__name__
                        self._invalidate()
                return result

            self._had_instance_method = 'print_update' in getattr(model, '__dict__', {})
            try:
                model.print_update = observed_update
            except (AttributeError, TypeError):
                self._attach_reason = 'hook_not_writable'
                return False
            self._model, self._original, self._wrapper = model, original, observed_update
            self._attach_reason = 'attached'
            self._coordinator = coordinator
            return True

    async def refresh_if_needed(self):
        """Request a real full status from the exact paired client, at most once/minute.

        The installed Bambu client refresh publishes GET_VERSION and PUSH_ALL only.
        A successful request does not establish any observation or receive timestamp.
        """
        with self._lock:
            if self._model is None or self._model.print_update is not self._wrapper or self._refresh_pending:
                return False
            now = self.clock()
            if type(now) not in (int, float) or not math.isfinite(now):
                return False
            if self._base_valid:
                if self._received_at is not None and 0 <= now - self._received_at <= 30:
                    return False
                self._invalidate()
                self._decode_error = 'Full baseline expired; require a fresh full frame'
            if self._refresh_requested_at is not None and now - self._refresh_requested_at < 60:
                return False
            coordinator = self._coordinator
            entries = getattr(self.hass, 'data', {}).get('bambu_lab', {})
            try:
                if (not isinstance(entries, dict) or not any(entry is coordinator for entry in entries.values()) or
                        coordinator.get_model() is not self._model or self._model.info.serial != self.printer_id or
                        self._model.info.device_type != 'P2S'):
                    self._refresh_error = 'paired_coordinator_changed'
                    return False
                refresh = coordinator.client.refresh
                if not callable(refresh):
                    self._refresh_error = 'client_refresh_unavailable'
                    return False
            except (AttributeError, TypeError):
                self._refresh_error = 'client_refresh_unavailable'
                return False
            self._refresh_requested_at = now
            self._refresh_pending = True
            self._refresh_error = None
        try:
            await asyncio.wait_for(refresh(), timeout=5)
        except Exception as error:
            with self._lock:
                self._refresh_error = type(error).__name__
            return False
        else:
            with self._lock:
                self._refresh_succeeded_at = now
            return True
        finally:
            with self._lock:
                self._refresh_pending = False

    def close(self):
        """Restore our own wrapper only, preserving any later replacement."""
        with self._lock:
            if self._model is not None and self._model.print_update is self._wrapper:
                if self._had_instance_method:
                    self._model.print_update = self._original
                else:
                    del self._model.print_update
            self._model = self._original = self._wrapper = None
            self._attach_reason = 'closed'
            self._coordinator = None
            self._invalidate()
