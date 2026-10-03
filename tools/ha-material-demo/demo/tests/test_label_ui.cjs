const fs=require('fs'),vm=require('vm'),assert=require('assert');
const id='5b2d8b02-e906-4def-ba78-94465a775b66';
const controls={},box={querySelector(selector){return controls[selector]??=( {} );},remove(){this.removed=true;}};
const ctx={HTMLElement:class{},customElements:{define(){}},window:{},URL,crypto:require('crypto').webcrypto,document:{createElement(){return box;}}};
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/inventory.js','utf8')+';globalThis.Card=QuackInventoryCard;',ctx);
const card=Object.create(ctx.Card.prototype),calls=[];
card.data={spools:[{uuid:id,product:'Test roll'}]};card.shadowRoot={append(value){assert.equal(value,box);}};
card.api=async(path,data)=>{
  calls.push({path,...data});
  if(card.failNext){card.failNext=false;throw Error('Connection failed');}
  return {svg:'<svg/>',payload:(data.target==='app'?'homeassistant://navigate':'http://homeassistant.local:8123')+'/dashboard-filament/rolls?spool='+id};
};
(async()=>{
  await card.handle('label',id);
  assert.equal(calls[0].target,'app');assert.ok(box.innerHTML.includes('HA app'));assert.ok(box.innerHTML.includes('homeassistant://navigate'));
  await controls['[data-switch]'].onclick();
  assert.equal(calls[1].target,'web');assert.ok(box.innerHTML.includes('http://homeassistant.local:8123'));
  const previous=box.innerHTML;card.failNext=true;await controls['[data-switch]'].onclick();
  assert.equal(box.innerHTML,previous);assert.equal(controls['[data-error]'].textContent,'Connection failed');
  controls['[data-close]'].onclick();assert.equal(card.editing,false);assert.ok(box.removed);
  console.log('App default, browser alternative, failed-switch recovery and close pass');
})().catch(e=>{console.error(e);process.exitCode=1;});
