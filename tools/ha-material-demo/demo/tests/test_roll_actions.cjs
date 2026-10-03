const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const {webcrypto} = require('node:crypto');
const id = '5b2d8b02-e906-4def-ba78-94465a775b66';
const other = 'f421c3ee-98e3-49e1-955e-01c7dbe9e602';
// DOM doubles cover only the browser boundary; render, handlers, editor and action are real.
function element() {
  return {controls: {}, attributes: {}, setAttribute(name, value) {this.attributes[name] = value;}, focus() {this.focused = true;}, querySelector(selector) { return this.controls[selector] ??= element(); }, remove() { this.removed = true; }};
}
function setup(url = 'http://homeassistant.local:8123/dashboard-filament/rolls') {
  const overlays = [];
  const ctx = {HTMLElement: class {}, customElements: {define() {}}, window: {}, URL, crypto: webcrypto,
    location: {href: url}, FormData: class {constructor(form) {this.values = form.values;} get(key) {return this.values[key];}},
    document: {createElement() {const overlay = element(); overlays.push(overlay); return overlay;}}};
  vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/inventory.js', 'utf8') + ';globalThis.Card=QuackInventoryCard;', ctx);
  const card = Object.create(ctx.Card.prototype);
  Object.assign(card, {view: 'rolls', query: '', message: '', shadowRoot: {
    querySelectorAll() {return [];}, querySelector(sel) {return sel === '.search' ? {} : sel === '.editor' ? this.overlay : null;}, append(overlay) {this.overlay = overlay;}
  }, data: {can_edit: true, revision: 5, spools: [
    {uuid: id, product: 'Rapid PETG', manufacturer: 'Elegoo', material_type: 'PETG', material_preset: '', color: '#FFFFFF', remaining_mg: 286088, available_mg: 286088, reserved_mg: 0, status: 'active'},
    {uuid: other, product: 'Existing PLA', manufacturer: 'Maker', material_type: 'PLA', material_preset: 'Installed PLA', remaining_mg: 100000, status: 'active'}
  ], slots: ['A1', 'A2', 'A3', 'A4', 'HT1', 'EXT'].map((slot, n) => ({id: slot, revision: n + 10, spool_uuid: slot === 'HT1' ? other : null})), customers: [], orders: [], jobs: [], stock_events: []}});
  const calls = [];
  card.api = async (path, body) => {
    calls.push({path, body});
    if (path === 'inventory') return card.data;
    if (card.failure) throw Error(card.failure);
    assert.equal(path, 'action/assign');
    return {slot: body.slot, spool_uuid: body.spool_uuid};
  };
  return {card, overlays, calls};
}
function submit(overlay, values) {
  const form = overlay.querySelector('form');
  form.values = values;
  return form.onsubmit({preventDefault() {}, target: form});
}
async function test(name, run) {await run(); console.log('PASS ' + name);}
(async () => {
  await test('roll rows expose five accessible HA icon actions', () => {
    const {card} = setup(); card.render();
    for (const [action, label] of [['assign-roll', 'Assign'], ['edit-roll', 'Edit'], ['label', 'QR label'], ['weigh', 'Weigh'], ['archive', 'Archive']]) {
      const button = card.shadowRoot.innerHTML.match(new RegExp(`<button[^>]*data-action="${action}"[^>]*>[\\s\\S]*?<\\/button>`));
      assert.ok(button, action + ' row action missing');
      assert.match(button[0], new RegExp(`aria-label="${label}"`));
      assert.match(button[0], new RegExp(`title="${label}"`));
      assert.match(button[0], /<ha-icon icon="mdi:[^"]+" aria-hidden="true"><\/ha-icon>/);
    }
    assert.match(card.shadowRoot.innerHTML, /\.roll-action\{[^}]*min-width:44px[^}]*min-height:44px/);
    assert.match(card.shadowRoot.innerHTML, /button:focus-visible/);
    card.data.spools[0].status = 'archived'; card.render();
    assert.match(card.shadowRoot.innerHTML, /data-action="restore"[^>]*aria-label="Restore"/);
    assert.match(card.shadowRoot.innerHTML, /data-action="assign-roll"[^>]*disabled/);
  });
  await test('standalone row actions have visible SVG fallback while defined HA icons hide the fallback', () => {
    const {card} = setup(); card.render();
    for (const action of ['assign-roll', 'edit-roll', 'label', 'weigh', 'archive']) {
      const button = card.shadowRoot.innerHTML.match(new RegExp(`<button[^>]*data-action="${action}"[^>]*>[\\s\\S]*?<\\/button>`))[0];
      assert.match(button, /<svg[^>]*class="icon-fallback"[^>]*aria-hidden="true"[^>]*focusable="false"/);
      assert.match(button, /<path d="[^"]+"\s*\/>/);
      assert.doesNotMatch(button, /<svg[^>]*\shidden(?:[\s=>])/);
    }
    // :defined responds to HA registration without polling or a card rerender.
    assert.match(card.shadowRoot.innerHTML, /\.roll-action ha-icon:not\(:defined\)\{display:none\}/);
    assert.match(card.shadowRoot.innerHTML, /\.roll-action ha-icon:defined\+\.icon-fallback\{display:none\}/);
    card.data.spools[0].status = 'archived'; card.render();
    const restored = card.shadowRoot.innerHTML.match(/<button[^>]*data-action="restore"[^>]*>[\s\S]*?<\/button>/)[0];
    assert.match(restored, /<svg[^>]*class="icon-fallback"/);
  });
  await test('row and QR assignment both require an explicit slot and show roll metadata and occupants', async () => {
    for (const action of ['assign-roll', 'select-slot']) {
      const {card, overlays, calls} = setup();
      await card.handle(action, id);
      assert.equal(card.editing, true);
      assert.equal(calls.length, 0);
      const overlay = overlays.at(-1);
      assert.ok(overlay, 'slot chooser missing');
      for (const text of [id, 'Rapid PETG', 'PETG', 'Standard PETG', 'Existing PLA', other]) assert.ok(overlay.innerHTML.includes(text), text);
      assert.match(overlay.innerHTML, /Remaining: 286[.,]088 g/);
      for (const slot of ['A1', 'A2', 'A3', 'A4', 'HT1', 'EXT']) assert.ok(overlay.innerHTML.includes(`value="${slot}"`));
      assert.match(overlay.innerHTML, /<option value="" selected>Select a slot<\/option>/);
      assert.ok(overlay.querySelector('form').querySelector('[type=submit]').disabled);
    }
  });
  await test('slot selection previews old and new UUID and saves only on explicit confirmation', async () => {
    const {card, overlays, calls} = setup(); await card.handle('assign-roll', id);
    const overlay = overlays.at(-1), select = overlay.querySelector('[name=slot]');
    select.value = 'HT1'; select.onchange();
    const preview = overlay.querySelector('[data-assignment-preview]').textContent;
    for (const text of ['HT1', other, id]) assert.ok(preview.includes(text));
    assert.equal(calls.length, 0);
    assert.equal(overlay.querySelector('form').querySelector('[type=submit]').disabled, false);
    await submit(overlay, {slot: 'HT1'});
    assert.deepEqual(JSON.parse(JSON.stringify(calls[0])), {path: 'action/assign', body: {slot: 'HT1', spool_uuid: id, revision: 14}});
    assert.equal(calls.filter(c => c.path !== 'inventory').length, 1);
    assert.equal(card.editing, false);
  });
  await test('assignment opens an accessible dialog with keyboard focus on the slot chooser', async () => {
    const {card, overlays} = setup(); await card.handle('assign-roll', id);
    const overlay = overlays.at(-1);
    assert.equal(overlay.attributes.role, 'dialog');
    assert.equal(overlay.attributes['aria-modal'], 'true');
    assert.equal(overlay.attributes['aria-label'], 'Assign roll');
    assert.equal(overlay.querySelector('[name=slot]').focused, true);
  });
  await test('poll and revision conflict retain the chosen slot and original reviewed revision', async () => {
    const {card, overlays, calls} = setup(); await card.handle('assign-roll', id);
    const overlay = overlays.at(-1), select = overlay.querySelector('[name=slot]');
    select.value = 'EXT'; select.onchange();
    const previous = overlay.innerHTML;
    card.data = {...card.data, slots: card.data.slots.map(slot => ({...slot, revision: slot.revision + 1}))};
    await card.refresh();
    assert.equal(overlay.innerHTML, previous); assert.equal(select.value, 'EXT');
    card.failure = 'Assignment changed; refresh before retrying';
    await submit(overlay, {slot: 'EXT'});
    assert.equal(calls.at(-1).body.revision, 15);
    assert.equal(card.editing, true); assert.equal(select.value, 'EXT');
    assert.match(overlay.querySelector('.form-error').textContent, /Assignment changed/);
    assert.equal(overlay.querySelector('form').querySelector('[type=submit]').disabled, false);
  });
  await test('cancel and blank submission cause no writes; permission, archived and empty guards remain', async () => {
    const {card, overlays, calls} = setup(); await card.handle('assign-roll', id);
    await submit(overlays.at(-1), {slot: ''}); assert.equal(calls.length, 0);
    overlays.at(-1).querySelector('.cancel').onclick(); assert.equal(card.editing, false); assert.equal(calls.length, 0);
    for (const change of [r => {r.status = 'archived';}, r => {r.remaining_mg = 0;}]) {
      const fixture = setup(); change(fixture.card.data.spools[0]); await fixture.card.handle('assign-roll', id);
      assert.equal(fixture.card.editing, undefined); assert.equal(fixture.calls.length, 0);
    }
    const readOnly = setup(); readOnly.card.data.can_edit = false; await readOnly.card.handle('assign-roll', id);
    assert.equal(readOnly.card.editing, undefined); assert.equal(readOnly.calls.length, 0);
  });
  await test('reserved and duplicate-slot rejections remain visible and retain the assignment draft', async () => {
    for (const message of ['Slot has an unsettled job; reconcile it first', 'Spool is already assigned to another slot']) {
      const {card, overlays, calls} = setup(); card.failure = message;
      await card.handle('assign-roll', id);
      const overlay = overlays.at(-1), select = overlay.querySelector('[name=slot]');
      select.value = 'A2'; select.onchange(); await submit(overlay, {slot: 'A2'});
      assert.equal(overlay.querySelector('.form-error').textContent, message);
      assert.equal(select.value, 'A2'); assert.equal(card.editing, true);
      assert.equal(calls.length, 1); assert.equal(calls[0].body.revision, 11);
    }
  });
  await test('an already assigned roll keeps its current slot and cannot save a no-op', async () => {
    const {card, overlays, calls} = setup(); await card.handle('assign-roll', other);
    const overlay = overlays.at(-1);
    assert.equal(overlay.querySelector('[name=slot]').value, 'HT1');
    assert.equal(overlay.querySelector('form').querySelector('[type=submit]').disabled, true);
    await submit(overlay, {slot: 'HT1'}); assert.equal(calls.length, 0);
    assert.match(overlay.querySelector('.form-error').textContent, /already assigned/);
  });
  await test('slot scan opens the shared review without an implicit A1 or immediate write', async () => {
    const {card, overlays, calls} = setup(); card.view = 'slots'; card.render();
    assert.match(card.shadowRoot.innerHTML, /<option value="" selected>Select a slot<\/option>/);
    card.shadowRoot.querySelector = sel => sel === '#scan' ? {value: id} : sel === '#slot' ? {value: ''} : sel === '.editor' ? card.shadowRoot.overlay : {};
    await card.handle('scan', ''); assert.equal(calls.length, 0); assert.equal(card.editing, true);
    assert.match(overlays.at(-1).innerHTML, /<option value="" selected>Select a slot<\/option>/);
  });
  console.log('Assignment icons and explicit review UI checks pass');
})().catch(error => {console.error(error); process.exitCode = 1;});
