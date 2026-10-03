// Exercise accounting rendering and action boundaries without a browser backend.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const context={window:{},URL,crypto:require('node:crypto').webcrypto,HTMLElement:class{},customElements:{define(){}},location:{href:'http://localhost/'}};
vm.runInNewContext(fs.readFileSync('demo/custom_components/quack_material_demo/inventory.js','utf8')+';globalThis.Card=QuackInventoryCard;',context);
const card=Object.create(context.Card.prototype);
card.data={can_edit:true,revision:15,spools:[],customers:[{id:'c',name:'Customer'}],orders:[
  {id:'old',title:'Original order',customer_id:'c',status:'active',currency:'EUR'},
  {id:'target',order_number:'R-2',title:'Replacement order',customer_id:'c',status:'draft',currency:'EUR'},
  {id:'closed',title:'Finished order',status:'completed',currency:'EUR'},
  {id:'archived',title:'Old order',status:'active',archived:true,currency:'EUR'}
],jobs:[{uuid:'job',name:'Finished buttons',state:'completed',customer_order_uuid:'old'}]};
card.render=()=>{};
(async()=>{
  let submit,fields,requests=[];
  card.editor=(title,html,callback)=>{fields=html;submit=callback;};
  card.action=async(name,payload)=>{requests.push({name,payload});};
  await card.handle('assign-order','job');
  assert.ok(submit,'Completed prints must expose the order editor');
  assert.match(fields,/value="old" selected/);
  assert.match(fields,/value="target"/);
  assert.doesNotMatch(fields,/value="closed"|value="archived"/);
  const form=new Map([['customer_order_uuid','target']]);
  await submit(form);await submit(form);
  assert.equal(requests[0].name,'job_order');
  assert.equal(requests[0].payload.job_uuid,'job');
  assert.equal(requests[0].payload.expected_order_uuid,'old');
  assert.equal(requests[0].payload.customer_order_uuid,'target');
  assert.equal(requests[0].payload.revision,15);
  assert.equal(requests[0].payload.request_key,requests[1].payload.request_key,'Retry identity must remain stable');
  assert.deepEqual(Object.keys(requests[0].payload).sort(),['customer_order_uuid','expected_order_uuid','job_uuid','request_key','revision']);
  form.set('customer_order_uuid','');await submit(form);
  assert.equal(requests.at(-1).payload.customer_order_uuid,null);
  form.set('customer_order_uuid','missing');assert.throws(()=>submit(form));
  card.data.can_edit=false;submit=null;
  await card.handle('assign-order','job');assert.equal(submit,null,'Read-only clients cannot edit assignment');

  assert.equal(card.money(0,'USD'),new Intl.NumberFormat(undefined,{style:'currency',currency:'USD'}).format(0));
  const breakdown=card.costTable({currency:'USD',material_cost_micros:1000000,electricity_cost_micros:250000,
    machine_wear_cost_micros:0,maintenance_cost_micros:0,repair_reserve_cost_micros:0,design_cost_micros:0,
    other_cost_micros:0,total_cost_micros:1250000,billable_subtotal_micros:1000000,discount_micros:100000,
    calculated_invoice_micros:900000,quoted_price_micros:null,invoice_amount_micros:0});
  assert.ok(breakdown.includes(new Intl.NumberFormat(undefined,{style:'currency',currency:'USD'}).format(1.25)));
  assert.ok(breakdown.includes(new Intl.NumberFormat(undefined,{style:'currency',currency:'USD'}).format(0.90)));assert.doesNotMatch(breakdown,/€/);

  const invoice={id:'i1',created_at:'2026-10-03T20:00:00Z',order_uuid:'old',currency:'EUR',
    details:{seller_name:'<script>alert(1)</script>',seller_address:'Address\nCity',seller_contact:'contact',
      customer_name:'Buyer & Co',customer_address:'Buyer address',invoice_number:'INV-1',invoice_date:'2026-10-03',
      service_date:'2026-10-03',due_date:'2026-10-17',tax_identifier:'ID',small_business:true,vat_basis_points:0},
    lines:[{category:'material',description:'PLA <white>',detail:'1.986 g estimated',color_hex:'" onload="x',
      internal_amount_micros:40000,invoice_amount_micros:0,included:false}],
    totals:{internal_cost_micros:40000,net_micros:0,tax_micros:0,gross_micros:0}};
  assert.equal(typeof card.invoiceDocument,'function','Saved invoices need a printable export');
  const html=card.invoiceDocument(invoice);
  assert.match(html,/&lt;script&gt;/);assert.match(html,/Buyer &amp; Co/);assert.match(html,/Not billed/);
  assert.match(html,/1\.986 g estimated/);assert.match(html,/INV-1/);assert.match(html,/@media print/);
  assert.doesNotMatch(html,/<script|onload=|http:\/\/|https:\/\//i,'Export must be self-contained, escaped and script-free');
  assert.ok(html.includes(new Intl.NumberFormat(undefined,{style:'currency',currency:'EUR'}).format(0.04)));
  assert.ok(html.includes(new Intl.NumberFormat(undefined,{style:'currency',currency:'EUR'}).format(0)));
  assert.match(card.costAmount({summary:null,error:'Mixed <currencies>'},'total_cost_micros'),/Mixed &lt;currencies&gt;/);
  assert.doesNotMatch(card.costAmount({summary:null,error:'Mixed currencies'},'total_cost_micros'),/0[.,]00/,'Invalid aggregates cannot masquerade as zero costs');
  let onSaved,shown,reads=[];
  card.data.can_edit=true;
  card.api=async path=>{reads.push(path);return path.startsWith('accounting?invoice_uuid=')?invoice:{revision:21,
    order:{title:'Original order'},customer:{name:'Buyer & Co'},invoice_lines:invoice.lines,
    summary:{currency:'EUR',calculated_invoice_micros:0},invoice_defaults:{seller_name:'Saved issuer'}};};
  card.showSavedInvoice=value=>{shown=value;};
  await card.handle('open-invoice','i1');assert.equal(shown,invoice);
  card.editor=(title,html,callback,options)=>{fields=html;submit=callback;onSaved=options.onSaved;};
  const before=requests.length;
  await card.handle('create-invoice','old');
  assert.equal(requests.length,before,'Opening invoice form must not create an invoice');
  assert.match(fields,/value="Saved issuer"/);
  assert.match(fields,/value="">Choose tax treatment/,'Do not infer VAT treatment');
  const invoiceForm=new Map(Object.entries(invoice.details));invoiceForm.set('tax_treatment','vat');invoiceForm.set('vat_rate','19,00');
  card.action=async(name,payload)=>{requests.push({name,payload});return invoice;};
  const saved=await submit(invoiceForm);onSaved(saved);
  assert.equal(shown,invoice);assert.equal(requests.at(-1).name,'create_invoice');
  assert.equal(requests.at(-1).payload.revision,21,'Use the reviewed invoice revision, not old card data');
  assert.equal(requests.at(-1).payload.details.vat_basis_points,1900);
  assert.equal(requests.at(-1).payload.details.small_business,false);
  invoiceForm.set('tax_treatment','');await assert.rejects(()=>submit(invoiceForm));
  invoiceForm.set('tax_treatment','vat');invoiceForm.set('vat_rate','19.999');await assert.rejects(()=>submit(invoiceForm));
  const alert={textContent:''};card.editing=true;
  card.shadowRoot={querySelector:selector=>selector==='.sheet-error'?alert:null};
  card.api=async()=>{throw Error('HA connection interrupted');};
  await card.handle('create-invoice','old');
  assert.equal(alert.textContent,'HA connection interrupted','A rejected nested request must be visible in the open sheet');
  console.log('PASS completed-print reassignment, retry/revision guards, currencies and safe saved invoice export');
})().catch(error=>{console.error(error);process.exitCode=1;});
