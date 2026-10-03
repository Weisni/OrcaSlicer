// Globally loaded Companion adapter. No inventory or printer mutations on scans.
(function(root) {
  'use strict';
  if (root.QuackNfc) return;
  const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
  const ROUTE = '/dashboard-filament/rolls';
  let messageId = Math.floor(Date.now() * 1000);

  function canWrite(hass, native = root) {
    return hass?.auth?.external?.config?.canWriteTag === true && (
      typeof hass.auth.external.fireMessage === 'function' ||
      typeof native.webkit?.messageHandlers?.externalBus?.postMessage === 'function' ||
      typeof native.externalAppV2?.postMessage === 'function' ||
      typeof native.externalApp?.externalBus === 'function');
  }

  async function writeTag(hass, roll, native = root) {
    if (!canWrite(hass, native)) throw Error('Open this page in an NFC-capable Home Assistant Companion app.');
    if (!UUID.test(roll?.uuid || '')) throw Error('Invalid roll UUID');
    const message = {type: 'tag/write', payload: {tag: roll.uuid.toLowerCase(), name: roll.product || null}};
    if (typeof hass.auth.external.fireMessage === 'function') {
      await hass.auth.external.fireMessage(message);
    } else {
      const serialized = JSON.stringify({id: ++messageId, ...message});
      if (native.webkit?.messageHandlers?.externalBus) native.webkit.messageHandlers.externalBus.postMessage(serialized);
      else if (native.externalAppV2) native.externalAppV2.postMessage(serialized);
      else native.externalApp.externalBus(serialized);
    }
    // tag/write is fire-and-forget: native success/cancellation is not reported.
    return {dialog_requested: true, written: false};
  }

  function clientKey(storage, user, create = false) {
    const name = 'quack:nfc:client:' + user;
    let key = storage.getItem(name);
    if (!/^[0-9a-f]{64}$/.test(key || '')) {
      if (!create) return null;
      const bytes = new Uint8Array(32);
      (root.crypto || crypto).getRandomValues(bytes);
      key = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
      storage.setItem(name, key);
    }
    return key;
  }

  function createController(env) {
    const state = {pending: null, card: null, busy: false, delivering: false, error: '', user: null,
      routed: null, epoch: 0, paired: false, memory: new Map()};
    const now = env.now || Date.now;
    const visible = env.visible || (() => true);
    const consumedKey = user => 'quack:nfc:consumed:' + user;
    const companion = hass => !!hass?.auth?.external && hass?.user?.is_admin === true;
    function consumed(user) {
      let saved;
      try {saved = JSON.parse(env.storage.getItem(consumedKey(user)) || '{}');} catch (_) {saved = {};}
      if (!saved || typeof saved !== 'object' || Array.isArray(saved)) saved = {};
      return Object.fromEntries(Object.entries(saved).filter(([, expiry]) => Number.isFinite(expiry) && expiry > now()));
    }
    async function acknowledge(pending) {
      try {
        await env.api('ack', {client_key: pending.key, request_id: pending.id});
        if (state.pending?.id === pending.id) state.pending = null;
      } catch (_) {state.error = 'Scan displayed; acknowledgement will retry when connected.';}
    }
    return Object.assign(state, {
      attach(card) {state.card = card;},
      detach(card) {if (state.card === card) state.card = null;},
      reset() {state.epoch++; state.pending = null; state.routed = null; state.error = '';},
      async tick() {
        const hass = env.getHass();
        if (!companion(hass) || !visible()) {state.pending = null; return;}
        const user = hass.user.id;
        if (state.user !== user) {this.reset(); state.user = user;}
        if (state.busy) return;
        state.busy = true;
        try {
          const key = clientKey(env.storage, user);
          if (!key) {state.paired = false; return;}
          const epoch = state.epoch;
          const requestedAt = now();
          const result = await env.api('poll', {client_key: key});
          if (epoch !== state.epoch || env.getHass()?.user?.id !== user || !visible()) return;
          state.paired = result.paired === true;
          state.error = '';
          const scan = result.scan;
          if (!state.paired || !scan || !UUID.test(scan.spool_uuid || '') ||
              typeof scan.id !== 'string' || !Number.isFinite(scan.expires_in) || scan.expires_in <= 0) {
            state.pending = null;
            return;
          }
          // Include network delay and app suspension in the remaining lifetime.
          const deadline = requestedAt + Math.min(90, scan.expires_in) * 1000;
          if (now() >= deadline) {state.pending = null; return;}
          state.pending = {...scan, user, key, deadline};
          const saved = consumed(user);
          if (saved[scan.id] || state.memory.has(user + ':' + scan.id)) {
            await acknowledge(state.pending);
            return;
          }
          if (root.QuackSlots?.isOpen?.() || (state.card?.isConnected !== false && state.card?.editing)) return;
          if (state.routed !== scan.id) {
            env.navigate(ROUTE + '?spool=' + encodeURIComponent(scan.spool_uuid) + '&nfc_request=' + encodeURIComponent(scan.id));
            state.routed = scan.id;
          }
          await this.deliver();
        } catch (error) {
          state.error = error?.body?.error || error?.message || 'NFC connection unavailable';
        } finally {state.busy = false;}
      },
      async deliver() {
        const pending = state.pending, card = state.card, hass = env.getHass();
        if (root.QuackSlots?.isOpen?.() || state.delivering || !pending || !visible() || !companion(hass) ||
            hass.user.id !== pending.user || now() >= pending.deadline ||
            !card || card.isConnected === false || !card.data || card.editing || card.busy) return;
        state.delivering = true;
        try {
          const saved = consumed(pending.user);
          if (!saved[pending.id] && !state.memory.has(pending.user + ':' + pending.id)) {
            if (!card.consumeNfc(pending)) return;
            // Retain deduplication beyond the conservative delivery deadline:
            // the server's actual expiry can be slightly later after a slow poll.
            const retainUntil = now() + 90000;
            state.memory.set(pending.user + ':' + pending.id, retainUntil);
            saved[pending.id] = retainUntil;
            env.storage.setItem(consumedKey(pending.user), JSON.stringify(saved));
            env.clearRoute(pending.id);
          }
          await acknowledge(pending);
          for (const [key, expiry] of state.memory) if (expiry <= now()) state.memory.delete(key);
        } catch (error) {
          state.error = error?.message || 'NFC request could not be displayed';
        } finally {state.delivering = false;}
      }
    });
  }

  const api = {canWrite, writeTag, clientKey, createController};
  root.QuackNfc = api;
  if (!root.document || !root.setInterval) return;

  const getHass = () => root.document.querySelector('home-assistant')?.hass;
  async function request(action, data) {
    const hass = getHass();
    if (!hass?.user?.is_admin || !hass?.auth?.external) throw Error('Open this page in the Companion app with inventory access.');
    let timeout;
    try {
      return await Promise.race([
        hass.callApi('POST', 'quack_material_demo/nfc/' + action, data),
        new Promise((_, reject) => {timeout = root.setTimeout(() => reject(Error('NFC connection timed out')), 15000);})
      ]);
    } finally {root.clearTimeout(timeout);}
  }
  api.request = async (action, data = {}) => {
    const hass = getHass();
    if (!hass?.user) throw Error('Home Assistant is still connecting');
    const key = clientKey(root.localStorage, hass.user.id, true);
    const result = await request(action, {...data, client_key: key});
    if (action === 'bind' || action === 'unbind') api.controller.reset();
    return result;
  };
  api.controller = createController({
    getHass, api: request,
    storage: {getItem: key => root.localStorage.getItem(key), setItem: (key, value) => root.localStorage.setItem(key, value)},
    visible: () => root.document.visibilityState !== 'hidden',
    navigate: path => {
      root.history.pushState(null, '', path);
      root.dispatchEvent(new CustomEvent('location-changed', {detail: {replace: false}}));
    },
    clearRoute: id => {
      const url = new URL(root.location.href);
      if (url.searchParams.get('nfc_request') !== id) return;
      url.searchParams.delete('nfc_request'); url.searchParams.delete('spool');
      root.history.replaceState(root.history.state, '', url.pathname + url.search + url.hash);
    }
  });
  const tick = () => {void api.controller.tick();};
  root.setInterval(tick, 2000);
  root.document.addEventListener('visibilitychange', tick);
  root.addEventListener('online', tick);
  root.addEventListener('focus', tick);
  tick();
})(typeof window === 'undefined' ? globalThis : window);
