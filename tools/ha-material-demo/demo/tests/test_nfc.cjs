const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const {webcrypto} = require('node:crypto');
const file = 'demo/custom_components/quack_material_demo/nfc.js';
assert.ok(fs.existsSync(file), 'NFC frontend is not implemented');
const context = {window: {}, crypto: webcrypto, URL, Date, setTimeout, clearTimeout};
vm.runInNewContext(fs.readFileSync(file, 'utf8'), context);
const lib = context.window.QuackNfc;
const roll = '11111111-1111-4111-8111-111111111111';
const calls = [];
const hass = {user: {id: 'user', is_admin: true}, auth: {external: {
  config: {canWriteTag: true}, fireMessage: msg => calls.push(msg)
}}};
function storage() {const data = new Map(); return {getItem: k => data.get(k) || null, setItem: (k,v) => data.set(k,v), removeItem: k => data.delete(k)};}
function fixture(saved = storage()) {
  let now = 1000000, response = {paired: true, scan: {id: 'scan-1', spool_uuid: roll, expires_in: 90}};
  let visible = true, failAck = false, delay = 0;
  const calls = [], routes = [], card = {view: 'rolls', data: {}, isConnected: true, editing: false,
    consumeNfc(scan) {this.shown = (this.shown || 0) + 1; this.last = scan; return !this.editing;}};
  saved.setItem('quack:nfc:client:user', 'a'.repeat(64));
  const router = lib.createController({storage: saved, now: () => now, getHass: () => hass,
    visible: () => visible, navigate: path => routes.push(path), clearRoute() {},
    api: async (action, data) => {calls.push({action,data}); if (action === 'ack' && failAck) throw Error('offline'); if (action === 'poll') now += delay; return action === 'poll' ? response : {}; }});
  return {router, card, calls, routes, saved,
    time: value => {now = value;}, response: value => {response = value;},
    visible: value => {visible = value;}, failAck: value => {failAck = value;}, delay: value => {delay = value;}};
}
async function test(name, fn) {await fn(); console.log('PASS ' + name);}
(async () => {
  await test('stock picker outside inventory defers NFC navigation and delivery', async () => {
    const f=fixture();context.window.QuackSlots={isOpen:()=>true};
    f.router.attach(f.card);await f.router.tick();await f.router.deliver();
    assert.equal(f.routes.length,0);assert.equal(f.card.shown||0,0);
    assert.equal(f.calls.filter(c=>c.action==='ack').length,0);
    context.window.QuackSlots={isOpen:()=>false};
    await f.router.tick();assert.equal(f.card.shown,1);
  });
  await test('writer uses existing HA helper and sends only logical UUID and name', async () => {
    assert.equal(lib.canWrite(hass), true);
    const result = await lib.writeTag(hass, {uuid: roll.toUpperCase(), product: 'PLA roll'});
    assert.deepEqual(JSON.parse(JSON.stringify(calls.pop())), {type: 'tag/write', payload: {tag: roll, name: 'PLA roll'}});
    assert.equal(result.written, false);
    assert.equal(result.dialog_requested, true);
    await assert.rejects(() => lib.writeTag(hass, {uuid: 'https://example.org', product: 'bad'}));
  });
  await test('raw iOS fallback sends serialized JSON and unique numeric message IDs', async () => {
    const sent = [], native = {webkit: {messageHandlers: {externalBus: {postMessage: value => sent.push(value)}}}};
    const h = {auth: {external: {config: {canWriteTag: true}}}};
    await lib.writeTag(h, {uuid: roll, product: 'PLA'}, native);
    await lib.writeTag(h, {uuid: roll, product: 'PLA'}, native);
    assert.equal(typeof sent[0], 'string');
    const one = JSON.parse(sent[0]), two = JSON.parse(sent[1]);
    assert.ok(Number.isSafeInteger(one.id)); assert.notEqual(one.id, two.id);
    assert.equal(one.payload.tag, roll);
  });
  await test('desktop, unsupported app and thrown native errors cannot claim a write', async () => {
    assert.equal(lib.canWrite({}), false);
    assert.equal(lib.canWrite({auth: {external: {config: {canWriteTag: false}, fireMessage() {}}}}), false);
    await assert.rejects(() => lib.writeTag({}, {uuid: roll}));
    const h = {auth: {external: {config: {canWriteTag: true}, fireMessage() {throw Error('native failure');}}}};
    await assert.rejects(() => lib.writeTag(h, {uuid: roll}), /native failure/);
  });
  await test('late card gets pending request; acknowledge occurs after display', async () => {
    const f = fixture(); await f.router.tick();
    assert.ok(f.routes[0].startsWith('/dashboard-filament/rolls?'));
    assert.equal(f.calls.filter(x => x.action === 'ack').length, 0);
    f.router.attach(f.card); await f.router.deliver();
    assert.equal(f.card.shown, 1);
    assert.equal(f.calls.filter(x => x.action === 'ack').length, 1);
  });
  await test('failed acknowledgement and frontend reload never reopen a displayed scan', async () => {
    const f = fixture(); f.failAck(true); f.router.attach(f.card); await f.router.tick(); await f.router.tick();
    assert.equal(f.card.shown, 1);
    const again = fixture(f.saved); again.router.attach(again.card); await again.router.tick();
    assert.equal(again.card.shown || 0, 0);
    assert.equal(again.calls.filter(x => x.action === 'ack').length, 1);
  });
  await test('expired, invisible and unpaired clients do not navigate', async () => {
    const f = fixture(); f.visible(false); await f.router.tick(); assert.equal(f.calls.length, 0);
    f.visible(true); f.response({paired: false, scan: null}); await f.router.tick(); assert.equal(f.routes.length, 0);
    f.response({paired: true, scan: {id: 'old', spool_uuid: roll, expires_in: 0}}); await f.router.tick();
    assert.equal(f.routes.length, 0);
  });
  await test('active assignment is preserved until closed; later scans on same page are handled', async () => {
    const f = fixture(); f.card.editing = true; f.router.attach(f.card); await f.router.tick();
    assert.equal(f.card.shown || 0, 0);
    f.card.editing = false; await f.router.deliver(); assert.equal(f.card.shown, 1);
    f.response({paired: true, scan: {id: 'scan-2', spool_uuid: roll, expires_in: 90}});
    await f.router.tick(); assert.equal(f.card.shown, 2);
  });
  await test('pending scan expires while frontend is unavailable', async () => {
    const f = fixture(); await f.router.tick(); f.time(1100000); f.router.attach(f.card); await f.router.deliver();
    assert.equal(f.card.shown || 0, 0);
    assert.equal(f.calls.filter(x => x.action === 'ack').length, 0);
  });
  await test('scan delivery works from every internal inventory tab', async () => {
    for (const view of ['slots', 'customers', 'orders', 'prints', 'history', 'sync']) {
      const f = fixture(); f.card.view = view; f.router.attach(f.card); await f.router.tick();
      assert.equal(f.card.shown, 1, view);
      assert.equal(f.calls.filter(x => x.action === 'ack').length, 1, view);
    }
  });
  await test('delayed poll response cannot extend an expired scan', async () => {
    const f = fixture(); f.router.attach(f.card); f.delay(5000);
    f.response({paired: true, scan: {id: 'delayed', spool_uuid: roll, expires_in: 1}});
    await f.router.tick();
    assert.equal(f.card.shown || 0, 0); assert.equal(f.routes.length, 0);
    assert.equal(f.calls.filter(x => x.action === 'ack').length, 0);
  });
  await test('consumed marker survives reload until server expiry even with conservative deadlines', async () => {
    const f = fixture(); f.router.attach(f.card); f.delay(500); f.failAck(true);
    f.response({paired: true, scan: {id: 'scan-1', spool_uuid: roll, expires_in: 1.5}});
    await f.router.tick(); assert.equal(f.card.shown, 1);
    const again = fixture(f.saved); again.time(1001600); again.router.attach(again.card);
    again.response({paired: true, scan: {id: 'scan-1', spool_uuid: roll, expires_in: .4}});
    await again.router.tick(); assert.equal(again.card.shown || 0, 0);
    assert.equal(again.calls.filter(x => x.action === 'ack').length, 1);
  });
})().catch(error => {console.error(error); process.exitCode = 1;});
