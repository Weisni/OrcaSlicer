// Explicit manual chamber warm-up. Rendering and polling never send commands.
class QuackPreheatControls extends HTMLElement {
  constructor() {
    super(); this.attachShadow({mode:'open'});
    this.targets={chamber:'55',bed:'110'};
    this.record=null; this.available=false; this.busy=false; this.message='';
  }
  setConfig(config) {
    if(!config || !/^[A-Za-z0-9_-]{1,128}$/.test(config.printer_id||''))throw Error('Configure a physical printer_id');
    this.config=config; this.printerPrefix='p2s_'+config.printer_id.toLowerCase(); this.render();
  }
  set hass(hass) { this._hass=hass; if(!this.shadowRoot.querySelector('#status'))this.render(); this.paint(); }
  connectedCallback() {
    if(!this.timer)this.timer=setInterval(()=>{void this.refresh();},5000);
    if(this._hass)void this.refresh();
  }
  disconnectedCallback() { if(this.timer)clearInterval(this.timer);this.timer=null; }
  getCardSize() { return this.config?.compact?(this.editing?3:1):7; }
  terminal() { return !this.record||['cancelled','expired','failed','manual_takeover','handoff','needs_review','transferred'].includes(this.record.phase); }
  canStart() {
    const states=this._hass?.states||{},status=states['sensor.'+this.printerPrefix+'_print_status'],stage=states['sensor.'+this.printerPrefix+'_current_stage'];
    return !!(this.available&&this.readyToStart!==false&&!this.busy&&this._hass?.user?.is_admin&&this.terminal()&&
      states['binary_sensor.'+this.printerPrefix+'_online']?.state==='on'&&
      ['idle','finish','failed'].includes(status?.state)&&stage?.state==='idle'&&
      !status?.attributes?.restored&&!stage?.attributes?.restored);
  }
  render() {
    const compact=!!this.config?.compact;
    this.shadowRoot.innerHTML=`<style>
      ha-card{background:transparent;box-shadow:none;color:var(--primary-text-color,#e6edf3);padding:16px 12px;border:0;border-top:1px solid #34404c;border-radius:0}
      h3{font-size:17px;margin:0 0 14px} .targets{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px}
      label{display:flex;flex-direction:column;gap:7px;font-size:14px} input{box-sizing:border-box;width:100%;padding:10px;border-radius:8px;border:1px solid #52616e;background:#17212b;color:inherit;font:inherit}
      .buttons{display:flex;flex-wrap:wrap;gap:10px;margin-top:14px}button{padding:11px 16px;border-radius:8px;border:1px solid #45616e;background:#173746;color:#c4effb;cursor:pointer;font:inherit}
      button:disabled{opacity:.4;cursor:default} .note{font-size:12px;color:var(--secondary-text-color,#b0bbc6);line-height:1.5} #status{font-weight:600;margin-top:15px} #message{color:#ffc27d;font-size:13px;white-space:pre-wrap}
      .compact{padding:8px 0 0;font-size:12px}.line{display:flex;align-items:center;gap:8px;min-height:32px}.line .title{color:#b0bbc6}.line #target-summary{margin-left:auto;font-variant-numeric:tabular-nums}
      .compact button{padding:5px 10px;font-size:12px}.compact #edit{padding:4px;background:transparent;border:0;color:#8eb8c8}.compact ha-icon{--mdc-icon-size:15px}
      .compact #settings{padding:10px 0}.compact .targets{gap:10px}.compact label{font-size:12px}.compact input{padding:6px;font-size:12px}
      .compact #status{font-size:11px;font-weight:400;margin:3px 0;color:#b0bbc6}.compact p{font-size:11px;margin:4px 0}.compact #message{font-size:11px}
      [hidden],p:empty{display:none!important}
    </style><ha-card class="${compact?'compact':''}">${compact?'<div class="line"><span class="title">Vorheizen</span><span id="target-summary"></span><button id="edit" aria-label="Vorheizziele bearbeiten" aria-controls="settings" aria-expanded="false" title="Vorheizziele bearbeiten"><ha-icon icon="mdi:pencil-outline"></ha-icon></button>':'<h3>Manuelle Vorheizung</h3>'}
    ${compact?'<button id="start">Starten</button><button id="cancel">Abbrechen</button></div>':''}
    <div id="settings"><div class="targets">
      <label>Kammerziel (°C)<input id="chamber" type="number" min="1" max="100" step="1" aria-label="Kammerziel"></label>
      <label>Druckbett beim Vorheizen (°C)<input id="bed" type="number" min="1" max="110" step="1" aria-label="Druckbett beim Vorheizen"></label>
    </div>${compact?'':'<div class="buttons"><button id="start">Vorheizung starten</button><button id="cancel">Vorheizung abbrechen</button></div>'}
    <p class="note">HA hält die Vorheizung höchstens 45 Minuten. Die Düse bleibt unverändert. Ein laufender Druck sperrt den Start. Abbrechen beendet die Heizauffrischungen; der Leerlaufschutz übernimmt die Abkühlung. Ein passender Quack-Druckauftrag übernimmt diese Vorheizung automatisch.</p></div>
    <div id="status" role="status"></div><p id="temperatures"></p><p id="deadline" class="note"></p>
    <p id="message" role="alert"></p></ha-card>`;
    for(const key of ['chamber','bed'])this.shadowRoot.querySelector('#'+key).addEventListener('input',event=>{this.targets[key]=event.target.value;this.paint();});
    if(compact)this.shadowRoot.querySelector('#edit').addEventListener('click',()=>{this.editing=!this.editing;this.paint();});
    this.shadowRoot.querySelector('#start').addEventListener('click',()=>{void this.start();});
    this.shadowRoot.querySelector('#cancel').addEventListener('click',()=>{void this.cancel();});
    this.paint();
  }
  paint() {
    const find=id=>this.shadowRoot.querySelector('#'+id);if(!find('status'))return;
    const active=!this.terminal(),record=this.successor||this.record;
    const compact=!!this.config?.compact;
    find('settings').hidden=compact&&!this.editing;
    if(compact){find('edit').ariaExpanded=String(!!this.editing);find('target-summary').textContent=(record?.parameters.chamber_target??this.targets.chamber)+' °C · Bett '+(record?.parameters.preheat_bed_target??this.targets.bed)+' °C';}
    for(const key of ['chamber','bed']){
      const field=find(key);field.disabled=this.busy||active||!!this.pending;
      if(this.shadowRoot.activeElement!==field)field.value=this.targets[key];
    }
    find('start').disabled=!this.canStart();find('start').textContent=this.pending?(compact?'Antwort prüfen':'Startantwort erneut prüfen'):(compact?'Starten':'Vorheizung starten');
    find('start').hidden=compact&&active;find('cancel').hidden=compact&&!active;
    find('cancel').disabled=!(this.available&&!this.busy&&active&&this._hass?.user?.is_admin);
    const phases={heating:'Vorheizauftrag aktiv',selecting_air:'Umluft wird eingestellt',conditioning_bed:'Druckbett wird vorbereitet',
      ready:'Kammer stabil · Temperatur wird gehalten',interrupted:'Vorheizung unterbrochen',cancelled:'Vorheizung abgebrochen',
      expired:'Zeitlimit erreicht',manual_takeover:'Sollwert oder Luftmodus geändert · Vorheizung beendet',failed:'Vorheizung fehlgeschlagen',needs_review:'Prüfung erforderlich',transferred:'Vom Druckauftrag übernommen'};
    const reasons={target_reached:'Kammerziel erreicht; keine zusätzliche Plateauwartezeit.',warming_bed:'Druckbett wird aufgeheizt.',stabilizing:'Kammerverlauf wird geprüft.',recovering_after_door_open:'Warte auf Temperaturerholung nach Türöffnung.',
      door_open_heating:'Tür offen: Vorheizung läuft weiter. Die Stabilitätsprüfung beginnt nach dem Schließen und der Temperaturerholung neu.',
      bed_target_reset:'Drucker meldet 0 °C: Das Auftragsziel wird erneut gesetzt. Zum Beenden den Vorheizauftrag abbrechen.',
      door_open_or_unknown:'Tür offen oder Zustand unbekannt.',idle_protection_unavailable:'Leerlaufschutz nicht bestätigt.',maintenance_mode:'Wartungsmodus aktiv oder unbekannt.',
      printer_busy_or_unknown:'Drucker beschäftigt oder Zustand unbekannt.',heater_command_failed:'Heizbefehl unbestätigt. Den Versuch abbrechen, Verbindung prüfen und bei Bedarf neu starten.'};
    const displayActive=record&&!['cancelled','expired','failed','manual_takeover','handoff','needs_review','transferred'].includes(record.phase);
    const why=displayActive?record.reason:this.blockedReason;
    const reason=why?.startsWith('stale_')?'Warte auf neue vollständige Druckerdaten.':reasons[why]||
      ({pending_dispatch:'Eine Druckübertragung muss zuerst abgeschlossen oder geklärt werden.',printer_owned:'Ein anderer Vorheizauftrag belegt den Drucker.',observer_busy:'Ein vorheriger Druck muss zuerst geklärt werden.'})[why]||'';
    if(record?.phase==='ready'&&record.reason==='target_reached')phases.ready='Kammerziel erreicht · Temperatur wird gehalten';
    find('status').textContent=!this.available?'Verbindung zur Vorheizsteuerung wird geprüft':record?
      (this.successor?'Druckauftrag · ':'')+(phases[record.phase]||'Status prüfen'):'Keine manuelle Vorheizung aktiv';
    find('status').hidden=compact&&this.available&&!record;
    const telemetry=record?.telemetry||{},fmt=value=>Number.isFinite(value)?value+' °C':'—';
    find('temperatures').textContent=record?`Kammer ${fmt(telemetry.chamber_temp)} / Ziel ${fmt(record.parameters.chamber_target)} · Druckbett ${fmt(telemetry.bed_temp)} / Soll ${fmt(telemetry.bed_target)}`:'';
    find('deadline').textContent=displayActive&&Number.isFinite(record.expires_at)?'Bis spätestens '+new Date(record.expires_at*1000).toLocaleTimeString('de-DE'):'';
    find('message').textContent=this.message||this.successorError||reason||(!this._hass?.user?.is_admin?'Zum Starten und Abbrechen ist ein Administrator erforderlich.':'');
  }
  receive(reply) {
    if(reply?.schema_version!==1||reply.enabled!==true||!(reply.preheat===null||typeof reply.preheat==='object'))throw Error('Ungültige Vorheizantwort');
    this.available=true;this.record=reply.preheat;this.successor=null;this.successorError='';this.readyToStart=reply.ready_to_start;this.blockedReason=reply.blocked_reason;
    if(this.pending?.job_uuid===this.record?.job_uuid){this.pending=null;this.message='';}
    if(this.message.startsWith('Vorheizsteuerung nicht erreichbar:'))this.message='';
    if(this.record&&!this.terminal()){
      this.targets={chamber:String(this.record.parameters.chamber_target),bed:String(this.record.parameters.preheat_bed_target)};
    }
    this.paint();
  }
  async refresh() {
    if(!this._hass||this.loading||this.busy)return;
    this.loading=true;
    try{
      this.receive(await this._hass.callApi('GET','quack_materials/manual_preheat'));
      const id=this.record?.phase==='transferred'?this.record.successor_job_uuid:null;
      if(typeof id==='string'&&/^[A-Za-z0-9_-]{1,128}$/.test(id)){
        try{
          const successor=await this._hass.callApi('GET','quack_materials/preheat/'+id);
          if(successor?.schema_version!==1||successor.job_uuid!==id||!successor.parameters||typeof successor.phase!=='string')throw Error('Ungültiger Nachfolgerstatus');
          this.successor=successor;
        }catch(error){this.successorError='Nachfolgerstatus nicht bestätigt: '+this.error(error);}
        this.paint();
      }
    }
    catch(error){this.available=false;this.message='Vorheizsteuerung nicht erreichbar: '+this.error(error);this.paint();}
    finally{this.loading=false;}
  }
  error(error) { return error?.body?.error||error?.message||'Verbindung prüfen'; }
  newId() {
    if(typeof crypto.randomUUID==='function')return crypto.randomUUID();
    // HA may run over local HTTP, where randomUUID requires a secure context.
    const bytes=crypto.getRandomValues(new Uint8Array(16));bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
    const hex=Array.from(bytes,value=>value.toString(16).padStart(2,'0')).join('');
    return `${hex.slice(0,8)}-${hex.slice(8,12)}-${hex.slice(12,16)}-${hex.slice(16,20)}-${hex.slice(20)}`;
  }
  async start() {
    if(!this.canStart())return;
    const chamber=Number(this.targets.chamber),bed=Number(this.targets.bed);
    if(!Number.isInteger(chamber)||chamber<1||chamber>100||!Number.isInteger(bed)||bed<1||bed>110){this.message='Gültige ganzzahlige Temperaturziele eingeben.';this.paint();return;}
    if(!window.confirm(`Kammer mit HA auf ${chamber} °C vorheizen? Das Druckbett wird auf ${bed} °C gesetzt und höchstens 45 Minuten gehalten. Die Düse bleibt unverändert.`))return;
    try{if(!this.pending){const id=this.newId();this.pending={job_uuid:id,request_key:'manual-start:'+id,instance_id:'ha-manual:'+this.newId(),printer_id:this.config.printer_id,
      parameters:{enabled:true,chamber_target:chamber,preheat_bed_target:bed,print_bed_target:bed}};}}
    catch(error){this.message='Auftragskennung konnte nicht erstellt werden. Browser prüfen und Seite neu laden.';this.paint();return;}
    this.busy=true;this.message='';this.paint();
    try{this.receive(await this._hass.callApi('POST','quack_materials/manual_preheat/start',this.pending));}
    catch(error){this.message='Startantwort unklar: '+this.error(error)+'. Erneut prüfen verwendet denselben Vorheizauftrag.';}
    finally{this.busy=false;this.paint();}
  }
  async cancel() {
    if(this.busy||!this._hass?.user?.is_admin||this.terminal())return;
    const confirmedJob=this.record.job_uuid;
    if(!window.confirm('Diese manuelle Vorheizung abbrechen und weitere Heizauffrischungen beenden?'))return;
    await this.refresh();if(this.terminal()||!this.available)return;
    if(this.record.job_uuid!==confirmedJob){this.message='Der Vorheizauftrag hat sich geändert. Den neuen Auftrag vor dem Abbruch prüfen.';this.paint();return;}
    const row=this.record;
    try{if(!this.cancelPending||this.cancelPending.job_uuid!==row.job_uuid||this.cancelPending.version!==row.version)
      this.cancelPending={job_uuid:row.job_uuid,request_key:'manual-cancel:'+this.newId(),instance_id:row.instance_id,version:row.version};}
    catch(error){this.message='Auftragskennung konnte nicht erstellt werden. Browser prüfen und Seite neu laden.';this.paint();return;}
    this.busy=true;this.message='';this.paint();
    try{this.receive(await this._hass.callApi('POST','quack_materials/manual_preheat/cancel',this.cancelPending));}
    catch(error){this.message='Abbruchantwort unklar: '+this.error(error)+'. Zustand erneut prüfen.';}
    finally{this.busy=false;this.paint();}
  }
}
customElements.define('quack-preheat-controls',QuackPreheatControls);
