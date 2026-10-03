// Same component runs as a HA card and on the loopback demo server.

const newRequestKey = () => {

  if (typeof crypto.randomUUID === 'function') return crypto.randomUUID();

  const bytes = crypto.getRandomValues(new Uint8Array(16));

  bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;

  const hex = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');

  return `${hex.slice(0,8)}-${hex.slice(8,12)}-${hex.slice(12,16)}-${hex.slice(16,20)}-${hex.slice(20)}`;

};

const escapeHtml = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

class QuackMaterialDemoCard extends HTMLElement {

  constructor() {

    super(); this.attachShadow({mode: 'open'}); this.message = ''; this.busy = false;

  }

  setConfig(config) { this.config = config; }

  set hass(hass) { this._hass = hass; if (!this.data) this.refresh(); }

  getCardSize() { return 12; }

  connectedCallback() {

    if (!this.timer) this.timer = setInterval(() => { if (!this.busy && !this.editing && !this.dialogOpen) this.refresh(); }, 5000);

    this.refresh();

  }

  disconnectedCallback() { clearInterval(this.timer); this.timer = null; this.stopCamera(); }

  async api(method, path, data) {

    if (this._hass) return this._hass.callApi(method, `quack_material_demo/${path}`, data);

    const result = await fetch(`/api/quack_material_demo/${path}`, {

      method, headers: {'Content-Type': 'application/json'}, ...(data ? {body: JSON.stringify(data)} : {})});

    const body = await result.json();

    if (!result.ok) throw new Error(body.error || `HTTP ${result.status}`);

    return body;

  }

  async refresh() {

    if (this.busy) return;

    try { this.data = await this.api('GET', 'materials'); this.render(); }

    catch (_) { this.message = 'Demo API unavailable. No printer material fallback.'; this.render(); }

  }

  async action(action, data) {

    if (this.busy) return;

    this.busy = true;

    try {

      const result = await this.api('POST', `action/${action}`, data);

      if (action === 'label') this.label = result;

      if (action === 'create') this.message = `Created roll ${result.uuid}. UUID generated automatically.`;

      if (action !== 'create') this.message = 'Saved to shared demo inventory. No device command was sent.';

    } catch (error) { this.message = error.message || 'Demo action failed'; }

    finally { this.busy = false; this.editing = false; await this.refresh(); }

  }

  ask(label, initial = '') {
    if (this.dialogOpen) return Promise.resolve(null);
    this.dialogOpen = true;
    const overlay = document.createElement('div');
    overlay.style.cssText = 'position:fixed;inset:0;background:#000a;z-index:10000;display:grid;place-items:center';
    overlay.innerHTML = `<form role="dialog" aria-label="Material inventory input" style="background:#20232a;padding:24px;border-radius:12px;max-width:90vw"><label>${escapeHtml(label)}<input aria-label="${escapeHtml(label)}" value="${escapeHtml(initial)}" style="display:block;width:90%;margin:16px 0"></label><button type="submit">Save value</button><button type="button">Cancel</button></form>`;
    this.shadowRoot.append(overlay);
    return new Promise(resolve => {
      const finish = value => { overlay.remove(); this.dialogOpen = false; resolve(value); };
      overlay.querySelector('form').onsubmit = event => { event.preventDefault(); finish(overlay.querySelector('input').value); };
      overlay.querySelector('[type="button"]').onclick = () => finish(null);
      overlay.onkeydown = event => { if(event.key === 'Escape') finish(null); };
      overlay.querySelector('input').focus();
    });
  }

  stopCamera() { if (this.stream) this.stream.getTracks().forEach(track => track.stop()); this.stream = null; }

  async scan() {

    if (!window.BarcodeDetector || !navigator.mediaDevices) {

      this.message = 'Camera QR scanning unavailable here. Paste a UUID/link or select a roll.'; this.render(); return;

    }

    this.busy = true;

    try {

      this.stream = await navigator.mediaDevices.getUserMedia({video: {facingMode: 'environment'}});

      const video = document.createElement('video'); video.autoplay = true; video.playsInline = true;

      video.srcObject = this.stream; this.shadowRoot.querySelector('#camera').append(video); await video.play();

      const detector = new BarcodeDetector({formats: ['qr_code']});

      const deadline = Date.now() + 20000;

      while (this.isConnected && Date.now() < deadline && this.stream) {

        const codes = await detector.detect(video);

        if (codes.length) { this.shadowRoot.querySelector('#identity').value = codes[0].rawValue; break; }

        await new Promise(resolve => setTimeout(resolve, 200));

      }

      video.remove();

    } catch (_) { this.message = 'Camera not available. Use selection or paste the roll UUID.'; }

    finally { this.stopCamera(); this.busy = false; }

  }

  render() {

    const data = this.data;

    const draft = Array.from(this.shadowRoot.querySelectorAll('input[id],select[id]'), field => [field.id, field.value]);
    const oldSlot = this.shadowRoot.querySelector('#slot')?.value;

    const oldRoll = this.shadowRoot.querySelector('#roll')?.value;

    const g = mg => `${(mg / 1000).toFixed(1)} g`;

    this.shadowRoot.innerHTML = `<style>

      :host{display:block;color:var(--primary-text-color,#edf4ff);font:15px system-ui}*{box-sizing:border-box}

      ha-card{display:block;background:var(--ha-card-background,#1b2431);border-radius:18px;padding:22px}

      h2{margin:0 0 8px;font-size:22px}p{line-height:1.45;color:var(--secondary-text-color,#b2bfd3)}

      .banner{border-left:4px solid #ffc366;padding:10px;background:#49371e;color:#ffe4ac;border-radius:5px}

      .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px;margin:18px 0}

      .slot{border:1px solid #465870;border-radius:12px;padding:12px;min-width:0}.slot b{display:block}

      .slot small{display:block;overflow-wrap:anywhere;margin:5px 0;color:#b2bfd3}.dot{display:inline-block;width:15px;height:15px;border-radius:50%;margin-right:6px;border:1px solid #aaa}

      .row{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:12px 0}input,select,button{font:inherit;padding:10px;border-radius:8px;max-width:100%;border:1px solid #526782;background:#111b28;color:#edf4ff}

      input{min-width:0;flex:1}button{cursor:pointer;background:#173f51}button:disabled{opacity:.5;cursor:default}

      .stock{padding:12px 0;border-bottom:1px solid #3b485b;overflow-wrap:anywhere}.muted{color:#b2bfd3;font-size:13px}

      .job{padding:10px;border:1px solid #40516a;border-radius:10px;margin:8px 0;overflow-wrap:anywhere}

      #message{color:#8ee0ef;min-height:22px}img[alt^="Roll QR code"]{background:white;padding:8px}video{max-width:100%;max-height:250px}label{display:block}

    </style><ha-card><h2>Quack · Material demo</h2>

      <div class="banner">Isolated example rolls and simulated jobs. No printer commands or production stock changes.</div>

      <p>HA keeps the real roll identity. P2S receives a Bambu substitute. Quack imports the exact installed material preset.</p>

      <div id="message" role="status">${escapeHtml(this.message)}</div>

      ${!data ? '<p>Waiting for demo API…</p>' : `

      <div class="grid">${data.slots.map(slot => {

        const spool = data.spools.find(s => s.uuid === slot.spool_uuid);

        return `<div class="slot"><b>${escapeHtml(slot.id)}</b>${spool ? `<small><span class="dot" style="background:${/^#[0-9a-f]{6}$/i.test(spool.color) ? spool.color : '#888'}"></span>${escapeHtml(spool.product)}</small><small>${g(spool.available_mg)} available</small><small>P2S: ${escapeHtml(spool.bambu_material)} · simulated</small>` : '<small>Unassigned</small>'}</div>`;

      }).join('')}</div>

      <h3>New roll · automatic UUID</h3>

      <div class="row"><input id="new-product" aria-label="New roll name" value="Elegoo Rapid PETG Blue"><input id="new-manufacturer" aria-label="Manufacturer" value="ELEGOO"><select id="new-type" aria-label="Material type"><option>PETG</option><option>PLA</option></select><input id="new-color" aria-label="Roll color" type="color" value="#2255aa"></div>

      <div class="row"><input id="new-weight" aria-label="New roll grams" type="number" min="0" value="1000"><input id="new-preset" aria-label="Exact material preset" value="Elegoo Rapid PETG @BBL P2S - HA Demo"><button id="create-roll">Create roll and UUID</button></div>

      <h3>Assign a physical roll</h3><a style="color:#8ee0ef" href="/quack-material-demo/labels/index.html" target="_blank" rel="noopener">Printable demo QR labels</a>

      <div class="row"><label>Slot <select id="slot">${data.slots.map(s => `<option>${escapeHtml(s.id)}</option>`).join('')}</select></label>

      <select id="roll" aria-label="Physical roll">${data.spools.filter(s => s.status !== 'archived' && s.remaining_mg > 0).map(s => `<option value="${escapeHtml(s.uuid)}">${escapeHtml(s.product)}</option>`).join('')}</select>

      <button id="assign">Assign selected roll</button><button id="clear">Clear slot</button></div>

      <div class="row"><input id="identity" aria-label="Scanned roll UUID or link" placeholder="Paste scanned UUID / spool link"><button id="scan">Scan QR</button><button id="assign-id">Assign scanned roll</button></div><div id="camera"></div>

      <h3>Inventory and material presets</h3>

      ${data.spools.map(s => `<div class="stock"><b>${escapeHtml(s.product)}</b> · ${escapeHtml(s.manufacturer)}<br>

        ${g(s.remaining_mg)} remaining · ${g(s.reserved_mg)} reserved · ${g(s.available_mg)} available <span class="muted">(${escapeHtml(s.weight_quality)})</span>

        <div class="muted">${escapeHtml(s.uuid)}</div><div class="muted">Quack: ${escapeHtml(s.material_preset)}</div>

        <div class="row"><button data-profile="${escapeHtml(s.uuid)}">Link installed material preset</button><button data-label="${escapeHtml(s.uuid)}">Show QR label</button><button data-weigh="${escapeHtml(s.uuid)}" ${s.status === 'archived' ? 'disabled' : ''}>Set measured remaining</button><button data-archive="${escapeHtml(s.uuid)}" data-mode="${s.status === 'archived' ? 'restore' : 'archive'}">${s.status === 'archived' ? 'Restore roll' : 'Archive roll'}</button></div></div>`).join('')}

      ${this.label ? `<section class="job"><h3>Printable roll identity</h3><img alt="Roll QR code ${escapeHtml(this.label.uuid)}" width="220" height="220" src="data:image/svg+xml,${encodeURIComponent(this.label.svg)}"><div>${escapeHtml(this.label.uuid)} · ${escapeHtml(this.label.status)}</div><div>${escapeHtml(this.label.payload)}</div><a download="${escapeHtml(this.label.uuid)}.svg" href="data:image/svg+xml,${encodeURIComponent(this.label.svg)}">Save label for printing / writing UUID to NFC</a></section>` : ''}

      <h3>Shared customer orders</h3><div class="row"><input id="order-title" aria-label="Order title" placeholder="Order title"><input id="order-customer" aria-label="Customer name" placeholder="Demo customer"><button id="create-order">Create demo order</button></div>

      ${(data.orders || []).map(o => `<div class="job"><b>${escapeHtml(o.title)}</b> · ${escapeHtml(o.status)} ${o.archived ? '· archived' : ''}<div class="muted">${escapeHtml(o.id)}</div><button data-order="${escapeHtml(o.id)}">Complete order</button><button data-order-archive="${escapeHtml(o.id)}">Archive order</button></div>`).join('')}

      <h3>Independent job journal</h3><p>Keep this service running and close Quack. Start/finish a simulated print here; its history and stock survive a restart.</p>

      <div class="row"><input id="job-name" aria-label="Demo job name" placeholder="Demo print name" value="demo-cube.3mf"><input id="job-grams" aria-label="Estimated grams" type="number" min="0" step="0.1" value="20"><select id="job-order" aria-label="Customer order"><option value="">No customer order</option>${(data.orders || []).filter(o => !o.archived).map(o => `<option value="${escapeHtml(o.id)}">${escapeHtml(o.title)}</option>`).join('')}</select><button id="start">Start demo job using selected slot</button><button id="external">Log external job without quantities</button></div>

      ${data.jobs.map(j => `<div class="job"><b>${escapeHtml(j.name)}</b> · ${escapeHtml(j.state)}<div class="muted">${escapeHtml(j.uuid)} · ${escapeHtml(j.source)} · ${escapeHtml(j.settlement?.quality || 'unsettled')} · order ${escapeHtml(j.customer_order_uuid || 'none')}</div><div>${j.allocations.map(a => `${escapeHtml(a.slot)} · ${escapeHtml(a.spool_uuid)} · ${g(a.weight_mg)} planned · ${j.settlement ? g(j.settlement.consumption[a.spool_uuid] || 0) + ' used' : 'usage pending'}`).join('<br>')}</div>

        ${!j.settlement ? `<div class="row"><button data-finish="${escapeHtml(j.uuid)}">Finish using estimate</button><button data-fail="${escapeHtml(j.uuid)}">Mark failed / review</button><button data-measure="${escapeHtml(j.uuid)}">Reconcile measured use</button></div>` : ''}</div>`).join('')}

      <button id="refresh">Refresh</button>`}</ha-card>`;

    if (!data) return;

    const $ = selector => this.shadowRoot.querySelector(selector);

    if (oldSlot) $('#slot').value = oldSlot;

    if (oldRoll) $('#roll').value = oldRoll;
    for (const [id, value] of draft) {
      const field = this.shadowRoot.getElementById(id);
      if (field && (field.tagName !== 'SELECT' || Array.from(field.options).some(o => o.value === value))) field.value = value;
    }

    this.shadowRoot.onfocusin = () => { this.editing = true; };

    this.shadowRoot.onfocusout = () => { this.editing = false; };

    const assign = uuid => {

      const slot = data.slots.find(s => s.id === $('#slot').value);

      return this.action('assign', {slot: slot.id, spool_uuid: uuid, revision: slot.revision});

    };

    $('#assign').onclick = () => assign($('#roll').value);

    $('#clear').onclick = () => assign(null);

    $('#scan').onclick = () => this.scan();

    $('#assign-id').onclick = () => {

      const input = $('#identity').value.trim();

      const match = input.match(/(?:^|\/)([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})(?:$|[?#])/i);

      if (!match) { this.message = 'A roll UUID or spool link is required.'; this.render(); return; }

      assign(match[1].toLowerCase());

    };

    $('#refresh').onclick = () => this.refresh();

    $('#create-roll').onclick = () => {

      const grams = Number($('#new-weight').value);

      if (!Number.isFinite(grams) || grams < 0 || grams > 1000) { this.message='Enter 0–1000 grams.'; this.render(); return; }

      this.action('create', {product: $('#new-product').value, manufacturer: $('#new-manufacturer').value,material_type: $('#new-type').value,

        color: $('#new-color').value,remaining_mg: Math.round(grams*1000),nominal_mg:1000000,diameter_mm:1.75,density_g_cm3:$('#new-type').value==='PETG'?1.26:1.24,

        material_preset:$('#new-preset').value,request_key:newRequestKey()});

    };

    this.shadowRoot.querySelectorAll('[data-label]').forEach(b => b.onclick = () => this.action('label',{spool_uuid:b.dataset.label}));

    this.shadowRoot.querySelectorAll('[data-archive]').forEach(b => b.onclick = () => this.action(b.dataset.mode,{spool_uuid:b.dataset.archive}));

    this.shadowRoot.querySelectorAll('[data-weigh]').forEach(b => b.onclick = async () => {

      const value=await this.ask('Measured remaining grams:',String(data.spools.find(s=>s.uuid===b.dataset.weigh).remaining_mg/1000));

      if(value===null) return;

      if(!value.trim() || !Number.isFinite(Number(value)) || Number(value)<0) {this.message='Enter nonnegative grams.';this.render();return;}

      this.action('weigh',{spool_uuid:b.dataset.weigh,remaining_mg:Math.round(Number(value)*1000),request_key:newRequestKey()});

    });

    $('#create-order').onclick=()=>this.action('order',{title:$('#order-title').value,customer:$('#order-customer').value || 'Demo customer'});

    this.shadowRoot.querySelectorAll('[data-order]').forEach(b=>b.onclick=()=>this.action('order',{uuid:b.dataset.order,status:'completed'}));

    this.shadowRoot.querySelectorAll('[data-order-archive]').forEach(b=>b.onclick=()=>this.action('order',{uuid:b.dataset.orderArchive,archive:true}));



    $('#start').onclick = () => this.action('start', {name: $('#job-name').value,

      request_key: newRequestKey(), customer_order_uuid: $('#job-order').value || null, allocations: [{slot: $('#slot').value, weight_mg: Math.round(Number($('#job-grams').value) * 1000)}]});

    $('#external').onclick = () => this.action('start', {name: $('#job-name').value, request_key: newRequestKey(), allocations: []});

    this.shadowRoot.querySelectorAll('[data-profile]').forEach(button => button.onclick = async () => {

      const spool = data.spools.find(s => s.uuid === button.dataset.profile);

      const preset = await this.ask('Exact name of an installed compatible material preset in Quack:', spool.material_preset);

      if (preset) this.action('profile', {spool_uuid: spool.uuid, material_preset: preset});

    });

    this.shadowRoot.querySelectorAll('[data-finish]').forEach(button => button.onclick = () => this.action('finish', {job_uuid: button.dataset.finish, outcome: 'completed', quality: 'estimated'}));

    this.shadowRoot.querySelectorAll('[data-fail]').forEach(button => button.onclick = () => this.action('finish', {job_uuid: button.dataset.fail, outcome: 'failed', quality: 'unknown'}));

    this.shadowRoot.querySelectorAll('[data-measure]').forEach(button => button.onclick = async () => {

      const job = data.jobs.find(j => j.uuid === button.dataset.measure); const consumption = {};

      if (!job.allocations.length) { this.message = 'External job has no roll allocation; consumption remains unknown in this demo.'; this.render(); return; }

      for (const a of job.allocations) {

        const value = await this.ask(`Measured consumed grams from ${a.slot}:`, String(a.weight_mg / 1000));

        if (value === null) return;

        const grams = Number(value);

        if (!value.trim() || !Number.isFinite(grams) || grams < 0) { this.message = 'Enter nonnegative grams.'; this.render(); return; }

        consumption[a.spool_uuid] = Math.round(grams * 1000);

      }

      const outcome = job.state === 'needs_review' ? await this.ask('Job outcome: completed or failed?', 'completed') : 'completed';

      if (outcome === null) return;

      if (!['completed', 'failed'].includes(outcome)) { this.message = 'Enter completed or failed.'; this.render(); return; }

      this.action('finish', {job_uuid: job.uuid, outcome, quality: 'measured', consumption});

    });

  }

}

if (!customElements.get('quack-material-demo-card')) customElements.define('quack-material-demo-card', QuackMaterialDemoCard);

window.customCards = window.customCards || [];

window.customCards.push({type: 'quack-material-demo-card', name: 'Quack Material Demo', description: 'Isolated roll assignment and independent job journal'});
