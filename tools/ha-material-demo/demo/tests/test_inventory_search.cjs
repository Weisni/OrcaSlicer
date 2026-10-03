const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const context={window:{},URL,crypto:require('node:crypto').webcrypto,HTMLElement:class{},customElements:{define(){}},location:{href:'http://localhost/'}};
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/inventory.js','utf8')+';globalThis.Card=QuackInventoryCard;',context);

class Shadow {
  constructor(){this.activeElement=null;this.search=null;}
  set innerHTML(value){
    this.html=value;this.activeElement=null;
    const shadow=this;
    this.search={value:value.match(/class="search"[^>]*value="([^"]*)"/)[1],selectionStart:0,selectionEnd:0,selectionDirection:'none',
      focus(options){shadow.activeElement=this;this.focusOptions=options;},
      setSelectionRange(start,end,direction){this.selectionStart=start;this.selectionEnd=end;this.selectionDirection=direction;}};
  }
  get innerHTML(){return this.html;}
  querySelector(selector){return selector==='.search'?this.search:null;}
  querySelectorAll(){return [];}
}

(async()=>{
  const card=Object.create(context.Card.prototype);
  Object.assign(card,{view:'prints',query:'',message:'',shadowRoot:new Shadow(),data:{can_edit:true,spools:[],orders:[],jobs:[
    {uuid:'one',name:'Filzspiel',state:'completed',source:'quack_provider'},
    {uuid:'two',name:'Calibration cube',state:'completed',source:'quack_provider'}]}});
  card.render();
  const original=card.shadowRoot.search;
  original.focus();original.value='Filzspiel';original.setSelectionRange(2,6,'backward');
  assert.equal(typeof original.oninput,'function','Typing must filter before change/blur');
  original.oninput({target:original});
  assert.equal(card.query,'Filzspiel');
  assert.match(card.shadowRoot.innerHTML,/Filzspiel/);
  assert.doesNotMatch(card.shadowRoot.innerHTML,/Calibration cube/);
  let input=card.shadowRoot.search;
  assert.notEqual(input,original,'Test must exercise replacement during rendering');
  assert.equal(card.shadowRoot.activeElement,input);
  assert.deepEqual([input.selectionStart,input.selectionEnd,input.selectionDirection],[2,6,'backward']);
  assert.equal(input.focusOptions.preventScroll,true);

  let complete;
  card.api=()=>new Promise(resolve=>{complete=resolve;});
  const refresh=card.refresh();
  input.value='Calibration';input.setSelectionRange(5,5,'none');input.oninput({target:input});
  complete(card.data);await refresh;
  input=card.shadowRoot.search;
  assert.equal(input.value,'Calibration','Delayed periodic refresh must keep the latest typed query');
  assert.equal(card.shadowRoot.activeElement,input);
  assert.deepEqual([input.selectionStart,input.selectionEnd],[5,5]);
  assert.match(card.shadowRoot.innerHTML,/Calibration cube/);
  assert.doesNotMatch(card.shadowRoot.innerHTML,/Filzspiel/);

  card.shadowRoot.activeElement=null;card.render();
  assert.equal(card.shadowRoot.activeElement,null,'Refresh must not steal focus after search was left');
  input=card.shadowRoot.search;input.focus();input.value='';input.setSelectionRange(0,0);input.oninput({target:input});
  assert.match(card.shadowRoot.innerHTML,/Filzspiel/);assert.match(card.shadowRoot.innerHTML,/Calibration cube/);
  console.log('PASS live inventory search, delayed refresh, focus and selection preservation');
})().catch(error=>{console.error(error);process.exitCode=1;});
