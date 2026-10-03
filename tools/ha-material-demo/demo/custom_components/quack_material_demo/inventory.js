// Central inventory UI. Optional printer assignments use the validated coordinator.
function escapeHtml(value) { return String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function materialProfileLabel(roll) { return roll.material_preset || `Standard ${roll.material_type} (resolved during Quack sync)`; }
function decodeSpool(value) {
  let text = String(value).trim();
  if (text.startsWith('quackslicer://spool/')) text = text.slice(20);
  else if (text.startsWith('homeassistant://')) {
    const url=new URL(text);
    if (url.host!=='navigate' || url.pathname!=='/dashboard-filament/rolls') throw Error('Invalid roll navigation link');
    text=url.searchParams.get('spool') || '';
  }
  else if (/^https?:\/\//.test(text)) text = new URL(text).searchParams.get('spool') || '';
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(text)) throw Error('Invalid roll UUID or QR link');
  return text.toLowerCase();
}
function gramsToMg(value) {
  const text = String(value).trim().replace(',', '.');
  if (!/^\d+(\.\d{1,3})?$/.test(text)) throw Error('Enter nonnegative grams, with at most three decimal places');
  const amount = Math.round(Number(text)*1000);
  if (!Number.isSafeInteger(amount) || amount > 1e10) throw Error('Weight is out of range');
  return amount;
}
function newRequestId() {
  if (typeof crypto.randomUUID==='function') return crypto.randomUUID();
  const bytes=crypto.getRandomValues(new Uint8Array(16));bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
  const hex=Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
  return `${hex.slice(0,8)}-${hex.slice(8,12)}-${hex.slice(12,16)}-${hex.slice(16,20)}-${hex.slice(20)}`;
}

class QuackInventoryCard extends HTMLElement {
  constructor() { super(); this.attachShadow({mode:'open'}); this.view='rolls'; this.query=''; this.message=''; }
  setConfig(config) { this.view=config.view || 'rolls'; }
  set hass(value) { this._hass=value; window.QuackNfc?.controller?.attach(this); if (!this.data && !this.loading) this.refresh(); }
  getCardSize() { return 10; }
  connectedCallback() {
    if (this._hass || ['localhost','127.0.0.1'].includes(location.hostname)) this.refresh();
    this.timer=setInterval(()=>{if (!this.editing && !this.busy) this.refresh();},10000);
  }
  disconnectedCallback() { clearInterval(this.timer); window.QuackNfc?.controller?.detach(this); }
  async api(path, body) {
    if (this._hass) return this._hass.callApi(body ? 'POST' : 'GET', 'quack_material_demo/'+path, body);
    const r=await fetch('/api/quack_material_demo/'+path, body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {});
    const result=await r.json(); if (!r.ok) throw Error(result.error || 'Request failed'); return result;
  }
  error(e) { return e?.body?.message || e?.body?.error || e?.message || 'Request failed'; }
  async refresh() {
    if (this.loading) return; this.loading=true;
    try { this.data=await this.api('inventory'); if (!this.editing) this.render(); }
    catch(e) { this.message=this.error(e); if (!this.editing) this.render(); }
    finally { this.loading=false; }
  }
  async action(name, data) {
    if (this.busy) return; this.busy=true;
    try { const result=await this.api('action/'+name,data); this.message='Saved'; await this.refresh(); return result; }
    catch(e) { this.message=this.error(e); throw e; }
    finally { this.busy=false; }
  }
  roll(id) { return this.data.spools.find(s=>s.uuid===id); }
  grams(v) { return ((v||0)/1000).toLocaleString(undefined,{maximumFractionDigits:3}); }
  money(v) { return v==null ? '—' : (v/1e6).toLocaleString(undefined,{style:'currency',currency:'EUR'}); }
  button(label, action, id='', disabled=false) { return `<button data-action="${escapeHtml(action)}" data-id="${escapeHtml(id)}" ${disabled?'disabled':''}>${escapeHtml(label)}</button>`; }
  nfcText(en, de) { return (this._hass?.locale?.language || this._hass?.language || '').startsWith('de') ? de : en; }
  consumeNfc(scan) {
    if (window.QuackSlots?.isOpen()) return false;
    if (!this.data || this.editing || this.busy) return false;
    if (this.view!=='rolls') {this.view='rolls';this.query='';this.render();}
    try { this.assignRoll(decodeSpool(scan.spool_uuid)); }
    catch(error) { this.message=this.error(error); this.render(); }
    return true; // Display (including rejection) consumes the scan; Save remains explicit.
  }
  rollButton(label, icon, action, id, disabled=false, reason='') {
    const e=escapeHtml;
    const paths={
      'tray-arrow-down':'M4 15v5h16v-5M12 3v12m-5-5 5 5 5-5',
      pencil:'m4 15 11-11 5 5-11 11H4zm9-9 5 5M4 15l5 5',
      qrcode:'M3 3h6v6H3zm12 0h6v6h-6zM3 15h6v6H3zM15 15h3v3h3v3h-6zM12 3v3m0 6h3m6 0v3M3 12h3m6 6v3',
      nfc:'M4 4h16v16H4zM8 16V8l8 8V8M2 8v8m20-8v8',
      scale:'M12 3v17M6 20h12M4 7h16M6 7l-3 7h6zm12 0-3 7h6z',
      archive:'M4 7h16v13H4zM3 3h18v4H3zM9 11h6',
      'archive-arrow-up':'M4 14v6h16v-6M12 4v12m-5-7 5-5 5 5'
    };
    return `<button class="roll-action" data-action="${e(action)}" data-id="${e(id)}" title="${e(reason ? label + ": " + reason : label)}" aria-label="${e(label)}" ${disabled?'disabled':''}><ha-icon icon="mdi:${e(icon)}" aria-hidden="true"></ha-icon><svg class="icon-fallback" aria-hidden="true" focusable="false" viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="${paths[icon]}" /></svg></button>`;
  }
  table(head, rows) { return `<div class="scroll"><table><thead><tr>${head.map(h=>`<th>${escapeHtml(h)}</th>`).join('')}</tr></thead><tbody>${rows.join('') || '<tr><td>No records</td></tr>'}</tbody></table></div>`; }
  filtered(items) { return items.filter(v=>JSON.stringify(v).toLowerCase().includes(this.query.toLowerCase())); }
  render() {
    const priorSearch=this.shadowRoot.querySelector('.search');
    const searchSelection=priorSearch && this.shadowRoot.activeElement===priorSearch
      ? [priorSearch.selectionStart,priorSearch.selectionEnd,priorSearch.selectionDirection] : null;
    const scan=this.shadowRoot.querySelector('#scan'),slot=this.shadowRoot.querySelector('#slot');
    if (scan) this.scanText=scan.value;if (slot) this.slotChoice=slot.value;
    const d=this.data, e=escapeHtml, locked=d && !d.can_edit;
    const views={rolls:'Rolls',slots:'Scan & slots',customers:'Customers',orders:'Orders',prints:'Prints',history:'Stock history',sync:'Synchronization'};
    let content='<p>Connecting to inventory…</p>';
    if (d) {
      if (this.view==='rolls') {
        content=this.button('New roll','new-roll','',locked)+this.table(['Roll / UUID','Stock','Material preset','Actions'],this.filtered(d.spools).map(s=>{
          const color=/^#[0-9a-f]{6}$/i.test(s.color)?s.color:'#808080';
          return `<tr><td><span class="swatch" style="background:${color}"></span><b>${e(s.product)}</b><br>${e(s.manufacturer)} · ${e(s.material_type)} · ${e(s.status||'active')}<small>${e(s.uuid)}</small></td><td>${this.grams(s.remaining_mg)} g<br><small>${this.grams(s.available_mg)} g available · ${this.grams(s.reserved_mg)} g reserved<br>${e(s.weight_quality)} · ${this.money(s.material_price_per_kg_micros)}/kg</small></td><td>${e(materialProfileLabel(s))}</td><td><div class="roll-actions">${this.rollButton('Assign','tray-arrow-down','assign-roll',s.uuid,locked||s.status==='archived'||s.remaining_mg===0)}${this.rollButton('Edit','pencil','edit-roll',s.uuid,locked)}${this.rollButton('QR label','qrcode','label',s.uuid)}${this.rollButton(this.nfcText('Write NFC','NFC schreiben'),'nfc','nfc-write',s.uuid,locked||!window.QuackNfc?.canWrite(this._hass),!window.QuackNfc?.canWrite(this._hass)?this.nfcText('Requires the NFC-capable Companion app','Benötigt die NFC-fähige Companion-App'):'')}${this.rollButton('Weigh','scale','weigh',s.uuid,locked||s.status==='archived')}${this.rollButton(s.status==='archived'?'Restore':'Archive',s.status==='archived'?'archive-arrow-up':'archive',s.status==='archived'?'restore':'archive',s.uuid,locked)}</div></td></tr>`;
        }));
      } else if (this.view==='slots') {
        const linked=this.scanText??new URL(location.href).searchParams.get('spool')??'';
        const nfcLink=!!this._hass?.auth?.external && !!window.QuackNfc?.request;
        const phoneControls=`<p>${this.nfcText('Link this phone once to open scanned rolls here. Choose the matching Companion device.','Verknüpfe dieses Handy einmalig, damit gescannte Spulen hier geöffnet werden. Wähle das passende Companion-Gerät.')}</p>${this.button(this.nfcText('Link this phone','Dieses Handy verknüpfen'),'nfc-link','',locked||!nfcLink)}${this.button(this.nfcText('Unlink this phone','Verknüpfung aufheben'),'nfc-unlink','',locked||!nfcLink)}`;
        content=phoneControls+`<p>Scan a printed HA link with the iPhone Camera, or paste a UUID / QR link below. Review and save the physical slot assignment. This records the roll in HA.</p><label>Roll identity<input id="scan" value="${e(linked)}" placeholder="UUID or QR link"></label><label>Slot<select id="slot" aria-label="Slot"><option value="" ${!this.slotChoice?'selected':''}>Select a slot</option>${d.slots.map(s=>`<option ${s.id===this.slotChoice?'selected':''}>${e(s.id)}</option>`).join('')}</select></label>${this.button('Review assignment','scan','',locked)}<p class="note">Quack uses the linked special material preset, or a compatible same-type standard when absent. ${d.printer_assignment?.enabled?'Confirmation also updates the printer substitute and color.':'The printer retains its existing Bambu substitute.'}</p>`+this.table(['Slot','Physical roll','Printer substitute','Actions'],d.slots.map(s=>{
          const r=this.roll(s.spool_uuid); return `<tr><td>${e(s.id)}</td><td>${e(r?.product||'Unassigned')}<small>${e(r?materialProfileLabel(r):'')}</small></td><td>${e(r?.bambu_material||'No substitute configured')} ${e(r?.color||'')}</td><td>${this.button('Clear','clear',s.id,locked||!r)}</td></tr>`;
        }));
      } else if (this.view==='customers') {
        content=this.button('New customer','new-customer','',locked)+this.table(['Customer','Contact','Status','Actions'],this.filtered(d.customers).map(c=>`<tr><td>${e(c.name)}<small>${e(c.id)}</small></td><td>${e(c.contact_name)}<br>${e(c.email)}<br>${e(c.phone)}</td><td>${c.archived?'Archived':'Active'}</td><td>${this.button('Edit','edit-customer',c.id,locked)}${this.button(c.archived?'Restore':'Archive','archive-customer',c.id,locked)}</td></tr>`));
      } else if (this.view==='orders') {
        content=this.button('New order','new-order','',locked)+this.table(['Order','Customer','Status / quote','Actions'],this.filtered(d.orders).map(o=>`<tr><td>${e(o.order_number)} · ${e(o.title)}<small>${e(o.id)}</small></td><td>${e(d.customers.find(c=>c.id===o.customer_id)?.name||'Unknown customer')}</td><td>${e(o.status)} ${o.archived?'· Archived':''}<br>${this.money(o.quoted_price_micros)}</td><td>${this.button('Edit','edit-order',o.id,locked)}${this.button(o.archived?'Restore':'Archive','archive-order',o.id,locked)}</td></tr>`));
      } else if (this.view==='prints') {
        content='<p>Print attempts remain recorded while Quack is closed. Successful correlated prints book the sliced estimate once. Failed prints need explicitly entered consumption; an uncertain send stays reserved until its outcome is known.</p>'+this.table(['Print attempt','State / source','Consumption','Order / actions'],this.filtered(d.jobs).slice(0,100).map(j=>`<tr><td>${e(j.name)}<small>${e(j.uuid)}<br>${e(j.created_at)}</small></td><td>${e(j.state)}<br>${e(j.source)}${j.observed_outcome?'<small>Printer: '+e(j.observed_outcome)+'</small>':''}</td><td>${j.settlement?this.grams(Object.values(j.settlement.consumption).reduce((a,b)=>a+b,0))+' g · '+e(j.settlement.quality):'Unknown / pending'}</td><td>${e(d.orders.find(o=>o.id===j.customer_order_uuid)?.title||'—')}${j.can_reconcile_provider?this.button('Reconcile consumption','reconcile-provider',j.uuid,locked):j.state==='needs_review'&&j.source==='printer_observation'?this.button('Reconcile','reconcile',j.uuid,locked):''}</td></tr>`));
      } else if (this.view==='history') {
        content=this.table(['Time','Roll','Stock change','Reason / print'],this.filtered(d.stock_events).slice().reverse().slice(0,200).map(v=>`<tr><td>${e(v.created_at)}</td><td>${e(this.roll(v.spool_id)?.product||v.spool_id)}</td><td>${this.grams(v.delta_mg)} g</td><td>${e(v.event_type)} ${e(v.note)}<small>${e(v.job_id)}</small></td></tr>`));
      } else if (this.view==='sync') {
        content=`<p>Authority: Home Assistant · revision ${e(d.revision)}</p><p>${d.spools.length} rolls · ${d.customers.length} customers · ${d.orders.length} orders · ${d.jobs.length} print attempts</p><p>Last Quack upload: ${e(d.last_quack_sync||'Awaiting connection')}</p><p>Rolls using automatic standard selection: ${d.spools.filter(s=>!s.material_preset).length}</p><p>Inventory observer: ${e(d.observer_status||'Not configured')}. Printing uses Quack's paired printer connection; slot material metadata is synchronized when assigning a roll.</p><p>Import: ${e(d.import_source?.imported_at||'None')}</p><p class="note">HA remains authoritative. When unavailable, changes cannot be committed. Pending requests keep their original identity for safe retry. Full material profiles are transferred only when explicitly selected for synchronization.</p>${this.button('Export recovery bundle','export-recovery','',locked)}<p class="note">Includes the ledger, material profiles and accepted requests, without access tokens. Keep the downloaded file private. Restore to a separate database and verify it before replacing live data.</p>`;
      }
      const linked=new URL(location.href).searchParams.get('spool');
      if (linked && this.view==='rolls') {
        let r; try { r=this.roll(decodeSpool(linked)); } catch(_) {}
        content=`<div class="notice">${r?e(r.product)+' · '+e(r.status||'active')+' · '+this.grams(r.remaining_mg)+' g':'Unknown roll UUID — register or correct the label'} ${r?this.button('Select slot','select-slot',r.uuid,locked):''}</div>`+content;
      }
    }
    this.shadowRoot.innerHTML=`<style>
      :host{display:block;color:var(--primary-text-color,#edf3f8);font:15px system-ui}*{box-sizing:border-box}article{padding:20px;background:var(--card-background-color,#19232e);border-radius:14px}h1{font-size:24px;margin:0 0 12px}nav{display:flex;flex-wrap:wrap;gap:5px;margin-bottom:15px}button{cursor:pointer;background:#263c50;color:inherit;border:1px solid #52677c;border-radius:7px;padding:8px 11px;margin:3px}button:hover{background:#34516c}button:disabled{opacity:.4;cursor:default}button.selected{border-color:#56cdf2;color:#56cdf2}input,select,textarea{width:100%;background:var(--secondary-background-color,#101a24);color:inherit;border:1px solid #718296;border-radius:6px;padding:9px;font:inherit}label{display:block;margin:12px 0}small{display:block;color:var(--secondary-text-color,#a8b5c3);font-size:12px;overflow-wrap:anywhere;margin:4px 0}.scroll{overflow:auto;margin-top:12px}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:12px 8px;border-bottom:1px solid #42505e;vertical-align:top}th{font-size:12px;text-transform:uppercase;white-space:nowrap}.swatch{display:inline-block;width:15px;height:15px;border:1px solid #aaa;border-radius:50%;margin-right:8px}.note{color:var(--secondary-text-color,#a8b5c3)}.warn{color:#ffcf6c}.notice{border:1px solid #56cdf2;padding:12px;border-radius:8px;margin:10px 0}.status{min-height:20px;color:#ffcf6c}.editor{position:fixed;inset:0;background:#0009;z-index:1000;display:grid;place-items:center;padding:15px}.sheet{background:var(--card-background-color,#19232e);border:1px solid #52677c;border-radius:12px;padding:20px;max-width:650px;width:100%;max-height:90vh;overflow:auto}.sheet img{width:240px;height:240px;background:white}.search{max-width:450px}code{overflow-wrap:anywhere}@media(max-width:600px){article{padding:12px}td{min-width:160px}h1{font-size:21px}}
      .roll-action{min-width:44px;min-height:44px;display:inline-flex;align-items:center;justify-content:center;padding:9px}.roll-action ha-icon{--mdc-icon-size:24px}.roll-actions{display:flex;flex-wrap:wrap;min-width:156px}button:focus-visible{outline:3px solid var(--primary-color,#56cdf2);outline-offset:2px}
      .roll-action ha-icon:not(:defined){display:none}.roll-action ha-icon:defined+.icon-fallback{display:none}
      </style><article><h1>Filament & Orders</h1><nav>${Object.entries(views).map(([v,l])=>`<button data-view="${v}" class="${v===this.view?'selected':''}">${l}</button>`).join('')}</nav><div class="status" role="status">${e(this.message)}</div><input class="search" placeholder="Search this view" value="${e(this.query)}">${content}</article>`;
    this.shadowRoot.querySelectorAll('[data-view]').forEach(b=>b.onclick=()=>{this.view=b.dataset.view;this.query='';this.render();});
    const search=this.shadowRoot.querySelector('.search');
    search.oninput=ev=>{this.query=ev.target.value;this.render();};
    if (searchSelection) {
      search.focus({preventScroll:true});
      search.setSelectionRange(...searchSelection);
    }
    this.shadowRoot.querySelectorAll('[data-action]').forEach(b=>b.onclick=()=>this.handle(b.dataset.action,b.dataset.id));
    window.QuackNfc?.controller?.attach(this);
    void window.QuackNfc?.controller?.deliver();
  }
  editor(title, fields, submit) {
    this.editing=true; const e=escapeHtml;
    const overlay=document.createElement('div'); overlay.className='editor';
    overlay.innerHTML=`<form class="sheet"><h2>${e(title)}</h2>${fields}<p class="form-error" role="alert"></p><button type="submit">Save</button><button type="button" class="cancel">Cancel</button></form>`;
    this.shadowRoot.append(overlay);
    const close=()=>{this.editing=false;overlay.remove();this.render();};
    overlay.querySelector('.cancel').onclick=close;
    overlay.querySelector('form').onsubmit=async ev=>{ev.preventDefault(); const button=ev.target.querySelector('[type=submit]'); button.disabled=true;
      try { await submit(new FormData(ev.target)); close(); }
      catch(err) { overlay.querySelector('.form-error').textContent=this.error(err); button.disabled=false; }
    };
  }
  field(name,label,value='',type='text',required=false) { return `<label>${escapeHtml(label)}<input name="${escapeHtml(name)}" type="${type}" value="${escapeHtml(value)}" ${required?'required':''} ${type==='number'?'step="any"':''}></label>`; }
  options(items,selected) { return items.map(([id,name])=>`<option value="${escapeHtml(id)}" ${id===selected?'selected':''}>${escapeHtml(name)}</option>`).join(''); }
  assignRoll(id, selectedSlot='') {
    const d=this.data, e=escapeHtml, roll=this.roll(id);
    if (!d.can_edit) throw Error('Inventory is read-only');
    if (!roll) throw Error('Unknown roll UUID; register the roll before assigning');
    if (roll.status==='archived') throw Error('Archived roll UUID; restore it explicitly before assigning');
    if (roll.remaining_mg===0) throw Error('Roll is empty; select another roll or reconcile weight');
    if (d.printer_assignment?.enabled) {
      if (!window.QuackSlots) throw Error('Stock picker is loading; retry shortly');
      if (window.QuackSlots.isOpen()) return;
      this.editing=true;
      window.QuackSlots.open(this._hass,{slot:selectedSlot,spoolUuid:id,onSaved:()=>this.refresh(),onClosed:()=>{this.editing=false;this.refresh();}});
      return;
    }
    // Keep the reviewed occupants and revisions stable until Save or Cancel.
    // Refresh can update this.data during editing without rewriting the draft.
    const slots=d.slots.map(slot=>({...slot}));
    const occupants=new Map(d.spools.map(spool=>[spool.uuid,{...spool}]));
    const existing=slots.find(slot=>slot.spool_uuid===id);
    const initial=slots.some(slot=>slot.id===selectedSlot)?selectedSlot:(existing?.id||'');
    const describe=uuid=>uuid?`${occupants.get(uuid)?.product||'Unknown roll'} · ${uuid}`:'Unassigned';
    const fields=`<p><b>${e(roll.product)}</b> · ${e(roll.manufacturer)} · ${e(roll.material_type)}</p><small>UUID: ${e(id)}</small><p>Material preset: ${e(materialProfileLabel(roll))}</p><p>Remaining: ${this.grams(roll.remaining_mg)} g · ${this.grams(roll.available_mg)} g available · ${this.grams(roll.reserved_mg)} g reserved</p><label>Slot<select name="slot" required><option value="" ${!initial?'selected':''}>Select a slot</option>${this.options(slots.map(slot=>[slot.id,`${slot.id} · ${describe(slot.spool_uuid)}`]),initial)}</select></label><p data-assignment-preview role="status"></p><p class="note">Save confirms this logical HA assignment. Printer loading and material commands remain separate. An existing assignment to another slot must be cleared explicitly first.</p>`;
    this.editor('Assign roll',fields,f=>{
      const slot=slots.find(slot=>slot.id===f.get('slot'));
      if (!slot) throw Error('Select a slot before saving');
      if (slot.spool_uuid===id) throw Error('This roll is already assigned to the selected slot');
      return this.action('assign',{slot:slot.id,spool_uuid:id,revision:slot.revision});
    });
    const overlay=this.shadowRoot.querySelector('.editor');
    overlay.setAttribute('role','dialog');overlay.setAttribute('aria-modal','true');overlay.setAttribute('aria-label','Assign roll');
    const select=overlay.querySelector('[name=slot]'), preview=overlay.querySelector('[data-assignment-preview]');
    const save=overlay.querySelector('form').querySelector('[type=submit]');
    const update=()=>{
      const slot=slots.find(slot=>slot.id===select.value);
      preview.textContent=slot?`${slot.id}: ${describe(slot.spool_uuid)} → ${describe(id)}`:'Select a slot to preview the assignment';
      save.disabled=!slot||slot.spool_uuid===id;
    };
    select.value=initial;select.onchange=update;update();select.focus();
  }
  async handle(action,id) {
    const d=this.data, e=escapeHtml, requestKey=newRequestId(), key=()=>requestKey, revision=d.revision;
    try {
      if (action==='export-recovery') {
        if (this.busy) return;
        this.busy=true;
        try {
          const bundle=await this.api('recovery');
          const objectUrl=URL.createObjectURL(new Blob([JSON.stringify(bundle)],{type:'application/json'}));
          const link=document.createElement('a');link.href=objectUrl;
          link.download='quack-ha-recovery-'+new Date().toISOString().slice(0,10)+'.json';
          link.click();setTimeout(()=>URL.revokeObjectURL(objectUrl),1000);
          this.message='Recovery bundle downloaded';
        } finally {this.busy=false;this.render();}
        return;
      }
      if (action==='reconcile-provider') {
        const job=d.jobs.find(j=>j.uuid===id);
        if (!job?.can_reconcile_provider) throw Error('This print has no confirmed terminal outcome to reconcile');
        const rolls=[...new Set(job.allocations.map(a=>a.spool_uuid))];
        let fields=`<p>${e(job.name)} · Printer: ${e(job.observed_outcome)}</p><p>Enter the total consumed from each reserved roll. Use 0 only when no material was consumed. The original material and price records remain attached to this print.</p>`;
        for (const uuid of rolls) fields+=this.field('grams_'+uuid,(this.roll(uuid)?.product||uuid)+' consumed (g)','','text',true);
        fields+='<label>Outcome<select name="outcome"><option value="failed">Failed</option><option value="completed">Completed</option></select></label><label>Quantity quality<select name="quality"><option value="estimated">Estimated</option><option value="measured">Measured</option></select></label>';
        this.editor('Reconcile print consumption',fields,f=>this.action('reconcile_provider',{
          job_uuid:id,revision,request_key:key(),confirmed:true,outcome:f.get('outcome'),quality:f.get('quality'),
          consumption:Object.fromEntries(rolls.map(uuid=>[uuid,gramsToMg(f.get('grams_'+uuid))]))
        }));return;
      }
      if (action==='label') {
        const box=document.createElement('div'); box.className='editor';
        const show=async target=>{
          const label=await this.api('action/label',{spool_uuid:id,target});
          const app=target==='app';
          box.innerHTML=`<div class="sheet"><h2>${e(this.roll(id).product)}</h2><p>${app?'HA app':'Browser'} QR</p><img alt="Roll QR code" src="data:image/svg+xml;charset=utf-8,${encodeURIComponent(label.svg)}"><p><code>${e(id)}</code></p><p>${e(label.payload)}</p><p>${app?'Scan with iPhone Camera to open the configured HA app.':'Scan to open the browser. Its HTTPS settings must match the HA address.'} Then choose a slot. Archived and empty rolls are rejected during assignment.</p><p data-error role="alert"></p><a download="roll-${e(id)}-${target}.svg" href="data:image/svg+xml;charset=utf-8,${encodeURIComponent(label.svg)}">Download printable SVG</a><button data-switch>${app?'Browser QR':'HA app QR'}</button><button data-close>Close</button></div>`;
          box.querySelector('[data-close]').onclick=()=>{this.editing=false;box.remove();};
          const switchButton=box.querySelector('[data-switch]');
          switchButton.onclick=async()=>{
            switchButton.disabled=true;
            try { await show(app?'web':'app'); }
            catch(err) { switchButton.disabled=false;box.querySelector('[data-error]').textContent=this.error(err); }
          };
        };
        await show('app');this.editing=true;this.shadowRoot.append(box);return;
      }
      if (action==='nfc-write') {
        if (!d.can_edit) throw Error('Inventory is read-only');
        if (!this.roll(id)) throw Error('Unknown roll UUID');
        if (!window.QuackNfc) throw Error('NFC support is still loading. Reopen the Companion app.');
        await window.QuackNfc.writeTag(this._hass,this.roll(id));
        this.message=this.nfcText('NFC write dialog requested. Complete writing in the Companion app.','NFC-Schreibdialog angefordert. Schließe den Schreibvorgang in der Companion-App ab.');
        this.render();return;
      }
      if (action==='nfc-link') {
        if (!d.can_edit) throw Error('Inventory is read-only');
        if (!window.QuackNfc?.request) throw Error('Open this page in the Companion app.');
        const result=await window.QuackNfc.request('devices');
        if (!result.devices.length) throw Error(this.nfcText('No Companion device registered for this HA user.','Für diesen HA-Benutzer ist kein Companion-Gerät registriert.'));
        const fields=`<p>${e(this.nfcText('Choose the phone you are using now. This replaces any previous link for that device.','Wähle das Handy, das du gerade verwendest. Eine frühere Verknüpfung dieses Geräts wird ersetzt.'))}</p><label>${e(this.nfcText('This phone','Dieses Handy'))}<select name="device_id" required><option value="">${e(this.nfcText('Select this phone','Dieses Handy auswählen'))}</option>${this.options(result.devices.map(device=>[device.id,device.name]),null)}</select></label>`;
        this.editor(this.nfcText('Link this phone','Dieses Handy verknüpfen'),fields,async form=>{
          const device=form.get('device_id');
          if (!device || !result.devices.some(item=>item.id===device)) throw Error('Select this phone before saving');
          await window.QuackNfc.request('bind',{device_id:device,confirmed:true});
          this.message=this.nfcText('Phone linked. Scan a roll tag to select a slot.','Handy verknüpft. Scanne einen Spulen-Tag, um einen Slot auszuwählen.');
        });return;
      }
      if (action==='nfc-unlink') {
        if (!d.can_edit) throw Error('Inventory is read-only');
        if (!window.QuackNfc?.request) throw Error('Open this page in the Companion app.');
        this.editor(this.nfcText('Unlink this phone','Verknüpfung aufheben'),'<p>'+e(this.nfcText('Stop opening scanned rolls in this app session.','Gescannte Spulen werden in dieser App-Sitzung nicht mehr automatisch geöffnet.'))+'</p>',async()=>{
          await window.QuackNfc.request('unbind');
          this.message=this.nfcText('Phone unlinked.','Verknüpfung aufgehoben.');
        });return;
      }
      if (action==='assign-roll'||action==='select-slot') { this.assignRoll(id);return; }
      if (action==='scan') { this.assignRoll(decodeSpool(this.shadowRoot.querySelector('#scan').value),this.shadowRoot.querySelector('#slot').value);return; }
      if (action==='clear') {
        await this.action('assign',{slot:id,spool_uuid:null,revision:d.slots.find(s=>s.id===id).revision});this.render();return;
      }
      if (['archive','restore'].includes(action)) {
        this.editor(action==='archive'?'Archive roll':'Restore roll',`<p>${e(this.roll(id).product)} · ${e(id)}</p><p>History and UUID are retained.</p>`,()=>this.action(action,{spool_uuid:id,revision,request_key:key()}));return;
      }
      if (action==='weigh') {
        this.editor('Record measured net filament',this.field('grams','Remaining filament (g), excluding spool tare',this.roll(id).remaining_mg/1000,'text',true),f=>this.action('weigh',{spool_uuid:id,remaining_mg:gramsToMg(f.get('grams')),revision,request_key:key()}));return;
      }
      if (action==='new-roll'||action==='edit-roll') {
        const r=this.roll(id)||{material_type:'PLA',color:'#FFFFFF',nominal_mg:1000000,remaining_mg:1000000,diameter_mm:1.75,density_g_cm3:1.24,material_price_per_kg_micros:20000000};
        let fields=`<small>UUID: ${e(id||'Generated automatically when saved')}</small>`;
        for (const [n,l] of [['product','Product'],['manufacturer','Manufacturer'],['material_type','Material type'],['material_preset','Special Quack material preset (optional; empty uses standard)']]) fields+=this.field(n,l,r[n]||'','text',n!=='material_preset');
        fields+=`<datalist id="profiles">${(d.profile_catalog||[]).map(p=>`<option value="${e(p)}"></option>`).join('')}</datalist>`;
        fields+=this.field('color','sRGB colour',r.color,'color')+this.field('nominal','Nominal filament (g)',r.nominal_mg/1000);
        if (!id) fields+=this.field('remaining','Initial remaining filament (g)',r.remaining_mg/1000);
        fields+=this.field('diameter_mm','Diameter (mm)',r.diameter_mm,'number')+this.field('density_g_cm3','Density (g/cm³)',r.density_g_cm3,'number')+this.field('price','Material price (EUR/kg)',(r.material_price_per_kg_micros||0)/1e6,'number');
        this.editor(id?'Edit roll':'New roll',fields,f=>{
          const data={revision,request_key:key()}; for(const n of ['product','manufacturer','material_type','material_preset','color']) data[n]=String(f.get(n));
          data.nominal_mg=gramsToMg(f.get('nominal')); data.diameter_mm=Number(f.get('diameter_mm'));data.density_g_cm3=Number(f.get('density_g_cm3'));data.material_price_per_kg_micros=Math.round(Number(f.get('price'))*1e6);
          if (id) data.spool_uuid=id;else data.remaining_mg=gramsToMg(f.get('remaining'));
          return this.action(id?'edit':'create',data);
        });this.shadowRoot.querySelector('[name=material_preset]').setAttribute('list','profiles');return;
      }
      if (/customer|order/.test(action)) {
        const kind=action.includes('customer')?'customer':'order', rows=kind==='customer'?d.customers:d.orders,r=rows.find(v=>v.id===id)||{};
        if (action.startsWith('archive')) { this.editor(r.archived?'Restore record':'Archive record',`<p>${e(r.name||r.title)}</p>`,()=>this.action(kind,{uuid:id,archive:!r.archived,revision,request_key:key()}));return; }
        let fields='';
        if (kind==='customer') for (const [n,l] of [['name','Name'],['contact_name','Contact name'],['email','Email'],['phone','Phone'],['notes','Notes']]) fields+=this.field(n,l,r[n]||'','text',n==='name');
        else {
          fields=`<label>Customer<select name="customer_id" required>${this.options(d.customers.filter(c=>!c.archived||c.id===r.customer_id).map(c=>[c.id,c.name]),r.customer_id)}</select></label>`;
          for (const [n,l] of [['order_number','Order number'],['title','Title'],['notes','Notes']]) fields+=this.field(n,l,r[n]||'','text',n==='title');
          fields+=`<label>Status<select name="status">${this.options(['draft','active','completed','cancelled'].map(s=>[s,s]),r.status||'active')}</select></label>`;
          for (const [n,l] of [['quoted_price_micros','Quote (EUR)'],['invoice_amount_micros','Invoice amount (EUR)'],['design_hourly_rate_micros','Design rate (EUR/hour)'],['other_cost_micros','Other cost (EUR)']]) fields+=this.field(n,l,r[n]==null?'':r[n]/1e6,'number');
          fields+=this.field('design_time_seconds','Design time (minutes)',(r.design_time_seconds||0)/60,'number')+this.field('discount_basis_points','Discount (%)',(r.discount_basis_points||0)/100,'number');
          for (const n of ['material','electricity','machine_wear','maintenance','repair_reserve','design','other']) fields+=`<label><input style="width:auto" type="checkbox" name="bill_${n}" ${r['bill_'+n]!==0?'checked':''}> Bill ${e(n.replaceAll('_',' '))}</label>`;
        }
        this.editor(id?'Edit '+kind:'New '+kind,fields,f=>{
          const data={revision,request_key:key()};if(id)data.uuid=id;
          for (const [n,v] of f.entries()) data[n]=String(v);
          if(kind==='order') {
            for(const n of ['quoted_price_micros','invoice_amount_micros','design_hourly_rate_micros','other_cost_micros']) data[n]=data[n]===''?(n.includes('price')||n.includes('invoice')?null:0):Math.round(Number(data[n])*1e6);
            data.design_time_seconds=Math.round(Number(data.design_time_seconds)*60);data.discount_basis_points=Math.round(Number(data.discount_basis_points)*100);
            for(const n of ['material','electricity','machine_wear','maintenance','repair_reserve','design','other'])data['bill_'+n]=f.has('bill_'+n);
          }
          return this.action(kind,data);
        });return;
      }
      if(action==='reconcile') {
        const fields=`<label>Roll<select name="spool_uuid">${this.options(d.spools.filter(s=>s.status!=='archived').map(s=>[s.uuid,s.product+' · '+this.grams(s.remaining_mg)+' g']),null)}</select></label><label>Slot<select name="slot">${this.options(d.slots.map(s=>[s.id,s.id]),'A1')}</select></label>`+this.field('grams','Actual or explicitly estimated total consumed (g)', '', 'text',true)+`<label>Outcome<select name="outcome"><option>completed</option><option>failed</option></select></label><label>Quantity quality<select name="quality"><option>estimated</option><option>measured</option></select></label><label>Order<select name="customer_order_uuid"><option value="">Unassigned</option>${this.options(d.orders.filter(o=>!o.archived).map(o=>[o.id,o.title]),null)}</select></label>`;
        this.editor('Reconcile observed print attempt',fields,f=>this.action('reconcile_observed',{job_uuid:id,spool_uuid:f.get('spool_uuid'),slot:f.get('slot'),consumed_mg:gramsToMg(f.get('grams')),outcome:f.get('outcome'),quality:f.get('quality'),customer_order_uuid:f.get('customer_order_uuid')||null,revision,request_key:key()}));
      }
    } catch(err) {this.message=this.error(err);if (!this.editing)this.render();}
  }
}
customElements.define('quack-inventory-card',QuackInventoryCard);
window.customCards=window.customCards||[];
window.customCards.push({type:'quack-inventory-card',name:'Quack Inventory',description:'Central physical rolls, customers, orders and print attempts'});
