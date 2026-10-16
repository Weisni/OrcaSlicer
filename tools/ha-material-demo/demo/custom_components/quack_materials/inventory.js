// Central inventory UI. Optional printer assignments use the validated coordinator.
function escapeHtml(value) { return String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function rollColorName(hex) {
  if(!/^#[a-f0-9]{6}$/i.test(hex||''))return 'Unbekannt';
  const rgb=[1,3,5].map(i=>parseInt(hex.slice(i,i+2),16)/255),max=Math.max(...rgb),min=Math.min(...rgb),delta=max-min;
  if(delta<.12)return max>.85?'Weiß':max<.2?'Schwarz':'Grau';
  const [r,g,b]=rgb;const hue=(max===r?(g-b)/delta+(g<b?6:0):max===g?(b-r)/delta+2:(r-g)/delta+4)*60;
  return hue<20||hue>=345?'Rot':hue<45?(max<.65?'Braun':'Orange'):hue<70?'Gelb':hue<170?'Grün':hue<255?'Blau':hue<290?'Violett':'Pink';
}
function rollProductName(roll) {
  if(!roll)return '';
  const quote=value=>String(value||'').trim().replace(/[.*+?^${}()|[\]\\]/g,'\\$&');
  const manufacturer=quote(roll.manufacturer),material=quote(roll.material_type);
  let product=String(roll.product||'').trim();
  if(manufacturer)product=product.replace(new RegExp('^'+manufacturer+'(?=\\s|$)','i'),'').trim();
  if(material)product=product.replace(new RegExp('(^|\\s)'+material+'(?=\\s|$)','i'),' ').replace(/\s+/g,' ').trim();
  return product||String(roll.color_name||'').trim()||rollColorName(roll.color);
}
function rollDisplayName(roll) {
  return roll?[roll.manufacturer,roll.material_type,rollProductName(roll)].map(s=>String(s||'').trim()).filter(Boolean).join(' '):'';
}
function materialProfileLabel(roll) { return roll.material_preset || `Standard ${roll.material_type} (bei Quack-Synchronisierung ausgewählt)`; }
function decodeSpool(value) {
  let text = String(value).trim();
  if (text.startsWith('quackslicer://spool/')) text = text.slice(20);
  else if (text.startsWith('homeassistant://')) {
    const url=new URL(text);
    if (url.host!=='navigate' || url.pathname!=='/dashboard-filament/rolls') throw Error('Ungültiger Rollenlink');
    text=url.searchParams.get('spool') || '';
  }
  else if (/^https?:\/\//.test(text)) text = new URL(text).searchParams.get('spool') || '';
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(text)) throw Error('Ungültige Rollen-UUID oder QR-Link');
  return text.toLowerCase();
}
function gramsToMg(value) {
  const text = String(value).trim().replace(',', '.');
  if (!/^\d+(\.\d{1,3})?$/.test(text)) throw Error('Nichtnegative Grammzahl mit höchstens drei Nachkommastellen eingeben');
  const amount = Math.round(Number(text)*1000);
  if (!Number.isSafeInteger(amount) || amount > 1e10) throw Error('Gewicht liegt außerhalb des zulässigen Bereichs');
  return amount;
}
function costDecimal(value,places,maximum) {
  const text=String(value).trim().replace(',','.');
  if(!new RegExp('^\\d+(\\.\\d{1,'+places+'})?$').test(text))throw Error('Nichtnegative Dezimalzahl mit höchstens '+places+' Nachkommastellen eingeben');
  const [whole,fraction='']=text.split('.');
  const amount=Number(whole)*10**places+Number(fraction.padEnd(places,'0'));
  if(!Number.isSafeInteger(amount)||amount>maximum)throw Error('Wert liegt außerhalb des zulässigen Bereichs');
  return amount;
}
function newRequestId() {
  if (typeof crypto.randomUUID==='function') return crypto.randomUUID();
  const bytes=crypto.getRandomValues(new Uint8Array(16));bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
  const hex=Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
  return `${hex.slice(0,8)}-${hex.slice(8,12)}-${hex.slice(12,16)}-${hex.slice(16,20)}-${hex.slice(20)}`;
}

var WORKSPACE_CSS=":host{display:block;color:var(--primary-text-color,#e6edf3);font:15px var(--primary-font-family,system-ui);--workspace-border:var(--divider-color,#34404c);--workspace-panel:var(--ha-card-background,var(--card-background-color,#1d242d));--workspace-accent:var(--primary-color,#70d6f2)}\n*{box-sizing:border-box}article{padding:24px;background:var(--primary-background-color,#11161d);max-width:1600px;margin:auto}h1{font-size:28px;margin:3px 0 0}h2{font-size:22px}h3{font-size:18px;margin:0 0 12px}.workspace-header,.header-actions,.toolbar,.pagination,.roll-actions,.detail-actions{display:flex;align-items:center;gap:8px}.workspace-header,.pagination{justify-content:space-between}.header-actions{flex-wrap:wrap}button,a.roll-action,summary{cursor:pointer;color:inherit}button,a.roll-action{background:var(--workspace-panel);border:1px solid var(--workspace-border);border-radius:10px;margin:0;padding:10px;font:inherit}button:hover,a.roll-action:hover{border-color:var(--workspace-accent);color:var(--workspace-accent)}button:disabled{opacity:.4;cursor:default}.roll-action{min-width:44px;min-height:44px;display:inline-flex;align-items:center;justify-content:center;padding:9px;text-decoration:none}.roll-action ha-icon{--mdc-icon-size:22px}.roll-action ha-icon:not(:defined){display:none}.roll-action ha-icon:defined+.icon-fallback{display:none}button:focus-visible,input:focus-visible,select:focus-visible,a:focus-visible,summary:focus-visible{outline:3px solid var(--workspace-accent);outline-offset:2px}.toolbar{margin-top:10px}.search-label{flex:1;max-width:600px}input,select,textarea{width:100%;background:var(--workspace-panel);color:inherit;border:1px solid var(--workspace-border);border-radius:8px;padding:10px;font:inherit;min-height:44px}label{display:block;font-size:12px;color:var(--secondary-text-color,#b0bbc6);margin:8px 0}label>input,label>select,label>textarea{margin-top:5px;font-size:14px}.filters,.scan-tools{border:1px solid var(--workspace-border);border-radius:12px;padding:12px 16px;margin:12px 0;background:var(--workspace-panel)}summary{display:flex;align-items:center;gap:8px;min-height:30px}.filter-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:0 16px}.sorting{display:flex;gap:12px;justify-content:flex-end;flex-wrap:wrap}.sorting label{max-width:220px}.scroll{overflow:auto;border:1px solid var(--workspace-border);border-radius:16px;background:var(--workspace-panel);margin-top:8px}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:14px 16px;border-bottom:1px solid var(--workspace-border);vertical-align:middle}th{font-size:12px;color:var(--secondary-text-color,#b0bbc6)}th .sort-head{padding:0;min-height:36px;border:0;background:transparent;text-align:left;font-size:inherit}.workspace-table tr:last-child td{border-bottom:0}.workspace-table td{max-width:340px;overflow-wrap:anywhere}small,.muted,.note{color:var(--secondary-text-color,#b0bbc6)}small{display:block;font-size:12px;line-height:1.5;margin:5px 0;overflow-wrap:anywhere}.swatch{display:inline-block;width:16px;height:16px;border:1px solid var(--workspace-border);border-radius:50%;margin-right:8px;vertical-align:middle}.badge{display:inline-block;border:1px solid var(--workspace-border);border-radius:20px;padding:5px 9px;font-size:12px}.status{color:var(--warning-color,#ffcf6c);font-size:13px}.status:empty{display:none}.warn{color:var(--warning-color,#ffcf6c)}.pagination{font-size:13px;color:var(--secondary-text-color,#b0bbc6);margin:14px 0}.pagination>div{display:flex;gap:6px}.slot-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;margin:20px 0}.slot-card{background:var(--workspace-panel);border:1px solid var(--workspace-border);border-radius:18px;padding:20px}.slot-card>button{margin-right:6px}.tools{position:relative}.tools>summary{border:1px solid var(--workspace-border);border-radius:10px;padding:9px;min-height:44px;list-style:none;background:var(--workspace-panel)}.tools>div{position:absolute;right:0;top:48px;z-index:2;padding:10px;display:flex;gap:8px;border:1px solid var(--workspace-border);border-radius:12px;background:var(--workspace-panel)}.editor{position:fixed;inset:0;z-index:1000;background:#0009;display:grid;place-items:center;padding:16px}.sheet{background:var(--workspace-panel);border:1px solid var(--workspace-border);border-radius:18px;padding:24px;width:100%;max-width:680px;max-height:90vh;overflow:auto}.sheet.wide{max-width:1100px}.sheet img{width:240px;height:240px;background:white}.sheet textarea{min-height:90px}.sheet input[type=checkbox]{width:auto;min-height:0}.sheet>button{margin:12px 8px 0 0}.sheet label{margin:12px 0}.form-error,.sheet-error{color:var(--error-color,#ffb3b3)}.notice{border:1px solid var(--workspace-accent);border-radius:12px;padding:14px;margin:12px 0}code{overflow-wrap:anywhere}.sheet iframe{max-width:100%}.detail-actions{flex-wrap:wrap;margin:16px 0}\n@media(max-width:700px){article{padding:12px}h1{font-size:24px}.workspace-header{align-items:flex-start}.header-actions{gap:5px}.workspace-table table,.workspace-table tbody,.workspace-table tr,.workspace-table td{display:block}.workspace-table thead{display:none}.workspace-table tr{padding:12px 16px;border-bottom:1px solid var(--workspace-border)}.workspace-table tr:last-child{border-bottom:0}.workspace-table td{max-width:none;padding:7px 0;border:0}.workspace-table td:before{content:attr(data-label);display:block;color:var(--secondary-text-color,#b0bbc6);font-size:11px;margin-bottom:3px}.sorting{justify-content:flex-start;gap:8px}.sorting label{flex:1;min-width:90px}.filter-grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:0 10px}.toolbar .search-label{min-width:0}.sheet{padding:18px}.slot-grid{grid-template-columns:1fr}.pagination{gap:8px;align-items:flex-start}.header-actions .roll-action,.tools>summary{min-width:44px}.workspace-header small{max-width:160px}.tools>div{max-width:250px}.roll-actions{flex-wrap:wrap}}\n\n.label-status{display:inline-flex;align-items:center;gap:3px;margin-right:10px;font-size:12px}.label-status ha-icon{--mdc-icon-size:16px}.label-status.confirmed{color:var(--success-color,#83db9d)}\n\n.roll-list{display:grid;gap:8px;margin-top:12px}.roll-card{border:1px solid var(--workspace-border);border-radius:14px;background:var(--workspace-panel);overflow:hidden}.roll-summary{display:grid;grid-template-columns:58px minmax(0,1fr) 24px;gap:12px;padding:12px 16px;list-style:none;min-height:100px}.roll-summary::-webkit-details-marker{display:none}.roll-identity{min-width:0;display:grid;gap:5px}.roll-brand{display:flex;align-items:center;justify-content:space-between;gap:8px;color:var(--secondary-text-color,#b0bbc6);font-size:12px;line-height:16px}.roll-material{color:var(--primary-text-color,#e6edf3);font-size:11px;border:1px solid var(--workspace-border);border-radius:6px;padding:1px 6px;flex-shrink:0}.roll-product{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:15px;line-height:20px}.roll-quick{display:flex;align-items:center;gap:12px;font-size:12px;line-height:18px;min-width:0}.roll-remaining{font-weight:650;font-variant-numeric:tabular-nums;white-space:nowrap}.roll-slot{color:var(--workspace-accent);font-weight:650;white-space:nowrap}.roll-quick .label-status{margin:0;white-space:nowrap}.roll-chevron{font-size:24px;color:var(--secondary-text-color,#b0bbc6);transition:transform .12s}.roll-card[open] .roll-chevron{transform:rotate(180deg)}.spool-gauge{position:relative;display:block;width:56px;height:70px;align-self:center}.spool-gauge svg{width:56px;height:56px;display:block}.spool-rim{fill:none;stroke:var(--workspace-border);stroke-width:2}.spool-track{fill:none;stroke:var(--workspace-border);stroke-width:10}.spool-fill{fill:none;stroke-width:10}.spool-hub{fill:var(--workspace-panel);stroke:var(--secondary-text-color,#b0bbc6);stroke-width:2}.spool-percent{position:absolute;bottom:0;left:0;width:100%;text-align:center;color:var(--secondary-text-color,#b0bbc6);font-size:10px;line-height:12px;font-variant-numeric:tabular-nums}.roll-expanded{border-top:1px solid var(--workspace-border);padding:12px 16px}.roll-expanded dl{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px 20px;margin:0}.roll-expanded dt{color:var(--secondary-text-color,#b0bbc6);font-size:11px;margin:0 0 4px}.roll-expanded dd{margin:0;font-size:13px;overflow-wrap:anywhere}.roll-expanded .detail-actions{margin:14px 0 0}.order-status{display:inline-flex;gap:6px;align-items:center;font-size:12px;line-height:20px;white-space:nowrap;margin-right:8px}.order-status ha-icon{--mdc-icon-size:19px}.order-status-draft,.order-status-unknown{color:#a9b5c2}.order-status-active{color:#70d6f2}.order-status-completed{color:#83db9d}.order-status-cancelled{color:#ff9292}.order-status-archived{color:#c5b1f5}.sheet.wide{padding:0;display:flex;flex-direction:column;overflow:hidden;max-height:90dvh}.sheet-header{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-shrink:0;padding:14px 20px;border-bottom:1px solid var(--workspace-border);background:var(--workspace-panel)}.sheet-header h2{font-size:20px;margin:0;overflow-wrap:anywhere;min-width:0}.sheet-header [data-close]{flex-shrink:0;align-self:flex-start}.sheet-body{overflow:auto;min-height:0;overscroll-behavior:contain;padding:16px 24px 24px}.sheet-header+.sheet-body .detail-actions{margin-top:12px}\n@media(max-width:700px){.roll-summary{padding:10px 12px;gap:10px;min-height:98px;grid-template-columns:56px minmax(0,1fr) 18px}.roll-quick{gap:10px}.roll-expanded dl{gap:12px}.sheet-header{padding:12px 14px}.sheet-body{padding:12px 16px 20px}.sorting label{min-width:0;font-size:11px}.sorting select{padding:8px;font-size:12px}.workspace-header small{max-width:none}.roll-brand{overflow:hidden}.roll-list{gap:7px}}\n@media(min-width:1050px){.roll-list{grid-template-columns:repeat(2,minmax(0,1fr));align-items:start}}\n.roll-quick{flex-wrap:wrap;row-gap:3px}.roll-brand{flex-wrap:wrap;row-gap:3px}\n";

class QuackInventoryCard extends HTMLElement {
  constructor() { super(); this.attachShadow({mode:'open'}); this.view='rolls'; this.query=''; this.message=''; }

  set hass(value) { const changed=this._hass?.user?.id!==value?.user?.id; this._hass=value; if(changed)this.query=this.workspaceState().query; window.QuackNfc?.controller?.attach(this); if (!this.data && !this.loading) this.refresh(); }
  getCardSize() { return 10; }
  connectedCallback() {
    if (this._hass || ['localhost','127.0.0.1'].includes(location.hostname)) this.refresh();
    this.timer=setInterval(()=>{if (!this.editing && !this.busy) this.refresh();},10000);
  }
  disconnectedCallback() { this.colorPicker?.close(); clearInterval(this.timer); window.QuackNfc?.controller?.detach(this); }
  async api(path, body) {
    if (this._hass) return this._hass.callApi(body ? 'POST' : 'GET', 'quack_materials/'+path, body);
    const r=await fetch('/api/quack_materials/'+path, body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {});
    const result=await r.json(); if (!r.ok) throw Object.assign(Error(result.error || 'Anfrage fehlgeschlagen'),{status:r.status,body:result}); return result;
  }
  error(e) { return e?.body?.message || e?.body?.error || e?.message || 'Anfrage fehlgeschlagen'; }
  async refresh() {
    if (this.loading) return; this.loading=true;
    try {
      this.data=await this.api('inventory');
      if (window.QuackNfc?.request && this._hass?.user?.is_admin) {
        try {this.labelStatus=(await window.QuackNfc.request('labels')).labels;this.labelStatusError='';}
        catch (_) {this.labelStatusError='Etikettenstatus derzeit nicht erreichbar';}
      }
      this.online=true;
      if (['orders','costs'].includes(this.view)) {
        this.accounting=null;
        const accounting=await this.api('accounting');
        if (accounting.revision!==this.data.revision) throw Error('Bestand hat sich beim Lesen der Kosten geändert. Aktuelle Summen neu laden.');
        this.accounting=accounting;
      }
      if (!this.editing) this.render();
      if(!this.editing&&!this.busy)void this.processRestartIntent();
    }
    catch(e) { this.online=false; this.message=this.error(e); if (!this.editing) this.render(); }
    finally { this.loading=false; }
  }
  async action(name, data) {
    if (this.busy) return; this.busy=true;
    try { const result=await this.api('action/'+name,data); this.message='Gespeichert'; await this.refresh(); return result; }
    catch(e) { this.message=this.error(e); throw e; }
    finally { this.busy=false; }
  }
  preheatPhase(phase) {
    return ({heating:'Wird vorgeheizt',selecting_air:'Umluft wird eingestellt',conditioning_bed:'Druckbett wird vorbereitet',
      ready:'Bereit zur erneuten Druckprüfung',interrupted:'Unterbrochen',cancelled:'Abgebrochen',expired:'Abgelaufen',
      failed:'Fehlgeschlagen',manual_takeover:'Manuell übernommen',handoff:'An Druck übergeben',needs_review:'Prüfung nötig'})[phase]||'Unbekannt';
  }
  preheatSummary(id) {
    const state=this.data?.job_preheats?.find(row=>row.job_uuid===id);
    return state?`<small>Vorheizung: ${escapeHtml(this.preheatPhase(state.phase))} ${this.button('Vorheizung öffnen','preheat',id)}</small>`:'';
  }
  async showPreheat(id) {
    const state=await this.api('preheat/'+encodeURIComponent(id)),e=escapeHtml;
    const telemetry=state.telemetry||{},parameters=state.parameters||{};
    const terminal=['cancelled','expired','failed','manual_takeover','handoff','needs_review'].includes(state.phase);
    const writable=this.data?.can_edit&&this.online!==false;
    const number=value=>Number.isFinite(value)?e(value)+' °C':'Unbekannt';
    const reason=({stale_status:'Frische Druckermeldung fehlt.',stale_bed_temp:'Frische Betttemperatur fehlt.',
      door_open_or_unknown:'Tür offen oder Zustand unbekannt.',idle_protection_unavailable:'Leerlaufschutz nicht bestätigt.',
      maintenance_mode:'Wartungsmodus aktiv oder unbekannt.',printer_busy_or_unknown:'Drucker beschäftigt oder Zustand unbekannt.',
      warming_bed:'Druckbett wird aufgeheizt.',stabilizing:'Temperaturverlauf wird geprüft.',stable:'Temperaturverlauf ist stabil.',
      user_cancelled:'Vorheizung wurde abgebrochen.',not_required:'Vorheizung ist für diesen Druck deaktiviert.',
      manual_override:'Kammerwartezeit wurde bewusst übersprungen.',deadline:'Zeitlimit erreicht.',
      heater_command_failed:'Heizbefehl konnte nicht bestätigt werden.'})[state.reason]||'Aktuellen Druckerzustand prüfen.';
    let body=`<p><b>${e(this.data.jobs?.find(job=>job.uuid===id)?.name||'Druck')}</b></p><p role="status">${e(this.preheatPhase(state.phase))}</p><p>${e(reason)}</p>`;
    body+=`<p>Kammer: ${number(telemetry.chamber_temp)} · Ziel: ${number(parameters.chamber_target)}<br>Druckbett: ${number(telemetry.bed_temp)} · Gemeldeter Sollwert: ${number(telemetry.bed_target)}<br>Druckbett beim Druck: ${number(parameters.print_bed_target)}</p>`;
    if(Number.isFinite(state.expires_at))body+=`<p>Zeitlimit: ${e(new Date(state.expires_at*1000).toLocaleString('de-DE'))}</p>`;
    body+='<p class="note">Die angezeigten Messwerte stammen aus der letzten Druckermeldung. Vor dem Start prüft QuackSlicer die Freigabe erneut. Diese Ansicht startet keinen Druck.</p>';
    body+=this.button('Erneut prüfen','preheat-refresh',id);
    if(writable&&!terminal){
      body+=this.button('Vorheizung abbrechen','preheat-cancel',id);
      if(state.phase==='interrupted')body+=this.button('Vorheizung erneut versuchen','preheat-retry',id);
      if(parameters.enabled&&state.reason!=='manual_override')body+=this.button('Kammerwartezeit überspringen','preheat-skip',id);
      body+=this.button('Einmal um 5 Minuten verlängern','preheat-extend',id);
    }
    const box=this.sheet('Vorheizung',body);
    box.querySelectorAll('[data-action]').forEach(button=>button.onclick=()=>this.handle(button.dataset.action,button.dataset.id));
  }
  async editPreheat(id,command) {
    if(!this.data?.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
    if(!['cancel','retry','skip','extend'].includes(command))throw Error('Ungültige Vorheizaktion');
    const state=await this.api('preheat/'+encodeURIComponent(id));
    const descriptions={cancel:['Vorheizung abbrechen','Vorheizung dieses Druckversuchs abbrechen. Weitere Heizauffrischungen enden; der aktivierte Leerlaufschutz des Druckers übernimmt die Abkühlung.'],
      retry:['Vorheizung erneut versuchen','Den unterbrochenen Vorheizversuch mit frischen Druckerdaten erneut prüfen.'],
      skip:['Kammerwartezeit überspringen','Kammerwartezeit bewusst überspringen. Das Druckbett wird vor der Druckfreigabe auf die vorgesehene Drucktemperatur zurückgeführt.'],
      extend:['Vorheizung verlängern','Das Zeitlimit einmal um 5 Minuten verlängern. Ein bereits abgelaufener Versuch wird dadurch nicht reaktiviert.']};
    const [title,description]=descriptions[command];
    const data={request_key:newRequestId(),instance_id:state.instance_id,version:state.version};
    if(command==='extend')data.seconds=300;
    this.editor(title,`<p>${escapeHtml(description)}</p>`,async()=>{
      const result=await this.api('preheat/'+encodeURIComponent(id)+'/'+command,data);
      this.message='Vorheizung aktualisiert';await this.refresh();return result;
    },{submitLabel:'Bestätigen',onSaved:()=>{void this.showPreheat(id).catch(error=>{this.message=this.error(error);this.render();});}});
  }
  roll(id) { return this.data.spools.find(s=>s.uuid===id); }
  grams(v) { return ((v||0)/1000).toLocaleString(undefined,{maximumFractionDigits:3}); }
  money(v, currency='EUR') { return v==null ? '—' : (v/1e6).toLocaleString(undefined,{style:'currency',currency}); }
  unitCost(v,currency='EUR') { return (v/1e6).toLocaleString(undefined,{style:'currency',currency,minimumFractionDigits:2,maximumFractionDigits:6}); }
  costAmount(row,field) { return row.summary?escapeHtml(this.money(row.summary[field],row.summary.currency)):`<span class="warn">${escapeHtml(row.error||'Kosten nicht verfügbar')}</span>`; }
  costTable(summary) {
    const labels={material_cost_micros:'Material',electricity_cost_micros:'Strom',machine_wear_cost_micros:'Maschinenverschleiß',maintenance_cost_micros:'Wartung',repair_reserve_cost_micros:'Reparaturrücklage',design_cost_micros:'Konstruktion',other_cost_micros:'Sonstige Kosten',total_cost_micros:'Interne Kosten',billable_subtotal_micros:'Berechenbare Zwischensumme',discount_micros:'Rabatt',calculated_invoice_micros:'Berechnet (netto)',quoted_price_micros:'Angebot',invoice_amount_micros:'Erfasster Rechnungsbetrag'};
    return this.table(['Kostenart','Betrag'],Object.entries(labels).map(([key,label])=>`<tr><td>${label}</td><td>${escapeHtml(this.money(summary[key],summary.currency))}</td></tr>`));
  }
  invoiceLines(lines, currency) {
    const e=escapeHtml;
    return this.table(['Beschreibung','Interne Kosten','Rechnung'],lines.map(line=>{
      const swatch=/^#[0-9a-f]{6}$/i.test(line.color_hex)?`<span class="swatch" style="background:${line.color_hex}"></span>`:'';
      return `<tr><td>${swatch}${e(this.invoiceLineCopy(line,currency).description)}<small>${e(this.invoiceLineCopy(line,currency).detail)}${line.included?'':' · Nicht berechnet'}</small></td><td>${e(this.money(line.internal_amount_micros,currency))}</td><td>${e(this.money(line.invoice_amount_micros,currency))}</td></tr>`;
    }));
  }
  invoiceDocument(invoice) {
    const e=escapeHtml,d=invoice.details,t=invoice.totals,m=value=>e(this.money(value,invoice.currency));
    return `<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Rechnung ${e(d.invoice_number)}</title><style>
      body{font:14px system-ui;color:#132f3e;background:white;max-width:960px;margin:32px auto;padding:24px}h1{border-bottom:4px solid #23798c;padding-bottom:12px}h2{font-size:18px}header,.parties{display:flex;justify-content:space-between;gap:32px}.parties>section{flex:1}.address{white-space:pre-wrap}table{width:100%;border-collapse:collapse;margin:24px 0}td,th{text-align:left;padding:10px;border-bottom:1px solid #cdd9dd;vertical-align:top}th{background:#eff4f6}td:not(:first-child),th:not(:first-child){text-align:right;white-space:nowrap}small{display:block;color:#526774;margin-top:5px}.swatch{display:inline-block;width:12px;height:12px;border:1px solid #777;border-radius:50%;margin-right:6px}.totals{margin-left:auto;max-width:440px}.totals p{display:flex;justify-content:space-between;gap:20px}.gross{border-top:2px solid #23798c;padding-top:14px;font-size:20px}footer{border-top:1px solid #cdd9dd;margin-top:30px;padding-top:14px;font-size:11px;overflow-wrap:anywhere}@media print{body{margin:0;padding:0;max-width:none}tr{break-inside:avoid}thead{display:table-header-group}.totals,.parties{break-inside:avoid}@page{size:A4;margin:16mm}}
      </style></head><body><header><h1>Rechnung</h1><p><b>${e(d.invoice_number)}</b><br>Rechnungsdatum: ${e(d.invoice_date)}<br>Leistungsdatum: ${e(d.service_date)}<br>Fällig am: ${e(d.due_date)}</p></header><div class="parties"><section><h2>Aussteller</h2><b>${e(d.seller_name)}</b><p class="address">${e(d.seller_address)}</p><p>${e(d.seller_contact)}<br>${e(d.tax_identifier)}</p></section><section><h2>Empfänger</h2><b>${e(d.customer_name)}</b><p class="address">${e(d.customer_address)}</p></section></div><p>Auftrag: ${e(invoice.order_title||invoice.order_uuid)}</p>${this.invoiceLines(invoice.lines,invoice.currency)}<div class="totals"><p>Interne Kosten <span>${m(t.internal_cost_micros)}</span></p><p>Nettobetrag <b>${m(t.net_micros)}</b></p><p>Umsatzsteuer ${d.small_business?'':e(d.vat_basis_points/100)+'%'} <span>${m(t.tax_micros)}</span></p><p class="gross">Gesamtbetrag <b>${m(t.gross_micros)}</b></p></div><p>${d.small_business?'Bei der gewählten Kleinunternehmerregelung wird keine Umsatzsteuer ausgewiesen.':''}</p><footer>Gespeicherte Rechnung ${e(invoice.id)} · ${e(invoice.created_at)}<br>Erstellt aus den erfassten Drucken und Kosteneinstellungen in Home Assistant. Spätere Auftragskorrekturen verändern dieses gespeicherte Dokument nicht.</footer></body></html>`;
  }
  invoiceText(invoice) {
    const d=invoice.details,t=invoice.totals,m=value=>this.money(value,invoice.currency);
    return [`Rechnung ${d.invoice_number}`,d.seller_name,d.seller_address,d.seller_contact,d.tax_identifier,'',`Empfänger: ${d.customer_name}`,d.customer_address,`Rechnungsdatum: ${d.invoice_date}`,`Leistungsdatum: ${d.service_date}`,`Fällig am: ${d.due_date}`,`Auftrag: ${invoice.order_title||invoice.order_uuid}`,'',...invoice.lines.map(line=>`${this.invoiceLineCopy(line,invoice.currency).description}\t${this.invoiceLineCopy(line,invoice.currency).detail}${line.included?'':' (Nicht berechnet)'}\t${m(line.internal_amount_micros)}\t${m(line.invoice_amount_micros)}`),'',`Interne Kosten: ${m(t.internal_cost_micros)}`,`Netto: ${m(t.net_micros)}`,`Umsatzsteuer: ${m(t.tax_micros)}`,`Gesamtbetrag: ${m(t.gross_micros)}`,d.small_business?'Keine Umsatzsteuer (gewählte Kleinunternehmerregelung).':'',`Gespeicherte Rechnung: ${invoice.id} · ${invoice.created_at}`].join('\n');
  }
  downloadFile(name, content, mime) {
    // All exports are generated locally from the selected accounting view.
    const url=URL.createObjectURL(new Blob([content],{type:mime}));
    const link=document.createElement('a');link.href=url;link.download=name;link.click();
    setTimeout(()=>URL.revokeObjectURL(url),30000);
  }
  costStatementTable(result) {
    const e=escapeHtml,currency=result.summary.currency;
    return this.table(['Position','Betrag'],(result.invoice_lines||[]).map(line=>{
      const copy=this.invoiceLineCopy(line,currency);
      return `<tr><td>${e(copy.description)}<small>${e(copy.detail)}${line.included?'':' · Nicht berechnet'}</small></td><td>${e(this.money(line.invoice_amount_micros,currency))}</td></tr>`;
    }))+`<p class="cost-total"><b>Endsumme: ${e(this.money(result.summary.calculated_invoice_micros,currency))}</b></p>`;
  }
  costStatementDocument(result) {
    const e=escapeHtml;
    return `<!doctype html><html lang="de"><meta charset="utf-8"><title>Kostenaufstellung</title><style>body{font:16px system-ui;max-width:800px;margin:32px auto;padding:16px;color:#162f38}table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:10px;border-bottom:1px solid #ddd}td:last-child{text-align:right}small{display:block;color:#666}.cost-total{text-align:right;font-size:22px}@media print{body{margin:0}tr{break-inside:avoid}}</style><h1>Kostenaufstellung</h1><p><b>${e(result.order.title)}</b> · ${e(result.customer?.name||'')}</p>${this.costStatementTable(result)}${result.payment?`<p>Bezahlt: ${e(this.money(result.payment.amount_micros,result.payment.currency))} · ${e(result.payment.paid_at)}</p>`:''}<p>Berechnet nach den aktuellen Mengen, Kostensätzen, Berechnungsschaltern und dem Auftragsrabatt. Geschätzter Verbrauch bleibt geschätzt.</p></html>`;
  }
  showCostStatement(result) {
    const box=this.sheet('Kostenaufstellung',`<p>${escapeHtml(result.order.title)}</p>${this.button('Drucken / Als PDF speichern','print-document')}${this.button('HTML herunterladen','download-html')}<p class="form-error" role="alert"></p><iframe title="Kostenaufstellung" sandbox="allow-same-origin allow-modals" style="width:100%;height:60vh;background:white"></iframe>`);
    const frame=box.querySelector('iframe');frame.srcdoc=this.costStatementDocument(result);
    box.querySelector('[data-action=print-document]').onclick=()=>{try{frame.contentWindow.focus();frame.contentWindow.print();}catch(error){box.querySelector('.form-error').textContent='Drucken ist hier nicht verfügbar. HTML herunterladen und im Browser drucken.';}};
    box.querySelector('[data-action=download-html]').onclick=()=>this.downloadFile('Kostenaufstellung.html',this.costStatementDocument(result),'text/html;charset=utf-8');
  }
  sheet(title,body) {
    this.colorPicker?.close();
    this.editing=true;
    const previous=this.shadowRoot.querySelector('.editor');if(previous)previous.remove();
    const box=document.createElement('div');box.className='editor';
    box.setAttribute('role','dialog');box.setAttribute('aria-modal','true');box.setAttribute('aria-label',title);
    box.innerHTML=`<div class="sheet wide"><header class="sheet-header"><h2>${escapeHtml(title)}</h2><button data-close class="roll-action" title="Schließen" aria-label="Schließen"><ha-icon icon="mdi:close" aria-hidden="true"></ha-icon></button></header><div class="sheet-body">${body}<p class="sheet-error" role="alert"></p></div></div>`;
    box.querySelector('[data-close]').onclick=()=>{this.editing=false;box.remove();this.render();void this.refresh();};
    this.shadowRoot.append(box);box.querySelector('[data-close]').focus({preventScroll:true});return box;
  }
  showSavedInvoice(invoice) {
    const name='invoice-'+String(invoice.details.invoice_number).replace(/[^a-z0-9_-]/gi,'_');
    const box=this.sheet('Gespeicherte Rechnung '+invoice.details.invoice_number,`<p>Dies ist die gespeicherte Rechnung in HA. Drucken öffnet den Druck- bzw. PDF-Dialog des Browsers.</p><div>${this.button('Drucken / Als PDF speichern','print-document')}${this.button('HTML herunterladen','download-html')}${this.button('Text herunterladen','download-text')}</div><p class="form-error" role="alert"></p><iframe title="Rechnungsvorschau" sandbox="allow-same-origin allow-modals" style="width:100%;height:60vh;background:white;border:1px solid #718296"></iframe>`);
    const frame=box.querySelector('iframe');frame.srcdoc=this.invoiceDocument(invoice);
    box.querySelector('[data-action=print-document]').onclick=()=>{try{frame.contentWindow.focus();frame.contentWindow.print();}catch(error){box.querySelector('.form-error').textContent='Drucken ist hier nicht verfügbar. HTML herunterladen und im Browser drucken.';}};
    box.querySelector('[data-action=download-html]').onclick=()=>this.downloadFile(name+'.html',this.invoiceDocument(invoice),'text/html;charset=utf-8');
    box.querySelector('[data-action=download-text]').onclick=()=>this.downloadFile(name+'.txt',this.invoiceText(invoice),'text/plain;charset=utf-8');
  }
  async showAccounting(scope,id) {
    const result=await this.api('accounting?'+scope+'_uuid='+encodeURIComponent(id)),e=escapeHtml;
    const locked=!this.data.can_edit||this.online===false;
    let body='';
    if(scope==='order'){
      const o=result.order||this.data.orders.find(o=>o.id===id),customer=result.customer||this.data.customers.find(c=>c.id===o?.customer_id);
      body+=`<p><b>${e(o?.title||id)}</b> · ${e(customer?.name||'—')} · ${this.orderStatus(o)}</p><p>${e(o?.notes)}</p><div class="detail-actions">${this.button('Auftrag bearbeiten','edit-order',id,locked||o?.archived)}${this.button(o?.archived?'Auftrag wiederherstellen':'Auftrag archivieren','archive-order',id,locked)}${this.viewLink('prints','Drucke dieses Auftrags','printer-3d',{order:id})}${customer?this.button('Kundendetails','customer-details',customer.id):''}</div>`;
    }
    if(scope==='order') {
      this.costStatementView=result;
      this.paymentRequestKey=newRequestId();
      body+=`<h3>Kostenaufstellung</h3>${this.costStatementTable(result)}${this.button('Kostenaufstellung drucken / speichern','cost-statement',id)}${this.button('Als bezahlt markieren und Auftrag archivieren','mark-paid',id,locked||!!result.order?.archived||!!result.payment)}`;
      if(result.payment)body+=`<p class="order-status order-status-completed"><ha-icon icon="mdi:cash-check" aria-hidden="true"></ha-icon>Bezahlt: ${e(this.money(result.payment.amount_micros,result.payment.currency))} · ${e(result.payment.paid_at)}</p>`;
    } else body+=this.costTable(result.summary);
    body+='<p class="note">Kosten verwenden die erfassten Mengen und Laufzeiten. Eine bewusste Rollenpreiskorrektur aktualisiert aktuelle Kosten in beiden Anwendungen; gespeicherte Rechnungen behalten ihre Werte. Geschätzter Verbrauch bleibt geschätzt. Offene Drucke können vorläufige Kosten enthalten. Erfasste Rechnungsbeträge und berechnete Nettobeträge werden getrennt angezeigt.</p>';
    if(scope==='order') {
      const costs=result.extra_costs||{items:[],legacy_cost_micros:result.order?.other_cost_micros||0};
      body+=`<h3>Sonderkostenpositionen</h3><p>Bezeichnung, Anzahl und Einzelwert erfassen. Die Summe zählt zu „Sonstige Kosten“; die Berechnung auf der Rechnung folgt den Auftragseinstellungen.</p>${this.button('Sonderkostenposition hinzufügen','new-extra-cost',id,locked||!!result.order?.archived)}`;
      if(costs.legacy_cost_micros)body+=`<p>Bisherige Pauschale: ${e(this.money(costs.legacy_cost_micros,result.summary.currency))}</p>`;
      body+=this.table(['Bezeichnung','Anzahl','Einzelwert','Gesamt','Aktionen'],costs.items.map(item=>`<tr><td>${e(item.description)}</td><td>${e((item.quantity_milli/1000).toLocaleString('de-DE',{maximumFractionDigits:3}))}</td><td>${e(this.unitCost(item.unit_price_micros,result.summary.currency))}</td><td>${e(this.money(item.total_micros,result.summary.currency))}</td><td>${this.button('Sonderkostenposition bearbeiten','edit-extra-cost',id+':'+item.id,locked||!!result.order?.archived)}${this.button('Sonderkostenposition entfernen','remove-extra-cost',id+':'+item.id,locked||!!result.order?.archived)}</td></tr>`));
      body+=`<h3>Zugeordnete Drucke</h3>`+this.table(['Druck','Status / Verbrauchsqualität','Interne Kosten'],(result.jobs||[]).map(row=>`<tr><td>${e(row.job?.job_name||row.name||row.uuid)}<small>${e(row.uuid)}</small></td><td>${e(this.label(row.job?.state||row.state))}<small>${e(this.label(row.consumption_quality))}</small></td><td>${this.costAmount(row,'total_cost_micros')}</td></tr>`));
      if(result.invoices?.length)body+=`<details><summary>Bisherige gespeicherte Rechnungen</summary>`+this.table(['Rechnung','Erstellt','Aktionen'],result.invoices.map(item=>`<tr><td>${e(item.details?.invoice_number||item.invoice_number)}</td><td>${e(item.created_at)}</td><td>${this.button('Öffnen','open-invoice',item.id)}</td></tr>`))+'</details>';
    }
    const record=result.order||result.job||result.customer||result;
    const box=this.sheet('Kosten · '+(record.title||record.job_name||record.name||id),body);
    box.querySelectorAll('[data-action]').forEach(button=>button.onclick=()=>this.handle(button.dataset.action,button.dataset.id));
  }
  nfcText(en, de) { return de; }
  consumeNfc(scan) {
    if (window.QuackSlots?.isOpen()) return false;
    if (!this.data || this.editing || this.busy) return false;
    // Open the shared assignment dialog in the current native HA route.
    try { this.assignRoll(decodeSpool(scan.spool_uuid), '', scan); }
    catch(error) { this.message=this.error(error); this.render(); }
    return true; // Display (including rejection) consumes the scan; Save remains explicit.
  }
  chooseNfcRecipient(scan) {
    const fields=`<p>Für diese Zuweisung ein Gerät auswählen oder ohne Geräteauswahl fortfahren.</p><label>Gerät<select name="device_id"><option value="">Undefiniert – ohne festes Gerät</option>${this.options((scan.devices||[]).map(d=>[d.id,d.name]),'')}</select></label><label><input type="checkbox" name="remember"> Als Standard für mein Konto verwenden</label><p class="note">Ein bislang nicht verknüpftes Gerät wird dieser geöffneten Oberfläche zugeordnet. Wähle dafür das Gerät, auf dem du diesen Dialog geöffnet hast.</p><p class="note">Ohne Auswahl kann der Dialog auf mehreren Geräten deines Kontos erscheinen.</p>`;
    const choose=async(device,remember)=>{
      await window.QuackNfc.request('route_select',{request_id:scan.id,device_id:device||null,remember});
      window.QuackNfc.controller?.reset();
    };
    const overlay=this.editor('Gerät für die Zuweisung',fields,form=>choose(form.get('device_id'),form.has('remember')),{submitLabel:'Auswahl übernehmen'});
    overlay.querySelector('.cancel').onclick=async()=>{
      try {await choose(null,false);}
      catch(error){this.message=this.error(error);}
      finally{this.editing=false;overlay.remove();this.render();}
    };
  }
  labelBadges(id) {
    const status=this.labelStatus?.[id]||{};
    return ['nfc'].map(kind=>{
      const marked=status[kind]?.confirmed===true;
      const title=marked?(kind==='nfc'?'NFC-Tag beschrieben':'QR-Code gedruckt'):(kind==='nfc'?'NFC-Schreiben noch nicht bestätigt':'QR-Druck noch nicht bestätigt');
      return `<span class="label-status ${marked?'confirmed':'muted'}" title="${title}" aria-label="${title}"><ha-icon icon="mdi:${kind==='nfc'?'nfc':'qrcode'}" aria-hidden="true"></ha-icon> ${marked?'✓':'—'}</span>`;
    }).join(' ');
  }
  async markLabel(id,kind,confirmed=true,source='user_confirmation') {
    const result=await window.QuackNfc.request('label_mark',{spool_uuid:id,kind,confirmed,source});
    this.labelStatus=result.labels;
  }
  processQrIntent() {
    if (!this.data||this.editing||this.busy||this.view!=='rolls'||window.QuackSlots?.isOpen?.()) return;
    const url=new URL(location.href),value=url.searchParams.get('spool');
    if(!value){this.qrIntent=null;return;}
    if(url.searchParams.has('nfc_request')||this.qrIntent===value)return;
    this.qrIntent=value;
    if(window.history?.replaceState){url.searchParams.delete('spool');window.history.replaceState(window.history.state,'',url.pathname+url.search+url.hash);}
    try {this.assignRoll(decodeSpool(value));}catch(error){this.message=this.error(error);this.render();}
  }
  rollButton(label, icon, action, id, disabled=false, reason='') {
    const e=escapeHtml;
    const paths={
      plus:"M12 4v16M4 12h16",
      "information-outline":"M12 8v1m0 3v5M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20",
      'tray-arrow-down':'M4 15v5h16v-5M12 3v12m-5-5 5 5 5-5',
      pencil:'m4 15 11-11 5 5-11 11H4zm9-9 5 5M4 15l5 5',
      qrcode:'M3 3h6v6H3zm12 0h6v6h-6zM3 15h6v6H3zM15 15h3v3h3v3h-6zM12 3v3m0 6h3m6 0v3M3 12h3m6 6v3',
      nfc:'M4 4h16v16H4zM8 16V8l8 8V8M2 8v8m20-8v8',
      scale:'M12 3v17M6 20h12M4 7h16M6 7l-3 7h6zm12 0-3 7h6z',
      archive:'M4 7h16v13H4zM3 3h18v4H3zM9 11h6',
      'archive-arrow-up':'M4 14v6h16v-6M12 4v12m-5-7 5-5 5 5'
    };
    return `<button class="roll-action" data-action="${e(action)}" data-id="${e(id)}" title="${e(reason ? label + ": " + reason : label)}" aria-label="${e(label)}" ${disabled?'disabled':''}><ha-icon icon="mdi:${e(icon)}" aria-hidden="true"></ha-icon><svg class="icon-fallback" aria-hidden="true" focusable="false" viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="${paths[icon]||paths['information-outline']}" /></svg></button>`;
  }
  table(head, rows) { return `<div class="scroll"><table><thead><tr>${head.map(h=>`<th>${escapeHtml(h)}</th>`).join('')}</tr></thead><tbody>${rows.join('') || '<tr><td>Keine Einträge</td></tr>'}</tbody></table></div>`; }
  filtered(items) { return items.filter(v=>JSON.stringify(v).toLowerCase().includes(this.query.toLowerCase())); }
  workspaceState(view=this.view) {
    const scope=this._hass?.user?.id||'local';
    window.QuackWorkspaceState ||= {};
    let persisted={};try{if(typeof sessionStorage!=='undefined')persisted=JSON.parse(sessionStorage.getItem('quack-workspace-'+scope)||'{}');}catch(_){}
    const states=window.QuackWorkspaceState[scope] ||= persisted;
    return states[view] ||= {query:'',filters:{archive:'active'},sort:({prints:'date',history:'date'})[view]||'name',direction:['prints','history'].includes(view)?'desc':'asc',page:1,size:25};
  }
  rememberWorkspace() { this.workspaceState().query=this.query||'';try{if(typeof sessionStorage!=='undefined')sessionStorage.setItem('quack-workspace-'+(this._hass?.user?.id||'local'),JSON.stringify(window.QuackWorkspaceState[this._hass?.user?.id||'local']));}catch(_){} }
  setConfig(config) {
    if(this.configured)this.rememberWorkspace(); this.view=config.view||'rolls'; this.query=this.workspaceState().query;this.configured=true;this.appliedRoute=false;
    if(this.data&&!this.editing){this.render();if(['orders','costs'].includes(this.view))void this.refresh();}
  }
  label(value) {
    return ({active:'Aktiv',draft:'Entwurf',completed:'Abgeschlossen',cancelled:'Storniert',failed:'Fehlgeschlagen',discarded:'Verworfen',quack_native:'QuackSlicer-Import',printing:'Druckt',reserved:'Reserviert',needs_review:'Prüfung nötig',archived:'Archiviert',estimated:'Geschätzt',measured:'Gewogen',unknown:'Unbekannt',quack_provider:'QuackSlicer',printer_observation:'Druckerbeobachtung',unassigned:'Ohne Auftrag',material:'Material',electricity:'Strom',machine_wear:'Maschinenverschleiß',maintenance:'Wartung',repair_reserve:'Reparaturrücklage',design:'Konstruktion',other:'Sonstiges'})[value]||value||'—';
  }
  invoiceLineCopy(line,currency='EUR') {
    if(line.cost_item_uuid)return {description:line.description,detail:`${(line.quantity_milli/1000).toLocaleString('de-DE',{maximumFractionDigits:3})} × ${this.unitCost(line.unit_price_micros,currency)}`};
    const detail={'Calculated from print runtime':'Aus der Drucklaufzeit berechnet','Runtime-based allowance':'Pauschale nach Laufzeit','Order-specific costs':'Auftragsspezifische Kosten'};
    detail['Existing order lump sum']='Bisherige Auftragspauschale';
    return {description:line.category&&line.category!=='material'?this.label(line.category):line.description,detail:detail[line.detail]||line.detail};
  }
  colorFamily(hex) {
    if(!/^#[a-f0-9]{6}$/i.test(hex||''))return 'Unbekannt';
    const rgb=[1,3,5].map(i=>parseInt(hex.slice(i,i+2),16)/255),max=Math.max(...rgb),min=Math.min(...rgb),delta=max-min;
    if(delta<.12)return max>.85?'Weiß':max<.2?'Schwarz':'Grau';
    const [r,g,b]=rgb;let hue=(max===r?(g-b)/delta+(g<b?6:0):max===g?(b-r)/delta+2:(r-g)/delta+4)*60;
    return hue<20||hue>=345?'Rot':hue<45?(max<.65?'Braun':'Orange'):hue<70?'Gelb':hue<170?'Grün':hue<255?'Blau':hue<290?'Violett':'Pink';
  }
  workspaceRows(view=this.view) {
    const d=this.data||{},s=this.workspaceState(view),f=s.filters,customers=d.customers||[],orders=d.orders||[];
    let rows=({rolls:d.spools,slots:d.slots,orders:d.orders,customers:d.customers,prints:d.jobs,history:d.stock_events})[view]||[];
    const customerName=id=>customers.find(c=>c.id===id)?.name||'';
    const orderFor=r=>orders.find(o=>o.id===r.customer_order_uuid);
    const mass=r=>view==='prints'?(r.settlement?.consumption?Object.values(r.settlement.consumption).reduce((a,b)=>a+b,0):null):r.available_mg;
    const slotFor=r=>(d.slots||[]).find(slot=>slot.spool_uuid===r.uuid)?.id||'';
    const searchable=r=>[r.product,r.manufacturer,r.material_type,r.color,this.colorFamily(r.color),r.material_preset,r.uuid,r.id,r.title,r.order_number,r.name,r.email,r.phone,r.contact_name,r.note,r.event_type,r.spool_id,r.job_id,this.label(r.status||r.state),customerName(r.customer_id||orderFor(r)?.customer_id),orderFor(r)?.title,orderFor(r)?.order_number,(d.spools||[]).find(p=>p.uuid===r.spool_id)?.product].join(' ').toLocaleLowerCase('de-DE');
    const query=(view===this.view?this.query:s.query)||'';
    rows=rows.filter(r=>{
      const archived=r.archived||r.status==='archived';
      if(['rolls','orders','customers'].includes(view)&&((f.archive!=='all'&&f.archive!=='archived'&&archived)||(f.archive==='archived'&&!archived)))return false;
      if(query.trim()&&!query.trim().toLocaleLowerCase('de-DE').split(/\s+/).every(term=>searchable(r).includes(term)))return false;
      if(f.material&&r.material_type!==f.material)return false;
      if(f.manufacturer&&r.manufacturer!==f.manufacturer)return false;
      if(f.color&&this.colorFamily(r.color)!==f.color)return false;
      if(f.slot&&(f.slot==='unassigned'?!!slotFor(r):f.slot==='assigned'?!slotFor(r):slotFor(r)!==f.slot))return false;
      if(f.min&&!(mass(r)!=null&&mass(r)>=Number(String(f.min).replace(',','.'))*1000))return false;
      if(f.max&&!(mass(r)!=null&&mass(r)<=Number(String(f.max).replace(',','.'))*1000))return false;
      if(f.quality&&(view==='prints'?r.settlement?.quality||'unknown':r.weight_quality||'unknown')!==f.quality)return false;
      if(f.profile&&(f.profile==='missing'?!!r.material_preset:!r.material_preset))return false;
      if(f.customer&&(r.customer_id||orderFor(r)?.customer_id)!==f.customer)return false;
      if(f.status&&(r.status||r.state)!==f.status)return false;
      if(f.quote&&(f.quote==='missing'?r.quoted_price_micros!=null:r.quoted_price_micros==null))return false;
      if(f.order&&(f.order==='unassigned'?!!r.customer_order_uuid:r.customer_order_uuid!==f.order))return false;
      if(f.printer&&(r.printer||r.printer_id||r.provider||r.source)!==f.printer)return false;
      if(f.from&&String(r.created_at||'').slice(0,10)<f.from)return false;
      if(f.to&&String(r.created_at||'').slice(0,10)>f.to)return false;
      if(f.roll&&r.spool_id!==f.roll)return false;
      if(f.activeOrders){const has=orders.some(o=>o.customer_id===r.id&&!o.archived&&['active','draft'].includes(o.status));if((f.activeOrders==='yes')!==has)return false;}
      return true;
    });
    const ranks={draft:0,active:1,reserved:2,printing:3,needs_review:4,completed:5,failed:6,cancelled:7,archived:8};
    const value=r=>({name:r.uuid?rollDisplayName(r):r.title||r.name||r.id,date:r.created_at,available:mass(r),remaining:r.remaining_mg,material:r.material_type,customer:customerName(r.customer_id||orderFor(r)?.customer_id),quote:r.quoted_price_micros,status:ranks[r.archived?'archived':r.status||r.state]??99,amount:r.delta_mg,slot:slotFor(r),net:this.accounting?.revision===d.revision?this.accounting?.orders?.find(o=>o.uuid===r.id)?.summary?.calculated_invoice_micros:null})[s.sort];
    return rows.slice().sort((a,b)=>{const x=value(a),y=value(b),empty=v=>v==null||v==='';if(empty(x)||empty(y))return empty(x)===empty(y)?String(a.uuid||a.id||a.created_at).localeCompare(String(b.uuid||b.id||b.created_at)):empty(x)?1:-1;
      const cmp=typeof x==='number'&&typeof y==='number'?x-y:String(x).localeCompare(String(y),'de',{numeric:true,sensitivity:'base'});
      return cmp*(s.direction==='desc'?-1:1)||String(a.uuid||a.id||a.created_at).localeCompare(String(b.uuid||b.id||b.created_at));});
  }
  viewLink(view,label,icon='arrow-right',query={}) {
    const params=Object.entries(query).map(([key,value])=>encodeURIComponent(key)+'='+encodeURIComponent(value)).join('&'),url='/dashboard-filament/'+view+(params?'?'+params:'');
    return `<a class="roll-action" href="${escapeHtml(url)}" title="${escapeHtml(label)}" aria-label="${escapeHtml(label)}"><ha-icon icon="mdi:${escapeHtml(icon)}" aria-hidden="true"></ha-icon></a>`;
  }
  button(label,action,id='',disabled=false) {
    const icons={'preheat':'radiator','preheat-refresh':'refresh','preheat-cancel':'cancel','preheat-retry':'restart','preheat-skip':'skip-next','preheat-extend':'timer-plus-outline','nfc-settings':'cellphone-cog','label-nfc-confirm':'check-circle-outline','label-reset':'backup-restore','new-roll':'plus','new-customer':'account-plus','new-order':'clipboard-plus-outline','mark-paid':'cash-check','cost-statement':'file-document-outline','restart-review':'restart-alert','new-extra-cost':'plus','edit-extra-cost':'pencil','remove-extra-cost':'delete-outline','edit-roll':'pencil','edit-customer':'pencil','edit-order':'pencil','cost-order':'clipboard-text-search-outline','cost-customer':'account-cash-outline','cost-job':'cash-multiple','assign-order':'clipboard-arrow-right-outline','archive-customer':'archive','archive-order':'archive','open-invoice':'file-document-outline','create-invoice':'file-document-plus-outline','print-document':'printer','download-html':'file-download-outline','download-text':'text-box-outline','clear':'link-off','scan':'qrcode-scan','nfc-link':'cellphone-nfc','nfc-unlink':'cellphone-off','reconcile':'scale','reconcile-provider':'scale','set-print-status':'clipboard-edit-outline','discard-provider':'cancel','correct-consumption':'pencil','export-recovery':'database-export-outline','roll-details':'information-outline','customer-details':'account-search-outline','slot-pick':'tray-arrow-down','customer-new-order':'clipboard-plus-outline','reset-filters':'filter-remove-outline','prev-page':'chevron-left','next-page':'chevron-right','refresh':'refresh'};
    if(action.startsWith('archive-')){const record=(action.endsWith('order')?this.data?.orders:this.data?.customers)?.find(r=>r.id===id);if(record?.archived)return this.rollButton(label,'archive-arrow-up',action,id,disabled);}
    return this.rollButton(label,icons[action]||'information-outline',action,id,disabled);
  }
  toolbar() {
    const s=this.workspaceState(),f=s.filters,d=this.data||{},e=escapeHtml;
    const unique=(rows,key)=>[...new Set((rows||[]).map(r=>r[key]).filter(Boolean))].sort((a,b)=>String(a).localeCompare(String(b),'de'));
    const select=(key,label,values)=>`<label>${e(label)}<select data-filter="${key}">${this.options([['','Alle'],...values],f[key]||'')}</select></label>`;
    let filters='';
    if(['rolls','orders','customers'].includes(this.view))filters+=select('archive','Archiv',[['active','Ausgeblendet'],['all','Einschließen'],['archived','Nur Archiv']]);
    if(this.view==='rolls'){
      filters+=select('material','Material',unique(d.spools,'material_type').map(v=>[v,v]))+select('manufacturer','Hersteller',unique(d.spools,'manufacturer').map(v=>[v,v]))+select('color','Farbe',[...new Set((d.spools||[]).map(r=>this.colorFamily(r.color)))].sort().map(v=>[v,v]));
      filters+=select('slot','Zuordnung',[['unassigned','Nicht zugeordnet'],['assigned','Im Drucker'],...(d.slots||[]).map(v=>[v.id,v.id])]);
      filters+=`<label>Mind. verfügbar (g)<input data-filter="min" type="text" inputmode="decimal" value="${e(f.min||'')}" placeholder="0"></label><label>Max. verfügbar (g)<input data-filter="max" type="text" inputmode="decimal" value="${e(f.max||'')}" placeholder="Unbegrenzt"></label>`;
      filters+=select('quality','Bestandsqualität',[['measured','Gewogen'],['estimated','Geschätzt'],['unknown','Unbekannt']])+select('profile','Materialprofil',[['present','Eigener Preset'],['missing','Standardauswahl']]);
    }
    if(['orders','prints'].includes(this.view))filters+=select('customer','Kunde',(d.customers||[]).map(c=>[c.id,c.name]));
    if(['orders','prints'].includes(this.view))filters+=select('status','Status',unique(this.view==='orders'?d.orders:d.jobs,this.view==='orders'?'status':'state').map(v=>[v,this.label(v)]));
    if(this.view==='orders')filters+=select('quote','Angebot',[['present','Vorhanden'],['missing','Fehlt']]);
    if(this.view==='prints'){
      filters+=select('order','Auftrag',[['unassigned','Ohne Auftrag'],...(d.orders||[]).map(o=>[o.id,`${(d.customers||[]).find(c=>c.id===o.customer_id)?.name||'—'} · ${o.order_number||''} ${o.title}${o.archived?' · Archiv':''} · ${o.id.slice(0,8)}`])]);
      filters+=select('quality','Verbrauch',[['measured','Gewogen'],['estimated','Geschätzt'],['unknown','Unbekannt']])+select('printer','Drucker / Quelle',[...new Set((d.jobs||[]).map(j=>j.printer||j.printer_id||j.provider||j.source).filter(Boolean))].map(v=>[v,this.label(v)]));
      filters+=`<label>Von<input data-filter="from" type="date" value="${e(f.from||'')}"></label><label>Bis<input data-filter="to" type="date" value="${e(f.to||'')}"></label>`;
    }
    if(this.view==='customers')filters+=select('activeOrders','Offene Aufträge',[['yes','Vorhanden'],['no','Keine']]);
    if(this.view==='history')filters+=select('roll','Rolle',(d.spools||[]).map(r=>[r.uuid,rollDisplayName(r)]));
    const sorts=({rolls:[['name','Rolle'],['material','Material'],['available','Verfügbar'],['remaining','Restbestand'],['slot','Slot']],orders:[['name','Auftrag'],['customer','Kunde'],['status','Status'],['quote','Angebot'],['net','Berechnet (netto)']],customers:[['name','Kunde']],prints:[['date','Datum'],['name','Druck'],['status','Status'],['customer','Kunde'],['available','Verbrauch']],history:[['date','Zeit'],['amount','Bestandsänderung']]})[this.view]||[['name','Name']];
    return `<div class="toolbar"><label class="search-label">Suche<input class="search" placeholder="${({rolls:'Rolle, Material oder Profil',orders:'Auftrag oder Kunde',prints:'Druck, Auftrag oder Kunde',customers:'Kunde oder Kontakt'})[this.view]||'In dieser Ansicht suchen'}" value="${e(this.query)}"></label>${this.button('Filter zurücksetzen','reset-filters')}</div><details class="filters" ${s.filtersOpen?'open':''}><summary><ha-icon icon="mdi:filter-variant" aria-hidden="true"></ha-icon> Filter${Object.entries(f).some(([k,v])=>v&&(k!=='archive'||v!=='active'))?' aktiv':''}</summary><div class="filter-grid">${filters}</div></details><div class="sorting"><label>Sortierung<select data-sort>${this.options(sorts,s.sort)}</select></label><label>Richtung<select data-direction>${this.options([['asc','Aufsteigend'],['desc','Absteigend']],s.direction)}</select></label><label>Zeilen<select data-size>${this.options([['10','10'],['25','25'],['50','50'],['100','100']],String(s.size))}</select></label></div>`;
  }
  listTable(columns,rows) {
    const e=escapeHtml,s=this.workspaceState();
    return `<div class="scroll workspace-table"><table><thead><tr>${columns.map(([title,key])=>`<th ${key?`aria-sort="${s.sort===key?(s.direction==='asc'?'ascending':'descending'):'none'}"`:''}>${key?`<button class="sort-head" data-sort-key="${key}" title="Nach ${e(title)} sortieren">${e(title)} ${s.sort===key?(s.direction==='asc'?'↑':'↓'):''}</button>`:e(title)}</th>`).join('')}</tr></thead><tbody>${rows.map(cells=>`<tr>${cells.map((cell,i)=>`<td data-label="${e(columns[i][0])}">${cell}</td>`).join('')}</tr>`).join('')||`<tr><td colspan="${columns.length}">Keine passenden Einträge. Filter zurücksetzen oder Archiv einschließen.</td></tr>`}</tbody></table></div>`;
  }
  bindActions(root=this.shadowRoot) {root.querySelectorAll('[data-action]').forEach(b=>b.onclick=()=>this.handle(b.dataset.action,b.dataset.id));}
  orderStatus(order) {
    const states={draft:['pencil-outline','draft'],active:['progress-clock','active'],completed:['check-circle-outline','completed'],cancelled:['close-circle-outline','cancelled']};
    const [icon,state]=states[order?.status]||['help-circle-outline','unknown'],e=escapeHtml;
    return `<span class="order-status order-status-${state}"><ha-icon icon="mdi:${icon}" aria-hidden="true"></ha-icon><span>${e(this.label(order?.status||'unknown'))}</span></span>${order?.archived?'<span class="order-status order-status-archived"><ha-icon icon="mdi:archive-outline" aria-hidden="true"></ha-icon> Archiviert</span>':''}`;
  }
  slotRollSummary(r) {
    const e=escapeHtml;
    if(!r)return '<div class="slot-empty"><b>Keine Rolle zugeordnet</b><p>Rolle aus dem Bestand auswählen</p></div>';
    const remaining=r.remaining_mg==null?'Unbekannt':this.grams(r.remaining_mg)+' g';
    return `<div class="slot-roll-summary">${this.spoolGauge(r)}<div class="roll-identity"><span class="roll-brand">${e(r.manufacturer||'Unbekannte Marke')}<span class="roll-material">${e(r.material_type||'—')}</span></span><strong class="roll-product" title="${e(rollDisplayName(r))}">${e(rollDisplayName(r))}</strong><span class="roll-quick"><span class="roll-remaining" title="Restbestand · ${e(this.label(r.weight_quality||'unknown'))}">${e(remaining)}</span>${this.labelBadges(r.uuid)}</span><span class="slot-color">${e(this.colorFamily(r.color))} · ${e(r.color||'—')}</span></div></div><small>Verfügbar: ${r.available_mg==null?'Unbekannt':this.grams(r.available_mg)+' g'} · Reserviert: ${r.reserved_mg==null?'Unbekannt':this.grams(r.reserved_mg)+' g'}</small>`;
  }
  spoolGauge(roll) {
    const e=escapeHtml,color=/^#[a-f0-9]{6}$/i.test(roll.color)?roll.color:'#8e9ba9';
    const known=Number.isFinite(roll.remaining_mg)&&roll.remaining_mg>=0&&Number.isFinite(roll.nominal_mg)&&roll.nominal_mg>0;
    const raw=known?roll.remaining_mg/roll.nominal_mg*100:null,percent=known?Math.max(0,Math.min(100,raw)):null;
    const description=known?`${this.grams(roll.remaining_mg)} g von ${this.grams(roll.nominal_mg)} g Nennmenge${raw>100?' · über der Nennmenge':''}`:'Füllstand unbekannt – Restbestand oder Nennmenge fehlt';
    return `<span class="spool-gauge" role="img" aria-label="${e(this.colorFamily(color)+' · '+description)}" title="${e(description)}"><svg viewBox="0 0 56 56" aria-hidden="true"><circle class="spool-rim" style="stroke:${color}" cx="28" cy="28" r="25"/><circle class="spool-track" cx="28" cy="28" r="18"/><circle class="spool-fill" cx="28" cy="28" r="18" pathLength="100" stroke="${color}" ${known?`stroke-dasharray="${percent} 100"`:'stroke-dasharray="4 4" opacity=".35"'} transform="rotate(-90 28 28)"/><circle class="spool-hub" cx="28" cy="28" r="6"/></svg><span class="spool-percent">${known?Math.round(raw)+'%':'?'}</span></span>`;
  }
  rollCards(rows,locked) {
    const e=escapeHtml;this.expandedRolls??=new Set();
    return `<div class="roll-list">${rows.map(r=>{
      const slot=this.data.slots?.find(s=>s.spool_uuid===r.uuid)?.id||'—',archived=r.status==='archived';
      const remaining=r.remaining_mg==null?'Unbekannt':this.grams(r.remaining_mg)+' g';
      return `<details class="roll-card" data-roll="${e(r.uuid)}" ${this.expandedRolls.has(r.uuid)?'open':''}><summary class="roll-summary">${this.spoolGauge(r)}<span class="roll-identity"><span class="roll-brand">${e(r.manufacturer||'Unbekannte Marke')}<span class="roll-material">${e(r.material_type||'—')}</span></span><strong class="roll-product" title="${e(rollDisplayName(r))}">${e(rollDisplayName(r))}</strong><span class="roll-quick"><span class="roll-remaining" title="Restbestand · ${e(this.label(r.weight_quality||'unknown'))}">${e(remaining)}</span><span class="roll-slot" title="Zugeordneter Druckerslot">${e(slot)}</span>${this.labelBadges(r.uuid)}${archived?'<span class="muted">Archiv</span>':''}</span></span><span class="roll-chevron" aria-hidden="true">⌄</span></summary><div class="roll-expanded"><dl><div><dt>Farbe</dt><dd>${e(this.colorFamily(r.color))} · ${e(r.color||'—')}</dd></div><div><dt>Materialprofil</dt><dd>${e(materialProfileLabel(r))}</dd></div><div><dt>Verfügbar / reserviert</dt><dd>${r.available_mg==null?'Unbekannt':this.grams(r.available_mg)+' g'} / ${this.grams(r.reserved_mg)} g</dd></div><div><dt>Nennmenge</dt><dd>${Number.isFinite(r.nominal_mg)&&r.nominal_mg>0?this.grams(r.nominal_mg)+' g':'Unbekannt'}</dd></div><div><dt>Bestandsqualität</dt><dd>${e(this.label(r.weight_quality||'unknown'))}</dd></div><div><dt>Rollenpreis</dt><dd>${this.money(r.material_price_per_kg_micros)}/kg</dd></div><div><dt>Rollen-ID</dt><dd><code>${e(r.uuid)}</code></dd></div></dl><div class="detail-actions">${this.rollButton('Rolle zuordnen','tray-arrow-down','assign-roll',r.uuid,locked||archived||r.remaining_mg===0)}${this.rollButton('Rolle bearbeiten','pencil','edit-roll',r.uuid,locked||archived)}${this.rollButton('Restbestand wiegen','scale','weigh',r.uuid,locked||archived)}${this.rollButton('QR-Etikett','qrcode','label',r.uuid)}${this.rollButton('NFC schreiben','nfc','nfc-write',r.uuid,locked||!window.QuackNfc?.canWrite(this._hass),'Benötigt die NFC-fähige Companion-App')}${this.button('Rollendetails','roll-details',r.uuid)}${this.rollButton(archived?'Wiederherstellen':'Archivieren',archived?'archive-arrow-up':'archive',archived?'restore':'archive',r.uuid,locked)}</div></div></details>`;
    }).join('')||'<p class="notice">Keine passenden Rollen. Filter zurücksetzen oder Archiv einschließen.</p>'}</div>`;
  }
  rollDetails(id) {
    const r=this.roll(id);if(!r)return;const e=escapeHtml,locked=!this.data.can_edit||this.online===false,archived=r.status==='archived';
    const box=this.sheet(rollDisplayName(r),`<p>${e(r.manufacturer)} · ${e(r.material_type)} · ${e(this.label(r.status))}</p><p><b>${r.remaining_mg==null?'Unbekannt':this.grams(r.remaining_mg)+' g'} Restbestand</b> · ${r.available_mg==null?'Unbekannt':this.grams(r.available_mg)+' g'} verfügbar · ${this.grams(r.reserved_mg)} g reserviert<br>${e(this.label(r.weight_quality))} · ${this.money(r.material_price_per_kg_micros)}/kg</p><p>Materialprofil: ${e(materialProfileLabel(r))}</p><p>Druckerersatz: ${e(r.bambu_material||'Nicht konfiguriert')} · ${e(r.color)}</p><p><code>${e(id)}</code></p><div class="detail-actions">${this.rollButton('Rolle zuordnen','tray-arrow-down','assign-roll',id,locked||archived||r.remaining_mg===0)}${this.rollButton('Rolle bearbeiten','pencil','edit-roll',id,locked||archived)}${this.rollButton('Restbestand wiegen','scale','weigh',id,locked||archived)}${this.rollButton('QR-Etikett','qrcode','label',id)}${this.rollButton('NFC schreiben','nfc','nfc-write',id,locked||!window.QuackNfc?.canWrite(this._hass),'Benötigt die NFC-fähige Companion-App')}${this.rollButton(archived?'Wiederherstellen':'Archivieren',archived?'archive-arrow-up':'archive',archived?'restore':'archive',id,locked)}${this.viewLink('history','Bestandsverlauf','history',{roll:id})}${this.button('NFC-Schreiben bestätigen','label-nfc-confirm',id,locked)}${this.button('NFC-Bestätigung zurücksetzen','label-reset',id,locked)}</div>`);
    this.bindActions(box);
  }
  customerDetails(id) {
    const c=this.data.customers.find(r=>r.id===id);if(!c)return;const e=escapeHtml,orders=this.data.orders.filter(o=>o.customer_id===id),locked=!this.data.can_edit||this.online===false;
    const box=this.sheet(c.name,`<p>${e(c.contact_name)}<br>${e(c.email)}<br>${e(c.phone)}</p><p>${e(c.notes)}</p><div class="detail-actions">${this.button('Auftrag für diesen Kunden anlegen','customer-new-order',id,locked||c.archived)}${this.button('Kunde bearbeiten','edit-customer',id,locked||c.archived)}${this.button('Kosten des Kunden','cost-customer',id)}${this.button(c.archived?'Kunde wiederherstellen':'Kunde archivieren','archive-customer',id,locked)}${this.viewLink('orders','Aufträge dieses Kunden','clipboard-list-outline',{customer:id})}</div><h3>Aufträge (${orders.length})</h3>${this.table(['Auftrag','Status','Öffnen'],orders.map(o=>`<tr><td>${e(o.title)}<small>${e(o.order_number)}</small></td><td>${this.orderStatus(o)}</td><td>${this.button('Auftrag öffnen','cost-order',o.id)}</td></tr>`))}`);this.bindActions(box);
  }
  recordEditor(action,id) {
    const d=this.data,e=escapeHtml,kind=action.includes('customer')?'customer':'order',rows=kind==='customer'?d.customers:d.orders,r=rows.find(v=>v.id===id)||{},revision=d.revision,requestKey=newRequestId();
    if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
    if(action.startsWith('archive')){this.editor(r.archived?'Wiederherstellen':'Archivieren',`<p>${e(r.name||r.title)}</p><p>Verlauf und gespeicherte Rechnungen bleiben erhalten.</p>`,()=>this.action(kind,{uuid:id,archive:!r.archived,revision,request_key:requestKey}));return;}
    if(r.archived)throw Error('Vor dem Bearbeiten wiederherstellen');
    let fields='';
    if(kind==='customer')for(const [name,label] of [['name','Name'],['contact_name','Kontaktperson'],['email','E-Mail'],['phone','Telefon'],['notes','Notizen']])fields+=this.field(name,label,r[name]||'','text',name==='name');
    else {
      const selected=r.customer_id||this.newOrderCustomer||'';this.newOrderCustomer=null;
      fields=`<label>Kunde<select name="customer_id" required>${this.options([['','Kunde auswählen'],...d.customers.filter(c=>!c.archived||c.id===r.customer_id).map(c=>[c.id,c.name]),...(!id?[['__new__','ï¼‹ Neuen Kunden anlegen']]:[])],selected)}</select></label>${!id?'<label data-inline-customer hidden>Neuer Kundenname<input name="new_customer_name" placeholder="Name des neuen Kunden"></label>':''}`;
      fields+=this.field('title','Auftrag / Titel',r.title||'','text',true)+this.field('order_number','Auftragsnummer',r.order_number||'')+this.field('notes','Notizen',r.notes||'');
      fields+=`<label>Status<select name="status">${this.options(['draft','active','completed','cancelled'].map(s=>[s,this.label(s)]),r.status||'active')}</select></label><details><summary>Preise & Abrechnung</summary>`;
      for(const [n,l] of [['quoted_price_micros','Angebot (EUR)'],['invoice_amount_micros','Erfasster Rechnungsbetrag (EUR)'],['design_hourly_rate_micros','Konstruktion (EUR/Stunde)'],['other_cost_micros','Sonstige Kosten gesamt inkl. Positionen (EUR)']])fields+=this.field(n,l,r[n]==null?(n==='design_hourly_rate_micros'?'45':n==='other_cost_micros'?'0':''):r[n]/1e6,'number');
      fields+=this.field('design_time_seconds','Konstruktionszeit (Minuten)',(r.design_time_seconds||0)/60,'number')+this.field('discount_basis_points','Rabatt (%)',(r.discount_basis_points||0)/100,'number');
      for(const n of ['material','electricity','machine_wear','maintenance','repair_reserve','design','other'])fields+=`<label><input type="checkbox" name="bill_${n}" ${r['bill_'+n]!==0?'checked':''}> ${e(this.label(n))} berechnen</label>`;
      fields+='</details>';
    }
    let customerRequest=null,createdCustomer=null,pendingOrder=null;
    const overlay=this.editor(id?(kind==='customer'?'Kunde bearbeiten':'Auftrag bearbeiten'):(kind==='customer'?'Neuer Kunde':'Neuer Auftrag'),fields,async form=>{
      const data={revision,request_key:requestKey};if(id)data.uuid=id;
      for(const [n,v] of form.entries())if(n!=='new_customer_name')data[n]=String(v);
      if(kind==='order'){
        if(!String(form.get('title')||'').trim())throw Error('Auftragstitel eingeben');
        for(const name of ['title','order_number','notes'])if(String(form.get(name)||'').length>(name==='notes'?4000:256))throw Error('Eingabe für '+name+' ist zu lang');
        const minutes=String(form.get('design_time_seconds')||'0').replace(',','.'),discount=String(form.get('discount_basis_points')||'0').replace(',','.');
        if(!/^\d+(\.\d+)?$/.test(minutes)||Number(minutes)*60>1e12)throw Error('Gültige nichtnegative Konstruktionszeit eingeben');
        if(!/^\d+(\.\d+)?$/.test(discount)||Number(discount)>100)throw Error('Rabatt zwischen 0 und 100 Prozent eingeben');
        for(const n of ['quoted_price_micros','invoice_amount_micros','design_hourly_rate_micros','other_cost_micros']){const value=String(form.get(n)||'').trim().replace(',','.');if(value&&!/^\d+(\.\d{1,6})?$/.test(value))throw Error('Gültigen positiven Betrag eingeben');data[n]=value===''?(n.includes('price')||n.includes('invoice')?null:0):Math.round(Number(value)*1e6);}
        data.design_time_seconds=Math.round(Number(minutes)*60);data.discount_basis_points=Math.round(Number(discount)*100);
        for(const n of ['material','electricity','machine_wear','maintenance','repair_reserve','design','other'])data['bill_'+n]=form.has('bill_'+n);
        if(data.customer_id==='__new__'){
          const name=String(form.get('new_customer_name')||'').trim();if(!name)throw Error('Namen des neuen Kunden eingeben');if(name.length>256)throw Error('Kundenname ist zu lang');
          if(customerRequest&&customerRequest.name!==name)throw Error('Zuerst den offenen Speichervorgang mit unverändertem Kundennamen wiederholen');
          customerRequest ||= {name,revision,request_key:newRequestId()};
          if(!createdCustomer)createdCustomer=await this.action('customer',customerRequest);
          data.customer_id=createdCustomer.uuid;data.revision=createdCustomer.revision;
        }else if(createdCustomer)throw Error('Der Kunde wurde bereits angelegt. Den offenen Auftrag zuerst unverändert speichern.');
        if(!data.customer_id)throw Error('Kunde auswählen');
        if(pendingOrder&&JSON.stringify(pendingOrder)!==JSON.stringify(data))throw Error('Speicherantwort unklar: Den offenen Vorgang unverändert wiederholen.');
        pendingOrder ||= data;
      }
      try{return await this.action(kind,pendingOrder||data);}
      catch(error){const status=error.status||error.status_code||error.statusCode;if([400,403,409,422].includes(status))pendingOrder=null;throw error;}
    });
    if(kind==='order'&&!id&&overlay){const select=overlay.querySelector('[name=customer_id]'),inline=overlay.querySelector('[data-inline-customer]');if(select&&inline)select.onchange=()=>{inline.hidden=select.value!=='__new__';inline.querySelector('input').required=!inline.hidden;};}
  }
  render() {
    const prior=this.shadowRoot.querySelector('.search'),selection=prior&&this.shadowRoot.activeElement===prior?[prior.selectionStart,prior.selectionEnd,prior.selectionDirection]:null;
    const focused=this.shadowRoot.activeElement,focusFilter=focused?.dataset?.filter,filterSelection=focusFilter&&focused.setSelectionRange?[focused.selectionStart,focused.selectionEnd]:null;
    const scan=this.shadowRoot.querySelector('#scan'),slot=this.shadowRoot.querySelector('#slot');if(scan)this.scanText=scan.value;if(slot)this.slotChoice=slot.value;
    const d=this.data,e=escapeHtml,s=this.workspaceState(),locked=!d?.can_edit||this.online===false;
    const priorFilters=this.shadowRoot.querySelector('.filters');if(priorFilters)s.filtersOpen=priorFilters.open;
    for(const [selector,key] of [['.tools','toolsOpen'],['.scan-tools','scanOpen']]){const prior=this.shadowRoot.querySelector(selector);if(prior)s[key]=prior.open;}
    const views={rolls:'Rollen',slots:'Drucker & Slots',orders:'Aufträge',prints:'Drucke',customers:'Kunden',history:'Bestandsverlauf',sync:'Synchronisierung',costs:'Kostenübersicht'};
    const titles=views[this.view]||'Rollen';let content='<p>Verbindung zum Materialbestand wird aufgebaut …</p>',top='',paging='';
    if(d){
      this.rememberWorkspace();
      if(!this.appliedRoute){const url=new URL(location.href);for(const key of ['customer','order','roll'])if(url.searchParams.has(key))s.filters[key]=url.searchParams.get(key);this.appliedRoute=true;}
      const rows=this.workspaceRows(),total=rows.length;const pages=Math.max(1,Math.ceil(total/s.size));s.page=Math.min(Math.max(1,s.page),pages);const page=rows.slice((s.page-1)*s.size,s.page*s.size);
      if(!['slots','sync','costs'].includes(this.view))paging=`<div class="pagination"><span>${total} Treffer · Seite ${s.page} von ${pages}</span><div>${this.button('Vorherige Seite','prev-page','',s.page===1)}${this.button('Nächste Seite','next-page','',s.page===pages)}</div></div>`;
      if(this.view==='rolls'){
        top=this.button('Neue Rolle','new-roll','',locked);
        content=this.rollCards(page,locked);
      }else if(this.view==='orders'){
        top=this.button('Neuer Auftrag','new-order','',locked);
        content=this.listTable([['Auftrag','name'],['Kunde','customer'],['Status','status'],['Angebot','quote'],['Berechnet (netto)','net'],['Aktionen','']],page.map(o=>{const cost=this.accounting?.revision===d.revision?this.accounting.orders?.find(c=>c.uuid===o.id):null;return [`<b>${e(o.title)}</b><small>${e(o.order_number||'')}${o.archived?' · Archiviert':''}</small>`,e(d.customers?.find(c=>c.id===o.customer_id)?.name||'Unbekannter Kunde'),`${this.orderStatus(o)}`,o.quoted_price_micros==null?'<span class="muted">Fehlt</span>':this.money(o.quoted_price_micros,o.currency),cost?this.costAmount(cost,'calculated_invoice_micros'):'<span class="muted">Wird geladen …</span>',this.button('Auftrag öffnen','cost-order',o.id)];}));
      }else if(this.view==='customers'){
        top=this.button('Neuer Kunde','new-customer','',locked);content=this.listTable([['Kunde','name'],['Kontakt',''],['Offene Aufträge',''],['Aktionen','']],page.map(c=>[`<b>${e(c.name)}</b>${c.archived?'<small>Archiviert</small>':''}`,`${e(c.contact_name||'—')}<small>${e(c.email)} ${e(c.phone)}</small>`,String((d.orders||[]).filter(o=>o.customer_id===c.id&&!o.archived&&['active','draft'].includes(o.status)).length),`${this.button('Kundendetails','customer-details',c.id)}${this.button('Auftrag für diesen Kunden anlegen','customer-new-order',c.id,locked||c.archived)}`]));
      }else if(this.view==='prints'){
        content=this.listTable([['Druck','name'],['Zeit','date'],['Status','status'],['Verbrauch','available'],['Auftrag / Kunde','customer'],['Aktionen','']],page.map(j=>{const o=d.orders?.find(o=>o.id===j.customer_order_uuid);return [`<b>${e(j.name)}</b><small>${e(this.label(j.source))}</small>`,e(j.created_at?new Date(j.created_at).toLocaleString('de-DE'):'—'),`<span class="badge">${e(this.label(j.state))}</span>${this.preheatSummary(j.uuid)}`,j.settlement?`${this.grams(Object.values(j.settlement.consumption||{}).reduce((a,b)=>a+b,0))} g<small>${e(this.label(j.settlement.quality))}${j.settlement.estimation?' · automatisch':''}${j.settlement.corrected?' · korrigiert':''}</small>`:'<span class="muted">Unbekannt / offen</span>',`${e(o?.title||'Ohne Auftrag')}<small>${e(d.customers?.find(c=>c.id===o?.customer_id)?.name||'')}</small>`,`${this.button('Auftrag ändern','assign-order',j.uuid,locked||o?.archived)}${this.button('Druckkosten','cost-job',j.uuid)}${j.source==='quack_provider'&&!j.settlement?this.button('Status ändern','set-print-status',j.uuid,locked):''}${j.settlement&&Object.keys(j.settlement.consumption||{}).length&&j.source==='quack_provider'?this.button('Verbrauch korrigieren','correct-consumption',j.uuid,locked):''}${j.can_discard_provider?this.button('Übertragung abgebrochen – Druck nicht gestartet','discard-provider',j.uuid,locked):''}${j.can_reconcile_provider?this.button('Verbrauch prüfen','reconcile-provider',j.uuid,locked):j.state==='needs_review'&&j.source==='printer_observation'?this.button('Verbrauch prüfen','reconcile',j.uuid,locked):''}`];}));
      }else if(this.view==='history'){
        content=this.listTable([['Zeit','date'],['Rolle','name'],['Änderung','amount'],['Grund / Druck','']],page.map(r=>[e(new Date(r.created_at).toLocaleString('de-DE')),e(rollDisplayName(this.roll(r.spool_id))||r.spool_id),this.grams(r.delta_mg)+' g',`${e(this.label(r.event_type))}<small>${e(r.note)} · ${e(r.job_id)}</small>`]));
      }else if(this.view==='slots'){
        const linked=this.scanText??new URL(location.href).searchParams.get('spool')??'';
        content=`<div class="slot-grid">${(d.slots||[]).map(slot=>{const r=this.roll(slot.spool_uuid);return `<section class="slot-card"><h3>${e(slot.id)}</h3>${this.slotRollSummary(r)}<small>Materialprofil: ${e(r?materialProfileLabel(r):'—')}</small><small>Druckerersatz: ${e(r?.bambu_material||'Nicht konfiguriert')}</small>${this.button('Rolle für '+slot.id+' auswählen','slot-pick',slot.id,locked)}${this.button('Zuordnung für '+slot.id+' aufheben','clear',slot.id,locked||!r)}${r?this.button('Rollendetails','roll-details',r.uuid):''}</section>`;}).join('')}</div><details class="scan-tools"><summary><ha-icon icon="mdi:qrcode-scan" aria-hidden="true"></ha-icon> QR / NFC & Handy</summary><p>QR-Code scannen oder Rollen-UUID einfügen. Die Zuordnung wird erst nach Prüfung gespeichert.</p><label>Rollen-UUID oder QR-Link<input id="scan" value="${e(linked)}" placeholder="UUID oder QR-Link"></label><label>Slot<select id="slot" aria-label="Slot"><option value="" ${!this.slotChoice?'selected':''}>Slot auswählen</option>${(d.slots||[]).map(s=>`<option ${s.id===this.slotChoice?'selected':''}>${e(s.id)}</option>`).join('')}</select></label>${this.button('Zuordnung prüfen','scan','',locked)}<p>NFC-Scans werden deinem Konto zugeordnet. Ohne festes Gerät sind Mehrfachanzeigen möglich.</p>${this.button('NFC-Standardgerät','nfc-settings','',locked||!window.QuackNfc?.request)}${this.button('Dieses Handy verknüpfen','nfc-link','',locked||!this._hass?.auth?.external||!window.QuackNfc?.request)}${this.button('Handyverknüpfung aufheben','nfc-unlink','',locked||!this._hass?.auth?.external||!window.QuackNfc?.request)}</details>`;
      }else if(this.view==='costs'){
        content='<p>Kosten und Rechnungen öffnen direkt am Auftrag.</p>'+this.listTable([['Auftrag',''],['Öffnen','']],(d.orders||[]).map(o=>[e(o.title),this.button('Auftrag öffnen','cost-order',o.id)]));
      }else{
        content=`<p>Bestandsführung: Home Assistant · Revision ${e(d.revision)}</p><p>${d.spools?.length||0} Rollen · ${d.customers?.length||0} Kunden · ${d.orders?.length||0} Aufträge · ${d.jobs?.length||0} Drucke</p><p>Letzte Quack-Übertragung: ${e(d.last_quack_sync||'Noch keine Verbindung')}</p><p>Rollen mit Standardauswahl: ${(d.spools||[]).filter(r=>!r.material_preset).length}</p><p>Bestandsbeobachter: ${e(d.observer_status||'Nicht konfiguriert')}</p><p>Import: ${e(d.import_source?.imported_at||'Keiner')}</p><p class="note">Materialprofile werden nur bei ausdrücklicher Synchronisierung übertragen. Bei unterbrochener Verbindung sind Änderungen gesperrt.</p>${this.button('Wiederherstellungsdaten exportieren','export-recovery','',locked)}<p class="note">Enthält Bestand, Materialprofile und bestätigte Vorgänge ohne Zugangsdaten. Export privat aufbewahren.</p>`;
      }
      const linked=new URL(location.href).searchParams.get('spool');if(linked&&this.view==='rolls'){let r;try{r=this.roll(decodeSpool(linked));}catch(_){}content=`<div class="notice">${r?e(rollDisplayName(r))+' · '+e(this.label(r.status)):'Unbekannte Rollen-UUID'} ${r?this.button('Slot auswählen','select-slot',r.uuid,locked||r.status==='archived'||r.remaining_mg===0):''}</div>`+content;}
    }
    if(d?.restart_reviews?.length)content=`<section class="notice" role="status"><b>Druckprüfung nach HA-Neustart</b><p>Zuordnung oder Verbrauch ist noch offen.</p>${d.restart_reviews.map(review=>`<div>${e(review.name)} ${this.button('Druck nach HA-Neustart prüfen','restart-review',review.job_uuid,locked)}</div>`).join('')}</section>`+content;
    this.shadowRoot.innerHTML=`<style>${WORKSPACE_CSS}.slot-card{min-width:0}.slot-card h3{margin:0 0 14px}.slot-roll-summary{display:grid;grid-template-columns:56px minmax(0,1fr);gap:12px;margin-bottom:14px;align-items:center}.slot-roll-summary .roll-product{white-space:normal;overflow-wrap:anywhere}.slot-color{font-size:11px;color:var(--secondary-text-color,#b0bbc6);overflow-wrap:anywhere}.slot-empty{min-height:102px}.slot-card>small{display:block;overflow-wrap:anywhere;margin-bottom:6px}.slot-card>button{margin-top:8px}</style><article><header class="workspace-header"><div><small>Material & Aufträge</small><h1>${e(titles)}</h1></div><div class="header-actions">${top}${this.button('Aktualisieren','refresh')}<details class="tools"><summary title="Werkzeuge" aria-label="Werkzeuge"><ha-icon icon="mdi:dots-vertical" aria-hidden="true"></ha-icon></summary><div>${this.viewLink('history','Bestandsverlauf','history')}${this.viewLink('sync','Synchronisierung','sync')}${this.view!=='rolls'?this.viewLink('rolls','Rollen','package-variant'):''}</div></details></div></header><div class="status" role="status">${e(this.message)}</div>${!['slots','sync','costs'].includes(this.view)?this.toolbar():''}${content}${paging}</article>`;
    const search=this.shadowRoot.querySelector('.search');if(search){search.oninput=ev=>{this.query=ev.target.value;s.page=1;this.render();};if(selection){search.focus({preventScroll:true});search.setSelectionRange(...selection);}}
    this.shadowRoot.querySelectorAll('[data-filter]').forEach(input=>{const change=()=>{s.filters[input.dataset.filter]=input.value;s.page=1;this.render();};input.onchange=change;if(['min','max','from','to'].includes(input.dataset.filter))input.oninput=change;});
    if(focusFilter){const input=this.shadowRoot.querySelector(`[data-filter="${focusFilter}"]`);input?.focus({preventScroll:true});if(filterSelection&&filterSelection[0]!=null)try{input.setSelectionRange(...filterSelection);}catch(_){}}
    this.shadowRoot.querySelectorAll('[data-sort-key]').forEach(b=>b.onclick=()=>{s.direction=s.sort===b.dataset.sortKey&&s.direction==='asc'?'desc':'asc';s.sort=b.dataset.sortKey;s.page=1;this.render();});
    const sort=this.shadowRoot.querySelector('[data-sort]');if(sort)sort.onchange=()=>{s.sort=sort.value;s.page=1;this.render();};const direction=this.shadowRoot.querySelector('[data-direction]');if(direction)direction.onchange=()=>{s.direction=direction.value;s.page=1;this.render();};const size=this.shadowRoot.querySelector('[data-size]');if(size)size.onchange=()=>{s.size=Number(size.value);s.page=1;this.render();};
    this.shadowRoot.querySelectorAll('[data-roll]').forEach(row=>{row.ontoggle=()=>{this.expandedRolls??=new Set();if(row.open)this.expandedRolls.add(row.dataset.roll);else this.expandedRolls.delete(row.dataset.roll);};});
    this.bindActions();window.QuackNfc?.controller?.attach(this);void window.QuackNfc?.controller?.deliver();if(typeof queueMicrotask==='function')queueMicrotask(()=>this.processQrIntent());
    for(const [selector,key] of [['.filters','filtersOpen'],['.tools','toolsOpen'],['.scan-tools','scanOpen']]){const panel=this.shadowRoot.querySelector(selector);if(panel){panel.open=!!s[key];panel.ontoggle=()=>{s[key]=panel.open;this.rememberWorkspace();};}}
  }

  editor(title, fields, submit, options={}) {
    this.colorPicker?.close();
    this.editing=true; const e=escapeHtml;
    this.shadowRoot.querySelector('.editor')?.remove();
    const overlay=document.createElement('div'); overlay.className='editor';
    overlay.setAttribute('role','dialog');overlay.setAttribute('aria-modal','true');overlay.setAttribute('aria-label',title);
    overlay.innerHTML=`<form class="sheet"><h2>${e(title)}</h2>${fields}<p class="form-error" role="alert"></p><button type="submit" class="roll-action" title="${e(options.submitLabel||'Speichern')}" aria-label="${e(options.submitLabel||'Speichern')}"><ha-icon icon="mdi:content-save-outline" aria-hidden="true"></ha-icon></button><button type="button" class="cancel roll-action" title="Abbrechen" aria-label="Abbrechen"><ha-icon icon="mdi:close" aria-hidden="true"></ha-icon></button></form>`;
    this.shadowRoot.append(overlay);
    const close=()=>{this.colorPicker?.close();this.editing=false;overlay.remove();this.render();};
    overlay.querySelector('.cancel').onclick=close;
    overlay.querySelector('form').onsubmit=async ev=>{ev.preventDefault(); const button=ev.target.querySelector('[type=submit]'); button.disabled=true;
      try { const result=await submit(new FormData(ev.target)); close();if(options.onSaved)options.onSaved(result); }
      catch(err) { overlay.querySelector('.form-error').textContent=this.error(err); button.disabled=false; }
    };
    return overlay;
  }
  async openColorPicker(overlay) {
    if(!window.QuackColorPicker)await import('/quack-materials/color_picker.js?v=20261007.5');
    const input=overlay.querySelector('[name=color]');
    this.colorPicker?.close();
    this.colorPicker=window.QuackColorPicker.open({parent:this.shadowRoot,initialColor:input.value,
      onApply:hex=>{input.value=hex;input.dispatchEvent(new Event('input',{bubbles:true}));},
      onClosed:()=>{this.colorPicker=null;}});
  }
  async processRestartIntent() {
    const ident=new URL(location.href).searchParams.get('restart_review');
    if(!ident||this.restartIntentSeen===ident||!this.data?.restart_reviews?.some(r=>r.job_uuid===ident))return;
    this.restartIntentSeen=ident;await this.handle('restart-review',ident);
  }
  field(name,label,value='',type='text',required=false) { return `<label>${escapeHtml(label)}<input name="${escapeHtml(name)}" type="${type}" value="${escapeHtml(value)}" ${required?'required':''} ${type==='number'?'step="any"':''}></label>`; }
  options(items,selected) { return items.map(([id,name])=>`<option value="${escapeHtml(id)}" ${id===selected?'selected':''}>${escapeHtml(name)}</option>`).join(''); }
  assignRoll(id, selectedSlot='', scan=null) {
    const d=this.data, e=escapeHtml, roll=this.roll(id);
    if (!d.can_edit||this.online===false) throw Error('Bestand ist schreibgeschützt');
    if (!roll) throw Error('Unbekannte Rollen-UUID; Rolle vor der Zuordnung registrieren');
    if (roll.status==='archived') throw Error('Archivierte Rolle; vor der Zuordnung wiederherstellen');
    if (roll.remaining_mg===0) throw Error('Rolle ist leer; andere Rolle auswählen oder Bestand korrigieren');
    if (d.printer_assignment?.enabled) {
      if (!window.QuackSlots) throw Error('Rollenauswahl wird geladen; bitte gleich erneut versuchen');
      if (window.QuackSlots.isOpen()) return;
      this.editing=true;
      window.QuackSlots.open(this._hass,{slot:selectedSlot,spoolUuid:id,nfcRequest:scan?.id,nfcNotice:scan?.duplicate_notice===true,onSaved:()=>this.refresh(),onClosed:()=>{this.editing=false;this.refresh();}});
      return;
    }
    // Keep the reviewed occupants and revisions stable until Save or Cancel.
    // Refresh can update this.data during editing without rewriting the draft.
    const slots=d.slots.map(slot=>({...slot}));
    const occupants=new Map(d.spools.map(spool=>[spool.uuid,{...spool}]));
    const existing=slots.find(slot=>slot.spool_uuid===id);
    const initial=slots.some(slot=>slot.id===selectedSlot)?selectedSlot:(existing?.id||'');
    const describe=uuid=>uuid?`${rollDisplayName(occupants.get(uuid))||'Unbekannte Rolle'} · ${uuid}`:'Nicht zugeordnet';
    const fields=`<p><b>${e(rollDisplayName(roll))}</b></p><small>UUID: ${e(id)}</small><p>Materialprofil: ${e(materialProfileLabel(roll))}</p><p>Restbestand: ${this.grams(roll.remaining_mg)} g · ${this.grams(roll.available_mg)} g verfügbar · ${this.grams(roll.reserved_mg)} g reserviert</p><label>Slot<select name="slot" required><option value="" ${!initial?'selected':''}>Slot auswählen</option>${this.options(slots.map(slot=>[slot.id,`${slot.id} · ${describe(slot.spool_uuid)}`]),initial)}</select></label><p data-assignment-preview role="status"></p><p class="note">Speichern bestätigt die logische Zuordnung in HA. Laden am Drucker erfolgt separat. Eine bestehende Zuordnung zu einem anderen Slot muss zuerst bewusst aufgehoben werden.</p>`;
    this.editor('Rolle zuordnen',fields,f=>{
      const slot=slots.find(slot=>slot.id===f.get('slot'));
      if (!slot) throw Error('Vor dem Speichern einen Slot auswählen');
      if (slot.spool_uuid===id) throw Error('Diese Rolle ist dem gewählten Slot bereits zugeordnet');
      return this.action('assign',{slot:slot.id,spool_uuid:id,revision:slot.revision});
    });
    const overlay=this.shadowRoot.querySelector('.editor');
    overlay.setAttribute('role','dialog');overlay.setAttribute('aria-modal','true');overlay.setAttribute('aria-label','Rolle zuordnen');
    const select=overlay.querySelector('[name=slot]'), preview=overlay.querySelector('[data-assignment-preview]');
    const save=overlay.querySelector('form').querySelector('[type=submit]');
    const update=()=>{
      const slot=slots.find(slot=>slot.id===select.value);
      preview.textContent=slot?`${slot.id}: ${describe(slot.spool_uuid)} → ${describe(id)}`:'Slot auswählen, um die Zuordnung zu prüfen';
      save.disabled=!slot||slot.spool_uuid===id;
    };
    select.value=initial;select.onchange=update;update();select.focus();
  }
  async handle(action,id) {
    const d=this.data, e=escapeHtml, requestKey=newRequestId(), key=()=>requestKey, revision=d.revision;
    try {
      if(action==='preheat'||action==='preheat-refresh'){await this.showPreheat(id);return;}
      if(['preheat-cancel','preheat-retry','preheat-skip','preheat-extend'].includes(action)){await this.editPreheat(id,action.slice(8));return;}
      if(action==='restart-review') {
        if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
        const fresh=await this.api('inventory'),review=fresh.restart_reviews?.find(r=>r.job_uuid===id);
        if(!review)throw Error('Diese Neustartprüfung ist bereits erledigt.');
        const observed=review.observed||{},baseline=review.baseline||{};
        const fields=`<p><b>Ursprünglicher Druck: ${e(review.name)}</b></p><p>Aktuelle Meldung: ${e(observed.name||'Noch keine frischen Daten')} · ${e(this.label(observed.state))}</p><p>Zuletzt gespeichert: ${baseline.progress_percent==null?'Fortschritt unbekannt':e(baseline.progress_percent)+' %'}</p><label>Entscheidung<select name="decision" required>${this.options([['','Entscheidung auswählen'],['resume_same','Laufender Druck ist derselbe – weiterverfolgen'],['completed','Gemeldeten Abschluss dieses Drucks bestätigen'],['failed','Gemeldeten Abbruch dieses Drucks bestätigen'],['different_job','Aktuellen Druck separat erfassen – ursprünglichen offen lassen'],['original_completed','Ursprünglicher Druck ist abgeschlossen – bestätigen'],['original_failed','Ursprünglicher Druck ist abgebrochen – gespeicherte Schätzung verwenden'],['keep_pending','Prüfung weiterhin offen lassen']],'')}</select></label><p>Die Bestätigung betrifft den ursprünglichen Druck und seine Rollen. Bei Abbruch wird nur dessen gespeicherter Fortschritt verwendet. Fehlende Mengen bleiben zur Verbrauchsprüfung offen. Ein anderer laufender Druck wird nicht gestoppt oder verändert.</p><label><input type="checkbox" name="confirmed" required> Ich habe den ursprünglichen Druck und das gewählte Ergebnis geprüft.</label>`;
        this.editor('Druck nach HA-Neustart prüfen',fields,form=>{
          if(!form.has('confirmed')||!['resume_same','completed','failed','different_job','original_completed','original_failed','keep_pending'].includes(form.get('decision')))throw Error('Entscheidung auswählen und Prüfung bestätigen.');
          return this.action('restart_confirmation',{job_uuid:id,action:form.get('decision'),confirmed:true,revision:fresh.revision,request_key:key()});
        },{submitLabel:'Prüfung bestätigen'});return;
      }
      if(action==='mark-paid') {
        if(this.busy)return;
        if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
        const result=this.costStatementView;
        if(!result||result.order.id!==id)throw Error('Kostenaufstellung zuerst öffnen.');
        if(result.order.archived||result.payment)throw Error('Dieser Auftrag ist bereits bezahlt oder archiviert.');
        await this.action('order_paid',{order_uuid:id,revision:result.revision,request_key:this.paymentRequestKey||key()});
        await this.showAccounting('order',id);return;
      }
      if(action==='cost-statement') {const result=await this.api('accounting?order_uuid='+encodeURIComponent(id));this.showCostStatement(result);return;}
      if(['new-extra-cost','edit-extra-cost','remove-extra-cost'].includes(action)) {
        if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
        const [orderId,itemId]=id.split(':');
        const result=await this.api('accounting?order_uuid='+encodeURIComponent(orderId));
        if(result.order?.archived)throw Error('Den Auftrag vor einer Kostenänderung wiederherstellen.');
        const items=result.extra_costs?.items||[];
        const item=items.find(row=>row.id===itemId);
        if(action!=='new-extra-cost'&&!item)throw Error('Unbekannte Sonderkostenposition; Auftrag neu öffnen.');
        const costId=item?.id||newRequestId();
        const clean=row=>({id:row.id,description:row.description,quantity_milli:row.quantity_milli,unit_price_micros:row.unit_price_micros});
        let fields=`<p>${e(result.order.title)}</p>`;
        if(action==='remove-extra-cost')fields+=`<p>Sonderkostenposition <b>${e(item.description)}</b> entfernen?</p><p>Gespeicherte Rechnungen und der Änderungsverlauf bleiben erhalten.</p>`;
        else fields+=this.field('description','Bezeichnung',item?.description||'','text',true)+
          this.field('quantity','Anzahl',item?item.quantity_milli/1000:1,'text',true)+
          this.field('unit_value','Einzelwert ('+result.order.currency+')',item?item.unit_price_micros/1e6:'','text',true)+
          '<p>Anzahl × Einzelwert ergibt den Gesamtbetrag. Bis zu drei Nachkommastellen für die Anzahl; jede Position wird auf Cent gerundet.</p>';
        this.editor(action==='remove-extra-cost'?'Sonderkostenposition entfernen':action==='edit-extra-cost'?'Sonderkostenposition bearbeiten':'Sonderkostenposition hinzufügen',fields,form=>{
          let updated=items.map(clean);
          if(action==='remove-extra-cost')updated=updated.filter(row=>row.id!==costId);
          else {
            const description=String(form.get('description')||'').trim();
            if(!description||description.length>256)throw Error('Bezeichnung mit höchstens 256 Zeichen eingeben');
            const quantity=costDecimal(form.get('quantity'),3,1e9);
            if(!quantity)throw Error('Anzahl muss größer als 0 sein');
            const changed={id:costId,description,quantity_milli:quantity,unit_price_micros:costDecimal(form.get('unit_value'),6,1e12)};
            updated=item?updated.map(row=>row.id===costId?changed:row):[...updated,changed];
          }
          return this.action('order_cost_items',{order_uuid:orderId,items:updated,revision:result.revision,request_key:key()});
        },{submitLabel:action==='remove-extra-cost'?'Position entfernen':'Speichern',onSaved:()=>this.showAccounting('order',orderId).catch(error=>{this.message=this.error(error);this.render();})});return;
      }
      if(['prev-page','next-page','reset-filters','refresh'].includes(action)) {
        const state=this.workspaceState();
        if(action==='reset-filters'){state.filters={archive:'active'};this.query='';state.query='';state.page=1;}
        else if(action==='prev-page')state.page=Math.max(1,state.page-1);
        else if(action==='next-page')state.page+=1;
        else {await this.refresh();return;}
        this.render();return;
      }
      if(action==='roll-details'){this.rollDetails(id);return;}
      if(action==='customer-details'){this.customerDetails(id);return;}
      if(action==='customer-new-order'){this.newOrderCustomer=id;await this.handle('new-order','');return;}
      if(action==='slot-pick'){
        if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
        if(d.printer_assignment?.enabled){if(!window.QuackSlots)throw Error('Rollenauswahl wird geladen');this.editing=true;window.QuackSlots.open(this._hass,{slot:id,onSaved:()=>this.refresh(),onClosed:()=>{this.editing=false;void this.refresh();}});}
        else {const choices=d.spools.filter(r=>r.status!=='archived'&&r.remaining_mg>0);this.editor('Rolle für '+id+' auswählen',`<label>Rolle<select name="spool_uuid" required><option value="">Rolle auswählen</option>${this.options(choices.map(r=>[r.uuid,rollDisplayName(r)]),'')}</select></label>`,form=>form.get('spool_uuid'),{onSaved:uuid=>this.assignRoll(uuid,id)});}
        return;
      }
      if(action==='assign-order') {
        if(!d.can_edit) throw Error('Bestand ist schreibgeschützt');
        const job=d.jobs.find(row=>row.uuid===id);
        if(!job) throw Error('Unbekannter Druck');
        const current=job.customer_order_uuid||null;
        const oldOrder=d.orders.find(order=>order.id===current);
        if(oldOrder?.archived) throw Error('Den aktuellen Auftrag wiederherstellen, bevor die Druckzuordnung geändert wird.');
        const choices=d.orders.filter(order=>order.id===current||(!order.archived&&['active','draft'].includes(order.status)));
        const describe=order=>`${d.customers.find(customer=>customer.id===order.customer_id)?.name||'Unbekannter Kunde'} · ${order.order_number?order.order_number+' · ':''}${order.title}`;
        const fields=`<p><b>${e(job.name)}</b> · ${e(this.label(job.state))}</p><label>Kundenauftrag<select name="customer_order_uuid">${this.options([['','Ohne Auftrag / privater Druck'],...choices.map(order=>[order.id,describe(order)])],current||'')}</select></label><p>Die erfassten Druckkosten werden dem gewählten Auftrag zugeordnet. Materialverbrauch, Bestand, gespeicherte Preise und Laufzeit bleiben erhalten.</p><p class="note">Abgeschlossene oder stornierte Zielaufträge müssen wieder geöffnet, archivierte Aufträge wiederhergestellt werden. Gespeicherte Rechnungen behalten ihre ursprünglichen Werte.</p>`;
        this.editor('Druck einem Auftrag zuordnen',fields,form=>{
          const selected=form.get('customer_order_uuid')||null;
          if(selected&&!choices.some(order=>order.id===selected))throw Error('Verfügbaren Kundenauftrag auswählen');
          return this.action('job_order',{job_uuid:id,expected_order_uuid:current,customer_order_uuid:selected,revision,request_key:key()});
        });return;
      }
      if(['cost-job','cost-order','cost-customer'].includes(action)) {await this.showAccounting(action.slice(5),id);return;}
      if(action==='open-invoice') {
        const result=await this.api('accounting?invoice_uuid='+encodeURIComponent(id));
        this.showSavedInvoice(result);return;
      }
      if(action==='create-invoice') {
        // Compatibility for an already-open older frontend action.
        const result=await this.api('accounting?order_uuid='+encodeURIComponent(id));
        this.showCostStatement(result);return;
      }
      if (action==='export-recovery') {
        if (this.busy) return;
        this.busy=true;
        try {
          const bundle=await this.api('recovery');
          const objectUrl=URL.createObjectURL(new Blob([JSON.stringify(bundle)],{type:'application/json'}));
          const link=document.createElement('a');link.href=objectUrl;
          link.download='quack-ha-recovery-'+new Date().toISOString().slice(0,10)+'.json';
          link.click();setTimeout(()=>URL.revokeObjectURL(objectUrl),1000);
          this.message='Wiederherstellungsdaten heruntergeladen';
        } finally {this.busy=false;this.render();}
        return;
      }
      if (action==='correct-consumption') {
        const job=d.jobs.find(j=>j.uuid===id);
        if (!job?.settlement || job.source!=='quack_provider') throw Error('Dieser Druck hat noch keinen gebuchten Verbrauch');
        const quantities=job.settlement.consumption||{};
        const rolls=Object.keys(quantities);
        let fields=`<p>${e(job.name)}</p><p>Neuen Gesamtverbrauch je Rolle eingeben. Es wird nur die Differenz zum bisherigen Verbrauch gebucht; Verlauf und ausgestellte Rechnungen bleiben erhalten.</p>`;
        for (const uuid of rolls) fields+=this.field('grams_'+uuid,(rollDisplayName(this.roll(uuid))||uuid)+' verbraucht (g)',this.grams(quantities[uuid]),'text',true);
        fields+='<label>Verbrauchsqualität<select name="quality"><option value="estimated">Geschätzt</option><option value="measured">Gewogen</option></select></label>';
        this.editor('Druckverbrauch korrigieren',fields,f=>this.action('correct_consumption',{
          job_uuid:id,revision,request_key:key(),confirmed:true,quality:f.get('quality'),
          consumption:Object.fromEntries(rolls.map(uuid=>[uuid,gramsToMg(f.get('grams_'+uuid))]))
        }));return;
      }
      if (action==='discard-provider') {
        if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
        const fresh=await this.api('inventory'),job=fresh.jobs?.find(j=>j.uuid===id);
        if(!fresh.can_edit||!job?.can_discard_provider)throw Error('Dieser Übertragungsversuch kann nicht mehr als nicht gestartet bestätigt werden.');
        const fields=`<p><b>${e(job.name)}</b></p><p>Nur bestätigen, wenn die Übertragung abgebrochen wurde und dieser Druck auf dem Drucker nicht gestartet ist. Der Versuch wird als verworfen mit 0 g Verbrauch abgeschlossen; seine Reservierung wird freigegeben. Bestand, Auftrag und andere Drucke bleiben erhalten.</p><label><input type="checkbox" name="confirmed" required> Ich bestätige: Dieser Druck ist nicht gestartet und hat kein Filament verbraucht.</label>`;
        this.editor('Abgebrochene Übertragung prüfen',fields,f=>{
          if(f.get('confirmed')!=='on')throw Error('Bitte den nicht gestarteten Druck ausdrücklich bestätigen.');
          return this.action('discard_provider',{job_uuid:id,revision:fresh.revision,request_key:key(),confirmed:true});
        },{submitLabel:'Als nicht gestartet bestätigen'});return;
      }
      if (action==='reconcile-provider' || action==='set-print-status') {
        const manual=action==='set-print-status';
        if (manual && (!d.can_edit || this.online===false)) throw Error('Bestand ist schreibgeschützt');
        const job=d.jobs.find(j=>j.uuid===id);
        if (manual ? (!job || job.source!=='quack_provider' || job.settlement) : !job?.can_reconcile_provider) throw Error('Dieser Druck kann nicht abgeschlossen werden');
        const rolls=[...new Set(job.allocations.map(a=>a.spool_uuid))];
        let fields=`<p>${e(job.name)} · Printer: ${e(job.observed_outcome)}</p><p>Gesamtverbrauch je reservierter Rolle eingeben. 0 nur verwenden, wenn kein Material verbraucht wurde. Ursprüngliche Material- und Preisdaten bleiben beim Druck.</p>`;
        for (const uuid of rolls) fields+=this.field('grams_'+uuid,(rollDisplayName(this.roll(uuid))||uuid)+' verbraucht (g)','','text',true);
        fields+='<label>Ergebnis<select name="outcome"><option value="failed">Fehlgeschlagen</option><option value="completed">Abgeschlossen</option></select></label><label>Verbrauchsqualität<select name="quality"><option value="estimated">Geschätzt</option><option value="measured">Gewogen</option></select></label>';
        if (manual) fields='<p>Nur verwenden, wenn dieser Druckversuch am Drucker beendet ist. Verbrauch je Rolle ausdrücklich eintragen; 0 nur, wenn nichts verbraucht wurde.</p>'+fields;
        this.editor(manual?'Druckstatus ändern':'Druckverbrauch prüfen',fields,f=>this.action(manual?'set_print_status':'reconcile_provider',{
          job_uuid:id,revision,request_key:key(),confirmed:true,outcome:f.get('outcome'),quality:f.get('quality'),
          consumption:Object.fromEntries(rolls.map(uuid=>[uuid,gramsToMg(f.get('grams_'+uuid))]))
        }));return;
      }
      if (action==='label') {
        const box=document.createElement('div'); box.className='editor';
        const show=async target=>{
          const label=await this.api('action/label',{spool_uuid:id,target});
          const app=target==='app';
          box.innerHTML=`<div class="sheet"><h2>${e(rollDisplayName(this.roll(id)))}</h2><p>${app?'HA-App':'Browser'} QR</p><img alt="Roll QR code" src="data:image/svg+xml;charset=utf-8,${encodeURIComponent(label.svg)}"><p><code>${e(id)}</code></p><p>${e(label.payload)}</p><p>${app?'Mit der iPhone-Kamera scannen, um die konfigurierte HA-App zu öffnen.':'Scannen öffnet den Browser. Die HTTPS-Einstellungen müssen zur HA-Adresse passen.'} Danach einen Slot auswählen. Archivierte und leere Rollen können nicht zugeordnet werden.</p><p data-error role="alert"></p><a download="roll-${e(id)}-${target}.svg" href="data:image/svg+xml;charset=utf-8,${encodeURIComponent(label.svg)}">Druckbares SVG herunterladen</a><button data-switch>${app?'Browser-QR':'HA-App-QR'}</button><button data-close class="roll-action" title="Schließen" aria-label="Schließen"><ha-icon icon="mdi:close" aria-hidden="true"></ha-icon></button></div>`;
          box.querySelector('[data-close]').onclick=()=>{this.editing=false;box.remove();this.render();};this.bindActions(box);
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
        if (!d.can_edit||this.online===false) throw Error('Bestand ist schreibgeschützt');
        if (!this.roll(id)) throw Error('Unknown roll UUID');
        if (!window.QuackNfc) throw Error('NFC support is still loading. Reopen the Companion app.');
        this.editing=false;this.shadowRoot.querySelector('.editor')?.remove();this.busy=true;
        this.message='NFC-Schreiben in der Companion-App …';this.render();
        try {
          const result=await window.QuackNfc.writeTag(this._hass,this.roll(id));
          if(result.written){await this.markLabel(id,'nfc',true,'native_write');this.message='NFC-Tag beschrieben.';}
          else this.message=result.outcome==='unsuccessful'?'NFC-Schreiben abgebrochen oder fehlgeschlagen.':'NFC-Schreibdialog angefordert; Ergebnis noch nicht bestätigt. In den Rollendetails bestätigen.';
        }finally{this.busy=false;if(!this.editing)this.render();}
        if(typeof setTimeout==='function')setTimeout(()=>{if(this.message.startsWith('NFC-')){this.message='';if(!this.editing)this.render();}},8000);
        return;
      }
      if (action==='label-nfc-confirm'||action==='label-reset') {
        if(!d.can_edit||this.online===false)throw Error('Bestand ist schreibgeschützt');
        const reset=action==='label-reset';
        const title=reset?'NFC-Bestätigung zurücksetzen':'NFC-Schreiben bestätigen';
        this.editor(title,`<p>${e(rollDisplayName(this.roll(id)))}</p><p>${reset?'Den NFC-Haken entfernen.':'Nur bestätigen, wenn der Tag tatsächlich beschrieben wurde.'}</p>`,async()=>{
          await this.markLabel(id,'nfc',!reset);
        });return;
      }
      if (action==='nfc-settings') {
        const result=await window.QuackNfc.request('devices');
        const selected=result.default_device||'';
        this.editor('NFC-Standardgerät',`<label>Standard für mein Konto<select name="device_id">${this.options([['','Undefiniert – mögliche Mehrfachanzeige'],...result.devices.map(device=>[device.id,device.name])],selected)}</select></label><p>Bei „Undefiniert“ ist keine Geräteverknüpfung erforderlich. Dialoge können auf mehreren geöffneten Geräten deines Kontos erscheinen.</p>`,form=>window.QuackNfc.request('preferences',{device_id:form.get('device_id')||null,confirmed:true}));return;
      }
      if (action==='nfc-link') {
        if (!d.can_edit||this.online===false) throw Error('Bestand ist schreibgeschützt');
        if (!window.QuackNfc?.request) throw Error('Diese Seite in der Companion-App öffnen.');
        const result=await window.QuackNfc.request('devices');
        if (!result.devices.length) throw Error(this.nfcText('No Companion device registered for this HA user.','Für diesen HA-Benutzer ist kein Companion-Gerät registriert.'));
        const fields=`<p>${e(this.nfcText('Choose the phone you are using now. This replaces any previous link for that device.','Wähle das Handy, das du gerade verwendest. Eine frühere Verknüpfung dieses Geräts wird ersetzt.'))}</p><label>${e(this.nfcText('This phone','Dieses Handy'))}<select name="device_id" required><option value="">${e(this.nfcText('Select this phone','Dieses Handy auswählen'))}</option>${this.options(result.devices.map(device=>[device.id,device.name]),null)}</select></label>`;
        this.editor(this.nfcText('Link this phone','Dieses Handy verknüpfen'),fields,async form=>{
          const device=form.get('device_id');
          if (!device || !result.devices.some(item=>item.id===device)) throw Error('Vor dem Speichern dieses Handy auswählen');
          await window.QuackNfc.request('bind',{device_id:device,confirmed:true});
          this.message=this.nfcText('Phone linked. Scan a roll tag to select a slot.','Handy verknüpft. Scanne einen Spulen-Tag, um einen Slot auszuwählen.');
        });return;
      }
      if (action==='nfc-unlink') {
        if (!d.can_edit||this.online===false) throw Error('Bestand ist schreibgeschützt');
        if (!window.QuackNfc?.request) throw Error('Diese Seite in der Companion-App öffnen.');
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
        this.editor(action==='archive'?'Rolle archivieren':'Rolle wiederherstellen',`<p>${e(rollDisplayName(this.roll(id)))} · ${e(id)}</p><p>Verlauf und UUID bleiben erhalten.</p>`,()=>this.action(action,{spool_uuid:id,revision,request_key:key()}));return;
      }
      if (action==='weigh') {
        this.editor('Gewogenen Restbestand erfassen',this.field('grams','Restliches Filament (g), ohne Leerrollengewicht',this.roll(id).remaining_mg/1000,'text',true),f=>this.action('weigh',{spool_uuid:id,remaining_mg:gramsToMg(f.get('grams')),revision,request_key:key()}));return;
      }
      if (action==='new-roll'||action==='edit-roll') {
        const r=this.roll(id)||{material_type:'PLA',color:'#FFFFFF',nominal_mg:1000000,remaining_mg:1000000,diameter_mm:1.75,density_g_cm3:1.24,material_price_per_kg_micros:20000000};
        let fields=`<small>UUID: ${e(id||'Wird beim Speichern automatisch erstellt')}</small>`;
        for (const [n,l] of [['product','Produkt'],['manufacturer','Hersteller'],['material_type','Materialtyp'],['material_preset','Quack-Materialprofil (optional; leer verwendet Standard)']]) fields+=this.field(n,l,n==='product'?rollProductName(r):r[n]||'','text',n!=='material_preset');
        fields+='<p class="note">Produkt: Farbe, Serie oder Variante – zum Beispiel Ocean Blue, Matte oder Basic. Anzeige: Hersteller Material Produkt. Bei bisherigen Gesamtnamen wird nur der Zusatz vorgeschlagen; ohne Zusatz die ungefähre Farbfamilie. Speichern übernimmt deine Eingabe.</p>';
        fields+=`<datalist id="profiles">${(d.profile_catalog||[]).map(p=>`<option value="${e(p)}"></option>`).join('')}</datalist>`;
        fields+=this.field('color','sRGB-Farbe',r.color,'color')+'<button type="button" data-color-photo class="roll-action" title="Farbe aus Kamera / Foto übernehmen" aria-label="Farbe aus Kamera / Foto übernehmen"><ha-icon icon="mdi:camera-iris" aria-hidden="true"></ha-icon></button>'+this.field('nominal','Nennfüllmenge (g)',r.nominal_mg/1000);
        if (!id) fields+=this.field('remaining','Anfänglicher Restbestand (g)',r.remaining_mg/1000);
        fields+=this.field('diameter_mm','Durchmesser (mm)',r.diameter_mm,'number')+this.field('density_g_cm3','Dichte (g/cm³)',r.density_g_cm3,'number')+this.field('price','Materialpreis (EUR/kg)',(r.material_price_per_kg_micros||0)/1e6,'number');
        this.editor(id?'Rolle bearbeiten':'Neue Rolle',fields,f=>{
          const data={revision,request_key:key()}; for(const n of ['product','manufacturer','material_type','material_preset','color']) data[n]=String(f.get(n));
          data.nominal_mg=gramsToMg(f.get('nominal')); data.diameter_mm=Number(f.get('diameter_mm'));data.density_g_cm3=Number(f.get('density_g_cm3'));data.material_price_per_kg_micros=Math.round(Number(f.get('price'))*1e6);
          if (id) data.spool_uuid=id;else data.remaining_mg=gramsToMg(f.get('remaining'));
          return this.action(id?'edit':'create',data);
        });this.shadowRoot.querySelector('[name=material_preset]').setAttribute('list','profiles');
        const overlay=this.shadowRoot.querySelector('.editor');
        overlay.querySelector('[data-color-photo]').onclick=()=>this.openColorPicker(overlay).catch(error=>{overlay.querySelector('.form-error').textContent=this.error(error);});return;
      }
      if (/customer|order/.test(action)) {this.recordEditor(action,id);return;}
      if(action==='reconcile') {
        const fields=`<label>Rolle<select name="spool_uuid">${this.options(d.spools.filter(s=>s.status!=='archived').map(s=>[s.uuid,rollDisplayName(s)+' · '+this.grams(s.remaining_mg)+' g']),null)}</select></label><label>Slot<select name="slot">${this.options(d.slots.map(s=>[s.id,s.id]),'A1')}</select></label>`+this.field('grams','Tatsächlicher oder bewusst geschätzter Gesamtverbrauch (g)', '', 'text',true)+`<label>Ergebnis<select name="outcome"><option value="completed">Abgeschlossen</option><option value="failed">Fehlgeschlagen</option></select></label><label>Verbrauchsqualität<select name="quality"><option value="estimated">Geschätzt</option><option value="measured">Gewogen</option></select></label><label>Auftrag<select name="customer_order_uuid"><option value="">Nicht zugeordnet</option>${this.options(d.orders.filter(o=>!o.archived).map(o=>[o.id,o.title]),null)}</select></label>`;
        this.editor('Beobachteten Druck prüfen',fields,f=>this.action('reconcile_observed',{job_uuid:id,spool_uuid:f.get('spool_uuid'),slot:f.get('slot'),consumed_mg:gramsToMg(f.get('grams')),outcome:f.get('outcome'),quality:f.get('quality'),customer_order_uuid:f.get('customer_order_uuid')||null,revision,request_key:key()}));
      }
    } catch(err) {
      this.message=this.error(err);
      const sheetError=this.shadowRoot?.querySelector('.sheet-error');
      if(this.editing&&sheetError)sheetError.textContent=this.message;
      else if(!this.editing)this.render();
    }
  }
}
customElements.define('quack-inventory-card',QuackInventoryCard);
window.customCards=window.customCards||[];
window.customCards.push({type:'quack-inventory-card',name:'Quack Inventory',description:'Central physical rolls, customers, orders and print attempts'});
