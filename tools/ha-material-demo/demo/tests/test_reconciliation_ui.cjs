const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const context={window:{},URL,crypto:require('node:crypto').webcrypto,HTMLElement:class{},customElements:{define(){}},location:{href:'http://localhost/'}};
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/inventory.js','utf8')+';globalThis.Card=QuackInventoryCard;',context);
(async()=>{
  const card=Object.create(context.Card.prototype),roll='11111111-1111-4111-8111-111111111111';
  card.data={revision:8,spools:[{uuid:roll,product:'Real PLA'}],jobs:[{uuid:'job',name:'Failed print',can_reconcile_provider:true,observed_outcome:'failed',allocations:[{spool_uuid:roll},{spool_uuid:roll}]}]};
  let submit,requests=[];
  card.editor=(title,fields,callback)=>{
    assert.equal((fields.match(new RegExp('name="grams_'+roll+'"','g'))||[]).length,1,'One physical roll must receive one total, even across two filament indices');
    assert.doesNotMatch(fields,/name="spool_uuid"/,'Review must retain the original allocation identity');
    submit=callback;
  };
  card.action=async(action,payload)=>{requests.push({action,payload});};
  await card.handle('reconcile-provider','job');
  const form=new Map([['grams_'+roll,'1,986'],['quality','estimated'],['outcome','failed']]);
  await submit(form);await submit(form);
  assert.equal(requests[0].action,'reconcile_provider');
  assert.equal(requests[0].payload.consumption[roll],1986);
  assert.equal(requests[0].payload.request_key,requests[1].payload.request_key,'Retry must retain its durable identity');
  assert.equal(requests[0].payload.confirmed,true);
  form.set('grams_'+roll,'-1');assert.throws(()=>submit(form));
  console.log('PASS allocated-roll identity, decimal quantities and retry identity in reconciliation');
})().catch(error=>{console.error(error);process.exitCode=1;});
