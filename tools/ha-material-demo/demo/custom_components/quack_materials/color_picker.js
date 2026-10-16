/* Local camera/photo color sampling. Images never leave the browser. */
(function (root) {
  'use strict';
  const MAX_FILE_BYTES = 40 * 1024 * 1024;
  const finite = value => Number.isFinite(Number(value)) ? Number(value) : 0;
  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));

  function fitImage(width, height) {
    if (!Number.isFinite(width) || !Number.isFinite(height) || width < 1 || height < 1) throw new Error('Invalid image dimensions');
    if (width * height > 64000000 || Math.max(width, height) > 16384) throw new Error('Image too large');
    const ratio = Math.min(1, 1600 / Math.max(width, height));
    return {width:Math.max(1, Math.round(width * ratio)), height:Math.max(1, Math.round(height * ratio))};
  }

  function clampSelection(rect, width, height) {
    const originX = finite(rect.x), originY = finite(rect.y);
    const endX = originX + finite(rect.width), endY = originY + finite(rect.height);
    const x = clamp(Math.floor(Math.min(originX, endX)), 0, width - 1);
    const y = clamp(Math.floor(Math.min(originY, endY)), 0, height - 1);
    const right = clamp(Math.ceil(Math.max(originX, endX)), x + 1, width);
    const bottom = clamp(Math.ceil(Math.max(originY, endY)), y + 1, height);
    return {x, y, width:right - x, height:bottom - y};
  }

  function canvasPoint(event, bounds, width, height) {
    if (!(bounds.width > 0 && bounds.height > 0)) return null;
    return {x:clamp((event.clientX - bounds.left) * width / bounds.width, 0, width),
      y:clamp((event.clientY - bounds.top) * height / bounds.height, 0, height)};
  }

  function averageRegion(image, rect) {
    if (!image || !Number.isInteger(image.width) || !Number.isInteger(image.height) || image.width < 1 || image.height < 1 || image.data?.length !== image.width * image.height * 4) throw new Error('Invalid pixel data');
    const selection = clampSelection(rect, image.width, image.height);
    const sum = [0, 0, 0];
    let count = 0;
    for (let y = selection.y; y < selection.y + selection.height; y++) {
      for (let x = selection.x; x < selection.x + selection.width; x++) {
        const offset = (y * image.width + x) * 4;
        if (!image.data[offset + 3]) continue;
        for (let channel = 0; channel < 3; channel++) sum[channel] += image.data[offset + channel];
        count++;
      }
    }
    // Arithmetic mean of encoded sRGB channels; exclude fully transparent pixels.
    return count ? '#' + sum.map(value => Math.round(value / count).toString(16).padStart(2, '0')).join('').toUpperCase() : null;
  }

  function open(options = {}) {
    const doc = options.parent?.ownerDocument || root.document;
    const parent = options.parent || doc.body;
    const previousFocus = parent.activeElement || doc.activeElement;
    const modal = doc.createElement('div');
    modal.className = 'quack-color-picker';
    modal.innerHTML = `<style>
      .quack-color-picker{position:fixed;inset:0;z-index:10000;background:#0009;display:flex;align-items:center;justify-content:center;padding:12px;box-sizing:border-box;font:14px system-ui;color:var(--primary-text-color,#222);line-height:1.4}
      .quack-color-picker *{box-sizing:border-box}
      .quack-color-picker .panel{width:min(100%,680px);max-height:92vh;max-height:92svh;overflow:auto;border-radius:14px;background:var(--card-background-color,#fff);padding:18px;box-shadow:0 8px 40px #0006}
      .quack-color-picker header,.quack-color-picker footer,.quack-color-picker .tools,.quack-color-picker .sample{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
      .quack-color-picker header{justify-content:space-between;margin-bottom:12px}
      .quack-color-picker h2{font-size:18px;margin:0}
      .quack-color-picker button{font:inherit;padding:10px 13px;border:1px solid var(--divider-color,#bbb);border-radius:8px;background:var(--secondary-background-color,#f3f3f3);color:inherit;cursor:pointer}
      .quack-color-picker button:disabled{opacity:.45;cursor:default}
      .quack-color-picker button:focus-visible,.quack-color-picker canvas:focus-visible{outline:3px solid var(--primary-color,#2979ff);outline-offset:2px}
      .quack-color-picker [data-action=apply]{background:var(--primary-color,#2979ff);color:#fff;border:0}
      .quack-color-picker .preview-wrap{text-align:center;margin:12px 0;overflow:hidden}
      .quack-color-picker [data-preview]{display:inline-block;position:relative;max-width:100%;line-height:0;background:repeating-conic-gradient(#bbb 0 25%,#eee 0 50%) 0/16px 16px}
      .quack-color-picker canvas{display:block;max-width:100%;max-height:48vh;max-height:48svh;width:auto;height:auto;touch-action:none;cursor:crosshair}
      .quack-color-picker [data-selection]{position:absolute;pointer-events:none;border:2px solid #fff;outline:1px solid #000;box-shadow:0 0 0 1000px #0003}
      .quack-color-picker .swatch{width:42px;height:42px;border:1px solid #888;border-radius:8px}
      .quack-color-picker code{font:600 17px monospace}
      .quack-color-picker footer{justify-content:flex-end;margin-top:16px}
      .quack-color-picker .hint{font-size:12px;opacity:.8;margin:10px 0}
      .quack-color-picker [role=status]{min-height:20px;margin:10px 0}
      .quack-color-picker label{display:flex;gap:10px;align-items:center;margin:10px 0}
      .quack-color-picker input[type=range]{flex:1;min-width:100px}
      .quack-color-picker [hidden]{display:none!important}
    </style>
    <section class="panel" role="dialog" aria-modal="true" aria-label="Farbe aus Foto wählen">
      <header><h2>Farbe aus Foto wählen</h2><button type="button" data-action="close" aria-label="Schließen">✕</button></header>
      <div class="tools"><button type="button" data-action="camera">📷 Foto aufnehmen</button><button type="button" data-action="photo">🖼 Foto auswählen</button></div>
      <input hidden type="file" accept="image/*" capture="environment" data-source="camera">
      <input hidden type="file" accept="image/*" data-source="photo">
      <p class="hint">Das Foto wird nur auf diesem Gerät verarbeitet. Auf Geräten ohne Kamerazugriff bitte ein vorhandenes Foto auswählen.</p>
      <div class="preview-wrap" hidden><div data-preview><canvas tabindex="0" aria-label="Farbbereich im Foto durch Ziehen auswählen. Pfeiltasten verschieben den Bereich."></canvas><div data-selection></div></div></div>
      <label hidden data-size-label>Bereichsgröße <input type="range" min="1" max="100" value="20" data-size aria-label="Größe des ausgewählten Farbbereichs"></label>
      <p class="hint" data-help hidden>Im Foto einen Bereich ziehen. Der Mittelwert dieses Bereichs wird als Farbe übernommen. Für genaue Farben gleichmäßiges, neutrales Licht verwenden.</p>
      <div class="sample"><span class="swatch" data-swatch></span><code data-value="color"></code></div>
      <p role="status" aria-live="polite">Ein Foto aufnehmen oder auswählen.</p>
      <footer><button type="button" data-action="cancel">Abbrechen</button><button type="button" data-action="apply" disabled>Farbe übernehmen</button></footer>
    </section>`;
    parent.appendChild(modal);
    const find = selector => modal.querySelector(selector);
    const canvas = find('canvas');
    const selectionBox = find('[data-selection]');
    const status = find('[role=status]');
    const apply = find('[data-action=apply]');
    const slider = find('[data-size]');
    const listeners = [];
    const objectUrls = new Set();
    const pendingDecoders = new Set();
    let imageData = null, selection = null, sampled = null, drag = null, closed = false, generation = 0;
    const on = (element, event, handler) => {element.addEventListener(event, handler);listeners.push(()=>element.removeEventListener(event, handler));};
    const displayColor = color => {find('[data-value=color]').textContent = color || '–';find('[data-swatch]').style.backgroundColor = color || 'transparent';};
    displayColor(/^#[0-9a-f]{6}$/i.test(options.initialColor || '') ? options.initialColor.toUpperCase() : null);

    function releaseDrag() {
      if (drag && canvas.hasPointerCapture?.(drag.id)) canvas.releasePointerCapture(drag.id);
      drag = null;
    }
    function close() {
      if (closed) return;
      closed = true;generation++;
      releaseDrag();
      pendingDecoders.forEach(cancel=>cancel());
      objectUrls.forEach(url=>root.URL.revokeObjectURL(url));
      objectUrls.clear();
      listeners.forEach(remove=>remove());
      imageData = null;
      canvas.width = canvas.height = 0;
      modal.remove();
      previousFocus?.focus?.();
      options.onClosed?.();
    }
    function refresh() {
      if (!imageData || !selection) return;
      selection = clampSelection(selection, canvas.width, canvas.height);
      selectionBox.style.left = (selection.x / canvas.width * 100) + '%';
      selectionBox.style.top = (selection.y / canvas.height * 100) + '%';
      selectionBox.style.width = (selection.width / canvas.width * 100) + '%';
      selectionBox.style.height = (selection.height / canvas.height * 100) + '%';
      sampled = averageRegion(imageData, selection);
      displayColor(sampled);apply.disabled = !sampled;
      status.textContent = sampled ? `${selection.width} × ${selection.height} Bildpunkte ausgewählt.` : 'Der Bereich ist vollständig transparent. Bitte einen anderen Bereich wählen.';
    }
    async function decode(file) {
      if (typeof root.createImageBitmap === 'function') {
        try {return await root.createImageBitmap(file, {imageOrientation:'from-image'});} catch (_) { /* Browser image decoding is the fallback. */ }
      }
      if (closed) throw new Error('Image decoding cancelled');
      const url = root.URL.createObjectURL(file);
      objectUrls.add(url);
      let cancel;
      const image = new root.Image();
      try {
        const loaded = new Promise((resolve,reject)=>{
          image.onload=()=>resolve(image);
          image.onerror=()=>reject(new Error('Cannot decode image'));
          cancel=()=>{image.onload=null;image.onerror=null;image.src='';reject(new Error('Image decoding cancelled'));};
          pendingDecoders.add(cancel);
        });
        image.src = url;
        return await loaded;
      } finally {
        image.onload=null;image.onerror=null;
        pendingDecoders.delete(cancel);
        if(objectUrls.delete(url))root.URL.revokeObjectURL(url);
      }
    }
    async function loadPhoto(file) {
      if (!file || closed) return;
      const ticket = ++generation;
      releaseDrag();imageData=null;selection=null;sampled=null;apply.disabled=true;
      find('.preview-wrap').hidden=true;find('[data-size-label]').hidden=true;find('[data-help]').hidden=true;
      displayColor(null);
      if (file.size > MAX_FILE_BYTES) {status.textContent='Die Datei ist zu groß (maximal 40 MB). Bitte ein kleineres Foto wählen.';return;}
      if (file.type && !file.type.startsWith('image/')) {status.textContent='Bitte eine Bilddatei auswählen.';return;}
      status.textContent='Foto wird geladen …';
      let image;
      try {
        image = await decode(file);
        if (closed || ticket !== generation) return;
        const size = fitImage(image.naturalWidth || image.width, image.naturalHeight || image.height);
        canvas.width=size.width;canvas.height=size.height;
        const context = canvas.getContext('2d', {willReadFrequently:true});
        if (!context) throw new Error('Canvas unavailable');
        context.drawImage(image,0,0,size.width,size.height);
        imageData=context.getImageData(0,0,size.width,size.height);
        const width=Math.max(1,Math.round(size.width*.2)), height=Math.max(1,Math.round(size.height*.2));
        selection={x:Math.floor((size.width-width)/2),y:Math.floor((size.height-height)/2),width,height};slider.value=20;
        find('.preview-wrap').hidden=false;find('[data-size-label]').hidden=false;find('[data-help]').hidden=false;
        refresh();
      } catch (error) {
        if (!closed && ticket === generation) status.textContent = /large/.test(error.message) ? 'Das Foto ist zu groß. Bitte ein kleineres Bild wählen (maximal 64 Megapixel).' : 'Das Foto konnte nicht gelesen werden. Bitte JPG, PNG oder ein anderes unterstütztes Bild wählen.';
      } finally {image?.close?.();}
    }
    for (const source of ['camera','photo']) {
      const input=find(`[data-source=${source}]`);
      on(find(`[data-action=${source}]`),'click',()=>input.click());
      on(input,'change',()=>{const file=input.files?.[0];input.value='';void loadPhoto(file);});
    }
    for (const action of ['cancel','close']) on(find(`[data-action=${action}]`),'click',close);
    on(apply,'click',()=>{if (!sampled || closed || apply.disabled) return;try {options.onApply?.(sampled);} finally {close();}});
    on(modal,'click',event=>{if(event.target===modal)close();});
    on(modal,'keydown',event=>{
      if (event.key==='Escape') {event.preventDefault();event.stopPropagation();close();}
      if (event.key==='Tab') {
        const controls=Array.from(modal.querySelectorAll('button:not(:disabled), canvas, input[type=range]')).filter(node=>!node.closest('[hidden]') && (node!==canvas || imageData));
        const active=parent.activeElement || doc.activeElement;
        const index=controls.indexOf(active);
        if (event.shiftKey && index<=0) {event.preventDefault();controls.at(-1)?.focus();}
        else if (!event.shiftKey && (index<0 || index===controls.length-1)) {event.preventDefault();controls[0]?.focus();}
      }
    });
    on(canvas,'pointerdown',event=>{
      if (!imageData || (event.button!==undefined && event.button!==0)) return;
      const point=canvasPoint(event,canvas.getBoundingClientRect(),canvas.width,canvas.height);
      if (!point) return;
      event.preventDefault();canvas.focus();releaseDrag();drag={id:event.pointerId,start:point};
      canvas.setPointerCapture?.(event.pointerId);
      selection={x:point.x,y:point.y,width:1,height:1};refresh();
    });
    on(canvas,'pointermove',event=>{
      if (!drag || event.pointerId!==drag.id) return;
      const point=canvasPoint(event,canvas.getBoundingClientRect(),canvas.width,canvas.height);
      if (!point) return;
      selection={x:drag.start.x,y:drag.start.y,width:point.x-drag.start.x,height:point.y-drag.start.y};refresh();
    });
    for (const eventName of ['pointerup','pointercancel','lostpointercapture']) on(canvas,eventName,event=>{if(drag && event.pointerId===drag.id)releaseDrag();});
    on(slider,'input',()=>{
      if (!imageData) return;
      const ratio=Number(slider.value)/100, width=Math.max(1,Math.round(canvas.width*ratio)),height=Math.max(1,Math.round(canvas.height*ratio));
      const centerX=selection.x+selection.width/2,centerY=selection.y+selection.height/2;
      selection={x:clamp(Math.floor(centerX-width/2),0,canvas.width-width),y:clamp(Math.floor(centerY-height/2),0,canvas.height-height),width,height};refresh();
    });
    on(canvas,'keydown',event=>{
      if (!imageData || !['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key)) return;
      event.preventDefault();const step=event.shiftKey?10:1;
      selection.x=clamp(selection.x+(event.key==='ArrowLeft'?-step:event.key==='ArrowRight'?step:0),0,canvas.width-selection.width);
      selection.y=clamp(selection.y+(event.key==='ArrowUp'?-step:event.key==='ArrowDown'?step:0),0,canvas.height-selection.height);refresh();
    });
    find('[data-action=camera]').focus();
    return {close};
  }
  const api = {open, averageRegion, clampSelection, canvasPoint, fitImage};
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  if (root) root.QuackColorPicker = api;
})(typeof window === 'undefined' ? globalThis : window);
