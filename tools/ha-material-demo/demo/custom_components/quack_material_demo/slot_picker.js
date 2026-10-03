// Integration-owned stock selection. Device commands are validated by the backend.

(() => {

  let activePicker=null;

  const escape = value => String(value ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

  const color = value => /^#[a-f0-9]{6}$/i.test(value || '') ? value : '#8899aa';

  const grams = value => (Number(value || 0)/1000).toLocaleString(undefined,{maximumFractionDigits:1});

  const filterRolls = (rolls,query) => rolls.filter(r=>r.status==='active' && r.remaining_mg>0 &&

    [r.manufacturer,r.product,r.material_type,r.color,r.uuid].join(' ').toLowerCase().includes(query.trim().toLowerCase()));

  const visibleSlots = (configured,data) => data ? configured.filter(slot=>data.slots.some(s=>s.id===slot)) : configured;

  function reason(roll,slot,slots) {

    const existing=slots.find(s=>s.spool_uuid===roll.uuid && s.id!==slot);

    if (existing) return 'Already assigned to '+existing.id;

    if (!['PLA','PETG','TPU'].includes(roll.material_type) || (roll.material_type==='TPU' && slot!=='EXT')) return 'No verified profile for this slot';

    return '';

  }

  function loadedSlot(config,states,extruder) {

    const rows=Object.entries(config?.slots || {});

    if (!rows.length || extruder!=='on' || !rows.every(([,id])=>states[id] && !['unavailable','unknown',''].includes(states[id].state) && typeof states[id].attributes?.active==='boolean')) return null;

    const active=rows.filter(([,id])=>states[id].attributes.active);

    return active.length===1 && states[active[0][1]].attributes.empty===false ? active[0][0] : null;

  }

  const css=`:host{font-family:var(--primary-font-family,Arial,sans-serif);color:var(--primary-text-color,#e6edf3)}

    button,input,select{font:inherit}button{cursor:pointer;border:1px solid #496172;border-radius:9px;background:#203e50;color:#e6edf3;padding:12px}button:disabled{opacity:.45;cursor:default}button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid #70d6f2;outline-offset:2px}

    small{color:#acbdca;line-height:1.5} .dot{display:inline-block;width:24px;height:24px;border:1px solid #8396a2;border-radius:50%;flex:none} .error{color:#ffb3b3} .success{color:#8fe3ac}`;

  class StockPicker extends HTMLElement {

    constructor(){super();this.attachShadow({mode:'open'});this.query='';this.busy=false;}

    text(en,de){return (this.hass?.locale?.language || this.hass?.language || '').startsWith('de')?de:en;}

    async connectedCallback(){

      this.previousFocus=document.activeElement;

      this.render();

      try {this.data=await this.hass.callApi('GET','quack_material_demo/inventory');this.render();this.shadowRoot.querySelector('input')?.focus();}

      catch(error){this.error=this.describe(error);this.render();}

    }

    describe(error){return error?.body?.error || error?.body?.message || error?.message || 'Request failed';}

    close(){if(this.busy)return;this.remove();this.previousFocus?.focus();}

    disconnectedCallback(){if(activePicker===this)activePicker=null;this.onClosed?.();}

    render(){

      const e=escape,d=this.data,pending=d?.printer_assignment?.pending;

      const chosen=d?.spools.find(r=>r.uuid===this.spoolUuid);

      this.shadowRoot.innerHTML=`<style>${css}

        :host{position:fixed;inset:0;z-index:10000;background:#0009;display:flex;align-items:center;justify-content:center;padding:16px;box-sizing:border-box}

        .sheet{width:620px;max-width:100%;max-height:90dvh;overflow:auto;box-sizing:border-box;background:#1d242d;border:1px solid #415462;border-radius:20px;padding:24px;box-shadow:0 18px 70px #0008}

        header,footer{display:flex;align-items:center;justify-content:space-between;gap:12px}h2{font-size:21px;margin:0}h3{font-size:16px}label{display:block;margin:18px 0 8px}input,select{width:100%;box-sizing:border-box;border:1px solid #617684;border-radius:8px;background:#121c23;color:#fff;padding:13px}

        .results{display:grid;gap:8px;max-height:310px;overflow:auto;margin-top:12px}.roll{display:flex;text-align:left;align-items:center;gap:12px;background:#17232c}.roll[aria-pressed=true]{border-color:#70d6f2;background:#193e50}.info{display:grid;gap:3px;min-width:0}.info strong{overflow-wrap:anywhere}.summary{padding:14px;background:#12232c;border-radius:10px;margin:16px 0;line-height:1.6}footer{margin-top:18px}.status{white-space:pre-wrap;overflow-wrap:anywhere}

        @media(max-width:500px){.sheet{padding:18px}.results{max-height:34dvh}h2{font-size:19px}}

        </style><section class="sheet" role="dialog" aria-modal="true" aria-labelledby="title"><header><h2 id="title">${e(this.text('Select inventory roll','Bestandsspule auswählen'))}${this.slot?' · '+e(this.slot):''}</h2><button data-close aria-label="Close" ${this.busy?'disabled':''}>✕</button></header>

        ${!d?'<p>'+e(this.error || this.text('Loading inventory…','Bestand wird geladen…'))+'</p>':!d.can_edit?'<p>Inventory administrator access required</p>':!d.printer_assignment?.enabled?'<p>Coordinated printer assignment is not enabled.</p>':`

        ${pending?`<div class="summary"><strong>${e(this.text('Unconfirmed assignment','Zuweisung noch nicht bestätigt'))} · ${e(pending.slot)}</strong><p>${e(d.spools.find(r=>r.uuid===pending.spool_uuid)?.product || pending.spool_uuid)}</p><small>${e(this.text('The HA binding stays unchanged until the printer reports the requested material.','Die HA-Zuordnung bleibt bestehen, bis der Drucker das angeforderte Material meldet.'))}</small><footer><button data-check ${this.busy?'disabled':''}>${e(this.text('Check state','Status prüfen'))}</button><button data-retry ${this.busy?'disabled':''}>${e(this.text('Send again','Erneut senden'))}</button></footer></div>`:`

        ${!this.fixedSlot?`<label>${e(this.text('Slot','Slot'))}<select data-slot ${this.busy?'disabled':''}><option value="">${e(this.text('Select a slot','Slot auswählen'))}</option>${d.slots.map(s=>`<option value="${e(s.id)}" ${s.id===this.slot?'selected':''}>${e(s.id)}</option>`).join('')}</select></label>`:''}

        <label for="search">${e(this.text('Search manufacturer, material or color','Hersteller, Material oder Farbe suchen'))}</label><input id="search" type="search" placeholder="${e(this.text('e.g. Ele…','z. B. Ele…'))}" value="${e(this.query)}" ${this.busy?'disabled':''}>

        <div class="results" aria-label="Inventory rolls"></div><div data-summary></div><footer><button data-cancel>${e(this.text('Cancel','Abbrechen'))}</button><button data-confirm disabled>${e(this.text('Assign material','Material zuweisen'))}</button></footer>`}

        <p class="status ${this.success?'success':'error'}" role="status">${e(this.busy?this.text('Waiting for printer confirmation…','Warte auf Druckerbestätigung…'):(this.success || this.error || ''))}</p>`}</section>`;

      this.shadowRoot.querySelector('[data-close]').onclick=()=>this.close();

      this.shadowRoot.querySelector('[data-cancel]')?.addEventListener('click',()=>this.close());

      this.shadowRoot.querySelector('[data-slot]')?.addEventListener('change',event=>{this.slot=event.target.value;this.request=null;this.success='';this.error='';this.render();});

      this.shadowRoot.querySelector('#search')?.addEventListener('input',event=>{this.query=event.target.value;this.drawResults();});

      this.shadowRoot.querySelector('[data-confirm]')?.addEventListener('click',()=>this.submit(false));

      this.shadowRoot.querySelector('[data-check]')?.addEventListener('click',()=>this.submit(false,pending));

      this.shadowRoot.querySelector('[data-retry]')?.addEventListener('click',()=>this.submit(true,pending));

      if(pending){

        const recovery=document.createElement('div');

        recovery.innerHTML=`<p><small>${e(this.text('Alternatively clear the HA slot to discard this operation. Printer material will remain unverified and must be reassigned before use.','Alternativ: HA-Slot leeren und diesen Vorgang verwerfen. Das Druckermaterial bleibt ungeprüft und muss vor Verwendung neu zugewiesen werden.'))}</small></p><button data-abandon ${this.busy?'disabled':''}>${e(this.text('Clear HA assignment','HA-Zuordnung leeren'))}</button>`;

        this.shadowRoot.querySelector('.sheet').appendChild(recovery);

        recovery.querySelector('button').onclick=()=>this.submit(false,pending,true);

      }

      this.shadowRoot.onkeydown=event=>{

        if(event.key==='Escape'){event.preventDefault();this.close();}

        if(event.key==='Tab'){

          const list=[...this.shadowRoot.querySelectorAll('button:not(:disabled),input:not(:disabled),select:not(:disabled)')];

          if(!list.length)return;const first=list[0],last=list[list.length-1],active=this.shadowRoot.activeElement;

          if(event.shiftKey&&active===first){event.preventDefault();last.focus();}else if(!event.shiftKey&&active===last){event.preventDefault();first.focus();}

        }

      };

      if(d&&!pending)this.drawResults();

    }

    drawResults(){

      const root=this.shadowRoot,box=root.querySelector('.results');if(!box)return;

      const e=escape,d=this.data,selected=d.spools.find(r=>r.uuid===this.spoolUuid);

      const rolls=filterRolls(d.spools,this.query);

      box.innerHTML=rolls.map(r=>{

        const why=reason(r,this.slot,d.slots);const current=d.slots.find(s=>s.spool_uuid===r.uuid);

        return `<button class="roll" data-uuid="${e(r.uuid)}" aria-pressed="${r.uuid===this.spoolUuid}" ${this.busy||why?'disabled':''}><span class="dot" style="background:${color(r.color)}"></span><span class="info"><strong>${e(r.product)}</strong><small>${e(r.manufacturer)} · ${e(r.material_type)} · ${e(r.color)}</small><small>${grams(r.remaining_mg)} g ${e(this.text('remaining','Rest'))} · ${grams(r.available_mg??r.remaining_mg)} g ${e(this.text('available','verfügbar'))} · …${e(r.uuid.slice(-8))}${current?' · '+e(current.id):''}</small>${why?'<small>'+e(why)+'</small>':''}</span></button>`;

      }).join('') || `<p>${e(this.text('No matching stock rolls','Keine passenden Bestandsspulen'))}</p>`;

      for(const button of box.querySelectorAll('[data-uuid]'))button.onclick=()=>{this.spoolUuid=button.dataset.uuid;this.request=null;this.error='';this.success='';this.render();};

      const profile=d.printer_assignment?.profiles?.[selected?.material_type];

      const metadata=d.printer_assignment?.metadata_sync?.[this.slot];

      const occupant=d.spools.find(r=>r.uuid===d.slots.find(s=>s.id===this.slot)?.spool_uuid);

      root.querySelector('[data-summary]').innerHTML=selected?`<div class="summary"><strong>${e(this.slot || this.text('Choose a slot','Slot auswählen'))}: ${e(occupant?.product || '—')} → ${e(selected.product)}</strong><br>${e(this.text('Printer','Drucker'))}: ${e(profile?.name || '—')} · ${e(selected.color)}<br>Quack: ${e(selected.material_preset || this.text('Compatible standard during synchronization','Kompatibles Standardprofil bei Synchronisierung'))}<br><small>${e(this.text('Updates assignment and printer material. Loading remains a separate action.','Aktualisiert Zuordnung und Druckermaterial. Laden bleibt eine eigene Aktion.'))}</small>${metadata?'<br><small>'+e(metadata.status==='confirmed'?(metadata.color_status==='approximate'?this.text('Printer reports an approximate color; HA color retained.','Drucker meldet abweichende Farbe; HA-Farbe bleibt erhalten.'):this.text('Printer material confirmed.','Druckermaterial bestätigt.')):this.text('Printer metadata is not yet confirmed.','Druckermetadaten noch nicht bestätigt.'))+'</small>':''}</div>`:'';

      root.querySelector('[data-confirm]').disabled=this.busy||!this.slot||!d.slots.some(s=>s.id===this.slot)||!selected||selected.status!=='active'||selected.remaining_mg<=0||!!reason(selected,this.slot,d.slots)||!d.printer_assignment?.ready;

    }

    async submit(retry,pending,cancel=false){

      if(this.busy)return;

      if(!pending&&!this.request){

        const slot=this.data.slots.find(s=>s.id===this.slot);if(!slot||!this.spoolUuid)return;

        const bytes=crypto.getRandomValues(new Uint8Array(16));

        this.request={slot:slot.id,spool_uuid:this.spoolUuid,revision:slot.revision,inventory_revision:this.data.revision,

          request_key:Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('')};

      }

      const request=pending || this.request;

      this.busy=true;this.error='';this.success='';this.render();

      try{

        const result=await this.hass.callApi('POST','quack_material_demo/slot_assignment',Object.fromEntries(

          ['slot','spool_uuid','revision','inventory_revision','request_key'].map(k=>[k,request[k]]).concat([['retry',retry],['cancel',cancel]])));

        if(result.status==='confirmed'){

          this.success=this.text('Material and inventory assignment confirmed. Quack can now synchronize this slot.','Material und Bestandszuordnung bestätigt. Quack kann diesen Slot jetzt synchronisieren.');

          if(result.color_status==='approximate') this.success+=' '+this.text(

            `HA color ${result.requested_color} was sent; the printer reports ${result.reported_color}. The HA color is retained.`,

            `HA-Farbe ${result.requested_color} wurde gesendet; der Drucker meldet ${result.reported_color}. Die HA-Farbe bleibt erhalten.`);

          else if(result.color_status==='unknown') this.success+=' '+this.text('Printer color is not confirmed. HA retains the requested color.','Die Druckerfarbe ist nicht bestätigt. HA behält die angeforderte Farbe.');

          this.onSaved?.();this.request=null;

        }else if(result.status==='cancelled'){

          this.error=this.text('HA slot cleared. Printer material is unverified; assign a stock roll before use.','HA-Slot geleert. Druckermaterial ist ungeprüft; vor Verwendung eine Bestandsspule zuweisen.');

          this.request=null;this.spoolUuid=null;this.onSaved?.();

        }else this.error=this.text('Not yet confirmed. Check the printer state or explicitly send again.','Noch nicht bestätigt. Druckerstatus prüfen oder ausdrücklich erneut senden.');

        this.data=await this.hass.callApi('GET','quack_material_demo/inventory');

      }catch(error){

        this.error=this.describe(error);

        // A lost HTTP response may hide a persisted operation. Reload its status.

        try{this.data=await this.hass.callApi('GET','quack_material_demo/inventory');}catch(_){}

      }finally{this.busy=false;this.render();}

    }

  }

  class StockSlotCard extends HTMLElement {

    constructor(){super();this.attachShadow({mode:'open'});}

    setConfig(config){this.config=config;if(!Array.isArray(config.slots)||config.slots.some(s=>!['A1','A2','A3','A4','HT1','EXT'].includes(s)))throw Error('Configure inventory slots');}

    set hass(value){this._hass=value;if(!this.data&&!this.loading)this.refresh();else if(this.data)this.render();}

    getCardSize(){return 3;}

    connectedCallback(){this.timer=setInterval(()=>this.refresh(),10000);}

    disconnectedCallback(){clearInterval(this.timer);}

    async refresh(){if(!this._hass||this.loading)return;this.loading=true;try{this.data=await this._hass.callApi('GET','quack_material_demo/inventory');this.error='';}catch(error){this.error=error?.message||'Inventory unavailable';}finally{this.loading=false;this.render();}}

    render(){

      const d=this.data,settings=d?.printer_assignment;
      const slotIds=visibleSlots(this.config.slots,d);
      if(!slotIds.length){this.shadowRoot.innerHTML='<p>No enabled inventory slots in this card.</p>';return;}

      const loaded=loadedSlot(settings,this._hass?.states||{},this._hass?.states?.[this.config.extruder_entity]?.state);

      this.shadowRoot.innerHTML=`<style>${css}.slots{display:grid;grid-template-columns:repeat(${slotIds.length},minmax(0,1fr));gap:12px}.slot{background:transparent;border:0;display:flex;align-items:center;flex-direction:column;gap:18px;padding:14px 4px;min-width:0}.ring{height:62px;width:62px;border:7px solid #344550;border-radius:50%;display:grid;place-items:center;box-shadow:0 0 0 3px transparent}.ring:after{content:'';width:27px;height:27px;border-radius:50%;background:#111c23}.loaded .ring{box-shadow:0 0 0 3px #70d6f2}.label{font-size:12px;min-height:44px;overflow-wrap:anywhere}.badge{color:#70d6f2;font-size:12px}@media(max-width:600px){.slots{grid-template-columns:repeat(${Math.min(2,slotIds.length)},minmax(0,1fr))}}</style><div class="slots">${slotIds.map(slot=>{

        const roll=d?.spools.find(r=>r.uuid===d.slots.find(s=>s.id===slot)?.spool_uuid);

        const reported=this._hass?.states?.[settings?.slots?.[slot]];

        return `<button class="slot ${loaded===slot?'loaded':''}" data-slot="${escape(slot)}" aria-label="Select inventory roll for ${escape(slot)}" ${!settings?.enabled||!d?.can_edit||this.error?'disabled':''}><small>${escape(slot)}</small><span class="ring" style="background:${color(roll?.color || reported?.attributes?.color)}"></span><span class="label">${escape(roll?.product || 'No stock roll assigned')}${roll?'<br><small>'+grams(roll.remaining_mg)+' g</small>':''}</span>${loaded===slot?'<span class="badge">✓ Geladen</span>':''}</button>`;

      }).join('')}</div>${this.error?'<p class="error">'+escape(this.error)+'</p>':''}`;

      for(const button of this.shadowRoot.querySelectorAll('[data-slot]'))button.onclick=()=>open(this._hass,{slot:button.dataset.slot,onSaved:()=>this.refresh()});

    }

  }

  function open(hass,options={}){

    if(activePicker?.isConnected)return activePicker;

    const element=document.createElement('quack-stock-picker');

    activePicker=element;

    Object.assign(element,{hass,...options,fixedSlot:!!options.slot});document.body.appendChild(element);return element;

  }

  customElements.define('quack-stock-picker',StockPicker);

  customElements.define('quack-stock-slot-card',StockSlotCard);

  window.QuackSlots={open,isOpen:()=>!!activePicker?.isConnected,filterRolls,reason,escape,loadedSlot,visibleSlots};

})();
