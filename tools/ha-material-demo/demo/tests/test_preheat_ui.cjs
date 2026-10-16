// Exercise the active inventory preheat UI without a device or network service.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const context={window:{},URL,crypto:require('node:crypto').webcrypto,HTMLElement:class{},customElements:{define(){}},location:{href:'http://localhost/dashboard-filament/prints'}};
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_materials/inventory.js','utf8')+';globalThis.Card=QuackInventoryCard;',context);
const card=Object.create(context.Card.prototype),job='11111111-1111-4111-8111-111111111111';
card.data={can_edit:true,revision:7,spools:[],orders:[],customers:[],jobs:[{uuid:job,name:'Plate',state:'reserved',source:'quack_provider'}],
  job_preheats:[{job_uuid:job,phase:'interrupted',ready:false}]};
card.online=true;card.view='prints';card.query='';card.message='';
card.workspaceState=()=>({page:1,size:25,filters:{}});card.rememberWorkspace=()=>{};card.workspaceRows=()=>card.data.jobs;
card.shadowRoot={querySelector:()=>null,querySelectorAll:()=>[]};
card.render();
assert.match(card.shadowRoot.innerHTML,/Vorheizung/);
assert.match(card.shadowRoot.innerHTML,/data-action="preheat"/);
assert.match(card.shadowRoot.innerHTML,/data-action="set-print-status"/,'Manual status action must remain available');
card.data.job_preheats=[];card.render();assert.doesNotMatch(card.shadowRoot.innerHTML,/data-action="preheat"/);
card.data.job_preheats=[{job_uuid:job,phase:'interrupted'}];
(async()=>{
  let html='',submit,options,requests=[];
  const buttons=['preheat-refresh','preheat-cancel','preheat-retry','preheat-skip','preheat-extend'].map(action=>({dataset:{action,id:job}}));
  card.sheet=(title,body)=>{html=body;return {querySelectorAll:()=>buttons};};
  card.editor=(title,body,callback,opts)=>{html=body;submit=callback;options=opts;};
  card.api=async(path,body)=>{requests.push({path,body});return {job_uuid:job,instance_id:'owner',version:4,
    phase:body?'cancelled':'interrupted',reason:'stale_bed_temp',expires_at:10000,
    parameters:{enabled:true,chamber_target:55,print_bed_target:100,preheat_bed_target:110},telemetry:{chamber_temp:43,bed_temp:109,bed_target:110}};};
  await card.showPreheat(job);
  assert.equal(requests.length,1);assert.equal(requests[0].body,undefined,'Opening details is read-only');
  assert.match(html,/Unterbrochen/);assert.match(html,/43/);assert.match(html,/Erneut prüfen/);
  card.refresh=async()=>{};
  await card.editPreheat(job,'cancel');assert.equal(requests.length,2,'Confirmation reads the current owner/version without mutating');
  assert.match(html,/abbrechen/i);assert.equal(typeof submit,'function');
  await submit(new Map());await submit(new Map());
  assert.equal(requests[2].path,'preheat/'+job+'/cancel');
  assert.equal(requests[2].body.instance_id,'owner');assert.equal(requests[2].body.version,4);
  assert.equal(requests[2].body.request_key,requests[3].body.request_key,'An uncertain reply keeps the same command identity');
  await card.editPreheat(job,'extend');await submit(new Map());assert.equal(requests.at(-1).body.seconds,300);
  card.data.can_edit=false;
  await assert.rejects(()=>card.editPreheat(job,'skip'),/schreibgeschützt|Administrator/);
  await card.showPreheat(job);assert.doesNotMatch(html,/data-action="preheat-(cancel|skip|retry|extend)"/);
  card.data.can_edit=true;card.online=false;
  await assert.rejects(()=>card.editPreheat(job,'cancel'),/schreibgeschützt/);
  console.log('PASS preheat row, read-only details, stable command identity, permissions and preserved manual status');
})().catch(error=>{console.error(error);process.exitCode=1;});
