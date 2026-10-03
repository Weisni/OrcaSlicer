const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const {webcrypto} = require('node:crypto');
const roll = '11111111-1111-4111-8111-111111111111';
const messages = [], requests = [];
const ctx = {window: {}, URL, crypto: webcrypto, HTMLElement: class {}, customElements: {define() {}},
  location: {href: 'http://homeassistant.local:8123/dashboard-filament/rolls'}};
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/nfc.js', 'utf8'), ctx);
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/inventory.js', 'utf8') + ';globalThis.Card=QuackInventoryCard;', ctx);
function card() {
  const item = Object.create(ctx.Card.prototype);
  Object.assign(item, {view: 'rolls', query: '', message: '', _hass: {locale: {language: 'de'}, user: {id: 'user', is_admin: true},
    auth: {external: {config: {canWriteTag: true}, fireMessage: message => messages.push(message)}}},
    data: {can_edit: true, spools: [{uuid: roll, product: 'PLA roll', status: 'active', remaining_mg: 100000}], slots: []},
    shadowRoot: {querySelectorAll() {return [];}, querySelector(selector) {return selector === '.search' ? {} : null;}}});
  item.api = () => {throw Error('NFC must not mutate inventory');};
  return item;
}
(async () => {
  const item = card(); item.render();
  assert.match(item.shadowRoot.innerHTML, /data-action="nfc-write"/, 'NFC row action missing');
  let button = item.shadowRoot.innerHTML.match(/<button[^>]*data-action="nfc-write"[^>]*>/)[0];
  assert.match(button, /aria-label="NFC schreiben"/); assert.doesNotMatch(button, /disabled/);
  await item.handle('nfc-write', roll);
  assert.equal(messages.length, 1); assert.equal(messages[0].payload.tag, roll);
  assert.match(item.message, /angefordert/); assert.doesNotMatch(item.message, /erfolgreich|gespeichert/i);
  delete item._hass.auth.external; item.render();
  button = item.shadowRoot.innerHTML.match(/<button[^>]*data-action="nfc-write"[^>]*>/)[0];
  assert.match(button, /disabled/); assert.match(button, /Companion/);
  console.log('PASS NFC action uses native writer, explains desktop limitation and does not claim success');

  const scanCard = card(); let shown;
  scanCard.assignRoll = id => {shown = id;};
  assert.equal(scanCard.consumeNfc({spool_uuid: roll}), true); assert.equal(shown, roll);
  for (const view of ['slots', 'customers', 'orders', 'prints', 'history', 'sync']) {
    scanCard.view = view; shown = null;
    assert.equal(scanCard.consumeNfc({spool_uuid: roll}), true, view);
    assert.equal(scanCard.view, 'rolls'); assert.equal(shown, roll);
  }
  scanCard.editing = true; shown = null;
  assert.equal(scanCard.consumeNfc({spool_uuid: roll}), false); assert.equal(shown, null);
  scanCard.editing = false;
  // Real assignRoll validates these states before opening any editor.
  scanCard.assignRoll = ctx.Card.prototype.assignRoll;
  scanCard.data.spools[0].status = 'archived';
  assert.equal(scanCard.consumeNfc({spool_uuid: roll}), true); assert.match(scanCard.message, /Archived/);
  scanCard.data.spools[0].status = 'active'; scanCard.data.spools[0].remaining_mg = 0;
  scanCard.consumeNfc({spool_uuid: roll}); assert.match(scanCard.message, /empty/);
  scanCard.consumeNfc({spool_uuid: '00000000-0000-4000-8000-000000000000'}); assert.match(scanCard.message, /Unknown/);
  console.log('PASS NFC reuses assignment and handles archived, empty and unknown rolls without saving');

  const linked = card();
  ctx.window.QuackNfc.request = async (action, data) => {
    requests.push({action, data});
    return {devices: [{id: 'phone-a', name: 'My iPhone'}, {id: 'phone-b', name: 'Other iPhone'}]};
  };
  let save;
  linked.editor = (title, fields, submit) => {
    assert.match(fields, /<option value=""[^>]*>/);
    assert.match(fields, /My iPhone/); assert.match(fields, /Other iPhone/);
    save = submit;
  };
  await linked.handle('nfc-link', '');
  assert.deepEqual(requests.map(r => r.action), ['devices']);
  await assert.rejects(() => save(new Map([['device_id', '']])));
  await save(new Map([['device_id', 'phone-a']]));
  assert.deepEqual(JSON.parse(JSON.stringify(requests.at(-1))), {action: 'bind', data: {device_id: 'phone-a', confirmed: true}});
  console.log('PASS phone linking requires explicit selection and confirmation');
})().catch(error => {console.error(error); process.exitCode = 1;});
