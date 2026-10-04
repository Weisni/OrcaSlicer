const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('demo/custom_components/quack_material_demo/slot_picker.js','utf8');
const types={}; const context={window:{},HTMLElement:class{},customElements:{define:(n,c)=>types[n]=c},crypto:require('crypto').webcrypto};
vm.runInNewContext(source,context);
const api=context.window.QuackSlots;
const rolls=[{uuid:'a',product:'ELEGOO PLA',manufacturer:'Elegoo',material_type:'PLA',color:'#ffffff',remaining_mg:120000,status:'active'},
 {...{uuid:'b',product:'ELEGOO PLA',manufacturer:'Elegoo',material_type:'PLA',color:'#000000',remaining_mg:70000,status:'active'}},
 {uuid:'c',product:'Old Elegoo',remaining_mg:1000,status:'archived'},
 {uuid:'d',product:'Empty Elegoo',remaining_mg:0,status:'active'}];
assert.deepEqual(Array.from(api.filterRolls(rolls,'eLe'),r=>r.uuid),['a','b']);
assert.equal(api.filterRolls(rolls,'#000000')[0].uuid,'b');
assert.equal(api.escape('<img onerror="x">'),'&lt;img onerror=&quot;x&quot;&gt;');
assert.equal(api.reason(rolls[0],'A1',[{id:'A2',spool_uuid:'a'}]),'Already assigned to A2');
assert.ok(api.reason({...rolls[0],material_type:'TPU'},'HT1',[]));
assert.equal(api.reason({...rolls[0],material_type:'TPU'},'EXT',[]),'');
assert.equal(api.loadedSlot({slots:{A1:'s1',A2:'s2'}},{s1:{state:'ok',attributes:{active:true,empty:false}},s2:{state:'ok',attributes:{active:false}}},'on'),'A1');
assert.equal(api.loadedSlot({slots:{A1:'s1',A2:'s2'}},{s1:{state:'ok',attributes:{active:true,empty:false}},s2:{state:'unavailable',attributes:{active:false}}},'on'),null);
assert.ok(types['quack-stock-slot-card']);assert.ok(types['quack-stock-picker']);
assert.deepEqual(Array.from(api.visibleSlots(['A1','A2','EXT'],{slots:[{id:'A1'},{id:'EXT'}]})),['A1','EXT']);
assert.deepEqual(Array.from(api.visibleSlots(['A1','A2'],{slots:[]})),[]);
console.log('Stock filtering, UUID distinction, compatibility, escaping and loaded-state policy pass');
(async()=>{
  const picker=Object.create(types['quack-stock-picker'].prototype);
  picker.slot='A1';picker.spoolUuid='a';picker.success='Old confirmed assignment';picker.render=()=>{};
  picker.data={revision:0,slots:[{id:'A1',revision:0}],spools:rolls};
  picker.hass={callApi:async method=>method==='POST'?{status:'pending'}:picker.data};
  await picker.submit(false);
  assert.equal(picker.success,'');assert.match(picker.error,/Not yet confirmed/);
  console.log('A previous successful assignment cannot hide the next pending result');
  picker.hass.callApi=async method=>method==='POST'?{status:'confirmed',color_status:'approximate',requested_color:'#FFFFFF',reported_color:'#C1C1C1'}:picker.data;
  await picker.submit(false);
  assert.equal(picker.error,'');assert.match(picker.success,/#FFFFFF/);assert.match(picker.success,/#C1C1C1/);
  console.log('Confirmed assignment discloses approximate printer color and preserves HA color');
})().catch(error=>{console.error(error);process.exitCode=1});
