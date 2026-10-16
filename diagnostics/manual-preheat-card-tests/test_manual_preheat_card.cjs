// Manual preheat controls must never heat on render, refresh, or reconnect.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=process.argv[2]||require('node:path').resolve(__dirname,'../../tools/ha-material-demo/demo/custom_components/quack_materials/manual_preheat.js');
const elements=new Map();
class Element {
  constructor(){this.disabled=false;this.value='';this.textContent='';this.listeners={};this.dataset={};}
  addEventListener(name,fn){this.listeners[name]=fn;}
}
const shadow={innerHTML:'',activeElement:null,querySelector(selector){if(!elements.has(selector))elements.set(selector,new Element());return elements.get(selector);}};
const timers=[];let registered,uuid=0,confirmations=[];
const context={HTMLElement:class{get prefix(){return null;} attachShadow(){this.shadowRoot=shadow;return shadow;}},
  customElements:{define(name,klass){assert.equal(name,'quack-preheat-controls');registered=klass;}},
  crypto:process.env.MANUAL_PREHEAT_HTTP==='1'?{getRandomValues:array=>require('node:crypto').webcrypto.getRandomValues(array)}:
    {randomUUID:()=>`uuid-${++uuid}`},setInterval:fn=>{timers.push(fn);return timers.length;},clearInterval:()=>{},
  window:{confirm:message=>{confirmations.push(message);return true;}},Date,Number,Promise,console};
vm.runInNewContext(fs.readFileSync(source,'utf8'),context);
const calls=[];
let reply={schema_version:1,enabled:true,preheat:null};
const hass={user:{is_admin:true},states:{
  'binary_sensor.p2s_p2stest00000001_online':{state:'on'},
  'sensor.p2s_p2stest00000001_print_status':{state:'finish'},
  'sensor.p2s_p2stest00000001_current_stage':{state:'idle'}},
  async callApi(method,path,data){calls.push({method,path,data});return reply;}};
(async()=>{
 const card=new registered();card.setConfig({printer_id:'P2STEST00000001',compact:true});card.hass=hass;
 card.connectedCallback();await card.refresh();
 assert(calls.every(c=>c.method==='GET'),'Creating and refreshing the card cannot actuate heaters');
 assert.equal(elements.get('#settings').hidden,true,'Compact settings start collapsed');
 elements.get('#edit').listeners.click();
 assert.equal(elements.get('#settings').hidden,false);
 elements.get('#chamber').listeners.input({target:{value:'53'}});
 assert(calls.every(c=>c.method==='GET'),'Opening and editing targets cannot start heating');
 assert.equal(card.targets.chamber,'53');
 elements.get('#chamber').listeners.input({target:{value:'55'}});
 elements.get('#edit').listeners.click();
 assert.equal(elements.get('#settings').hidden,true);
 assert.equal(card.canStart(),true);
 await card.start();
 let start=calls.find(c=>c.method==='POST');
 assert.equal(start.path,'quack_materials/manual_preheat/start');
 assert.equal(start.data.parameters.chamber_target,55);
 assert.equal(start.data.parameters.preheat_bed_target,110);
 assert.equal(start.data.parameters.print_bed_target,110);
 assert.equal(start.data.parameters.enabled,true);
 assert.equal(confirmations.length,1);
 if(process.env.MANUAL_PREHEAT_HTTP==='1'){
  assert.match(start.data.job_uuid,/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  assert.notEqual(start.data.instance_id,'ha-manual:'+start.data.job_uuid);
 }
 const pendingKey=start.data.request_key;
 hass.callApi=async(method,path,data)=>{calls.push({method,path,data});if(method==='POST')throw Error('Connection lost');return reply;};
 await card.start();await card.start();
 const retries=calls.filter(c=>c.method==='POST').slice(-2);
 assert.equal(retries[0].data.request_key,retries[1].data.request_key,'Lost replies keep a stable attempt and command identity');
 assert.equal(retries[0].data.request_key,pendingKey);
 hass.callApi=async(method,path,data)=>{calls.push({method,path,data});return reply;};
 reply={schema_version:1,enabled:true,preheat:{job_uuid:'own-job',instance_id:'owner',version:2,phase:'heating',reason:'warming_bed',
  parameters:{enabled:true,chamber_target:55,preheat_bed_target:110,print_bed_target:110},expires_at:Date.now()/1000+2700,
  telemetry:{chamber_temp:40,bed_temp:100,bed_target:110}}};
 await card.refresh();assert.equal(card.canStart(),false,'An active manual attempt occupies the printer');
 await card.cancel();const cancel=calls.at(-1);
 assert.equal(cancel.path,'quack_materials/manual_preheat/cancel');assert.equal(cancel.data.job_uuid,'own-job');assert.equal(cancel.data.instance_id,'owner');assert.equal(cancel.data.version,2);
 const cancelCrypto=context.crypto;context.crypto={};card.cancelPending=null;
 const beforeCancelIdFailure=calls.filter(c=>c.method==='POST').length;
 await card.cancel();
 assert.equal(calls.filter(c=>c.method==='POST').length,beforeCancelIdFailure);
 assert.match(card.message,/Auftragskennung/,'Cancel ID failures must be visible');
 context.crypto=cancelCrypto;
 const oldRecord=card.record;
 const beforeRace=calls.filter(c=>c.method==='POST').length;
 reply={schema_version:1,enabled:true,preheat:{...oldRecord,job_uuid:'new-job',instance_id:'another-tab'}};
 await card.cancel();assert.equal(calls.filter(c=>c.method==='POST').length,beforeRace,'A replacement attempt needs its own confirmation');
 card.pending={job_uuid:'new-job'};
 reply={schema_version:1,enabled:true,preheat:{...reply.preheat,phase:'expired'}};
 await card.refresh();assert.equal(card.pending,null,'A terminal reply resolves the lost start identity and permits a future attempt');
 reply={schema_version:1,enabled:true,ready_to_start:false,preheat:{...reply.preheat,phase:'transferred',successor_job_uuid:'provider-successor'}};
 await card.refresh();
 assert.equal(card.terminal(),true,'Transferred manual heating belongs to its print successor');
 assert.equal(elements.get('#cancel').disabled,true,'Old manual controls cannot stop the successor');
 assert.equal(card.canStart(),false,'The successor occupies the printer');
 assert.equal(elements.get('#status').textContent,'Vom Druckauftrag übernommen');
 const transferred=reply;
 hass.callApi=async(method,path,data)=>{calls.push({method,path,data});return path.endsWith('/provider-successor')?
  {job_uuid:'provider-successor',schema_version:1,phase:'expired',reason:'deadline',expires_at:0,
   parameters:{chamber_target:56,preheat_bed_target:110,print_bed_target:110},telemetry:{bed_temp:98,bed_target:0,chamber_temp:54}}:transferred;};
 const writesBeforeSuccessor=calls.filter(c=>c.method==='POST').length;
 await card.refresh();
 assert.match(elements.get('#status').textContent,/Druckauftrag.*Zeitlimit erreicht/);
 assert.match(elements.get('#temperatures').textContent,/Soll 0 °C/,'Show actual printer target, not the configured warm-up target');
 assert.equal(elements.get('#deadline').textContent,'','An expired successor has no active countdown');
 assert.equal(elements.get('#cancel').disabled,true);
 assert.equal(calls.filter(c=>c.method==='POST').length,writesBeforeSuccessor,'Reading successor status never actuates');
 hass.callApi=async(method,path,data)=>{calls.push({method,path,data});if(path.endsWith('/provider-successor'))throw Error('Offline');return transferred;};
 await card.refresh();
 assert.match(elements.get('#message').textContent,/Nachfolgerstatus.*nicht bestätigt/,'Do not present a cached successor as current after a failed refresh');
 assert(!elements.get('#status').textContent.includes('Zeitlimit erreicht'));
 hass.callApi=async(method,path,data)=>{calls.push({method,path,data});return reply;};
 reply={schema_version:1,enabled:true,preheat:null};await card.refresh();
 reply={schema_version:1,enabled:true,preheat:{job_uuid:'hot-job',phase:'ready',reason:'target_reached',
  parameters:{chamber_target:55,preheat_bed_target:110,print_bed_target:110},telemetry:{chamber_temp:57,bed_temp:110,bed_target:110}}};
 await card.refresh();
 assert.match(elements.get('#status').textContent,/Kammerziel erreicht/);
 assert(!elements.get('#status').textContent.includes('stabil'),'A reached target must not be mislabeled as a measured plateau');
 assert.match(elements.get('#message').textContent,/keine zusätzliche Plateauwartezeit/);
 reply={schema_version:1,enabled:true,preheat:null};await card.refresh();
 const savedCrypto=context.crypto;context.crypto={};card.pending=null;
 const beforeUnavailable=calls.filter(c=>c.method==='POST').length;
 await card.start();
 assert.equal(calls.filter(c=>c.method==='POST').length,beforeUnavailable,'Unavailable secure randomness cannot send a malformed start');
 assert.match(card.message,/Auftragskennung/,'ID generation failures are visible instead of silently losing the click');
 context.crypto=savedCrypto;
 reply={schema_version:1,enabled:true,ready_to_start:false,blocked_reason:'stale_status',preheat:null};await card.refresh();
 assert.equal(card.canStart(),false,'Backend safety diagnostics may veto otherwise idle UI state');
 reply={schema_version:1,enabled:true,preheat:null};await card.refresh();
 for(const status of ['running','pause','prepare','unknown','unavailable']){
  hass.states['sensor.p2s_p2stest00000001_print_status'].state=status;card.hass=hass;assert.equal(card.canStart(),false);
 }
 hass.states['sensor.p2s_p2stest00000001_print_status'].state='finish';hass.user.is_admin=false;card.hass=hass;
 assert.equal(card.canStart(),false,'Only an administrator can start manual heating');
 card.disconnectedCallback();console.log('PASS manual preheat: passive render/GET, explicit confirmation, stable retries, occupancy and permissions');
})().catch(error=>{console.error(error);process.exitCode=1;});
