import {
  BUTTON_ACTIONS,
  DISPLAY_HEIGHT,
  DISPLAY_WIDTH,
  GRID_COLUMNS,
  GRID_ROWS,
  addPage,
  applyButtonAction,
  canPlace,
  createDefaultProject,
  createWidget,
  deepClone,
  deletePage,
  dynamicScheduleForPage,
  findFreePosition,
  movePage,
  sanitizeProject,
} from './model.js';
import { BMP_SIZE, MARGINS, calculateLayout, canvasToMonoBmp, canvasToOneBitBmp, ditherImageData, renderPage, wrapText } from './render.js';
import { hydrateMaterialIcons, iconMarkup } from './material-icons.js';

export { BMP_SIZE, MARGINS, DISPLAY_HEIGHT, DISPLAY_WIDTH, calculateLayout, canvasToMonoBmp, canvasToOneBitBmp, wrapText };

const TYPE_LABELS = Object.freeze({ text: 'Texto', clock: 'Reloj', weather: 'Tiempo', calendar: 'Calendario', events: 'Eventos', device: 'Dispositivo', image: 'Imagen', qr: 'Código QR' });
const ACTION_LABELS = Object.freeze({ previous_page: 'Página anterior', next_page: 'Página siguiente', show_page: 'Mostrar una página', refresh: 'Refrescar', toggle_invert: 'Invertir', blank: 'Dejar en blanco', none: 'Sin acción' });
const LIVE_WIDGET_TYPES = new Set(['clock', 'calendar', 'events', 'weather']);
const WEATHER_REFRESH_MS = 60 * 60 * 1000;

let project = createDefaultProject();
let selectedWidgetId = null;
let currentView = 'design';
let undoStack = [];
let redoStack = [];
let saveTimer = null;
let saveRetryTimer = null;
let saveRevision = 0;
let savedRevision = 0;
let savePromise = null;
let renderToken = 0;
let runtime = { blank: false, inverted: false };
let displayJobActive = false;
let previewRefreshTimer = null;
let deviceTelemetry = null;
let deviceStatusTimer = null;
let spanishHolidays = [];
let physicalDevice = false;
let weatherRefreshTimer = null;
const weatherRequests = new Map();

const $ = (selector, parent = document) => parent.querySelector(selector);
const $$ = (selector, parent = document) => Array.from(parent.querySelectorAll(selector));
const activePage = () => project.pages.find((page) => page.id === project.activePageId) || project.pages[0];
const selectedWidget = () => activePage().widgets.find((widget) => widget.id === selectedWidgetId) || null;
const assetUrl = (assetId) => project.assets?.[assetId]?.data || `/api/assets/${encodeURIComponent(assetId)}`;

export function viewVisibility(view) {
  return { design: view === 'design', preview: view === 'preview' };
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>'"]/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[character]);
}

function toast(message) {
  const element = $('#toast');
  element.textContent = message;
  element.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => element.classList.remove('show'), 2600);
}

async function jsonResponse(response) {
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body.message || `Error HTTP ${response.status}`);
  return body;
}

async function sha256Hex(bytes) {
  if (globalThis.crypto?.subtle) {
    const digest = await crypto.subtle.digest('SHA-256', bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
    return Array.from(new Uint8Array(digest), (value) => value.toString(16).padStart(2, '0')).join('');
  }
  // El portal del dispositivo se sirve por HTTP y algunos navegadores ocultan
  // SubtleCrypto fuera de un contexto seguro. Este fallback mantiene el hash
  // por contenido también al configurar directamente desde 192.168.4.1.
  const constants = [
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
  ];
  const length = bytes.length;
  const paddedLength = Math.ceil((length + 9) / 64) * 64;
  const padded = new Uint8Array(paddedLength);
  padded.set(bytes); padded[length] = 0x80;
  const bitLength = length * 8;
  const view = new DataView(padded.buffer);
  view.setUint32(paddedLength - 8, Math.floor(bitLength / 0x100000000), false);
  view.setUint32(paddedLength - 4, bitLength >>> 0, false);
  const hash = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const words = new Uint32Array(64);
  const rotate = (value, amount) => (value >>> amount) | (value << (32 - amount));
  for (let offset = 0; offset < paddedLength; offset += 64) {
    for (let index = 0; index < 16; index += 1) words[index] = view.getUint32(offset + index * 4, false);
    for (let index = 16; index < 64; index += 1) {
      const s0 = rotate(words[index - 15], 7) ^ rotate(words[index - 15], 18) ^ (words[index - 15] >>> 3);
      const s1 = rotate(words[index - 2], 17) ^ rotate(words[index - 2], 19) ^ (words[index - 2] >>> 10);
      words[index] = (words[index - 16] + s0 + words[index - 7] + s1) >>> 0;
    }
    let [a,b,c,d,e,f,g,h] = hash;
    for (let index = 0; index < 64; index += 1) {
      const sum1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
      const choice = (e & f) ^ (~e & g);
      const t1 = (h + sum1 + choice + constants[index] + words[index]) >>> 0;
      const sum0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
      const majority = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (sum0 + majority) >>> 0;
      h=g; g=f; f=e; e=(d+t1)>>>0; d=c; c=b; b=a; a=(t1+t2)>>>0;
    }
    [a,b,c,d,e,f,g,h].forEach((value, index) => { hash[index] = (hash[index] + value) >>> 0; });
  }
  return Array.from(hash, (value) => value.toString(16).padStart(8, '0')).join('');
}

function crc32Hex(bytes) {
  let crc = 0xffffffff;
  for (const value of bytes) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return ((crc ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0');
}

async function syncPageFrames() {
  const agenda = { ...project.agenda, holidays: [...project.agenda.holidays.manual, ...spanishHolidays] };
  for (const page of project.pages) {
    const canvas = document.createElement('canvas');
    canvas.width = DISPLAY_WIDTH;
    canvas.height = DISPLAY_HEIGHT;
    await renderPage(canvas, project, page, {
      agenda,
      telemetry: deviceTelemetry,
      assetResolver: assetUrl,
      staticOnly: true,
      deferDisplayTransform: true,
    });
    const frame = canvasToMonoBmp(canvas, project.device.display.threshold);
    const frameId = await sha256Hex(frame);
    const response = await fetch(`/api/frames/${frameId}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'image/bmp' },
      body: frame,
    });
    if (!response.ok) await jsonResponse(response);
    page.frameId = frameId;
  }
}

async function loadProject() {
  let persistBrowserCopy = false;
  let rawProject = null;
  try {
    const response = await fetch('/api/project', { cache: 'no-store' });
    if (!response.ok) {
      persistBrowserCopy = response.status === 404;
      throw new Error(`Error HTTP ${response.status}`);
    }
    rawProject = await response.json();
    project = sanitizeProject(rawProject);
  } catch {
    try { rawProject = JSON.parse(localStorage.getItem('mieink-project') || 'null'); project = sanitizeProject(rawProject); }
    catch { project = createDefaultProject(); }
  }
  let migratedAssets = false;
  for (const [legacyId, legacy] of Object.entries(rawProject?.assets || {})) {
    if (!legacy?.data?.startsWith('data:image/')) continue;
    try {
      const response = await fetch(legacy.data);
      const uploaded = await uploadAsset(new File([await response.blob()], legacy.name || 'imagen', { type: legacy.mime || response.headers.get('content-type') || 'image/jpeg' }));
      project.assets[uploaded.id] = uploaded;
      for (const page of project.pages) for (const widget of page.widgets) {
        if (widget.type === 'image' && widget.config.assetId === legacyId) widget.config.assetId = uploaded.id;
      }
      migratedAssets = true;
    } catch { /* La configuración restante sigue siendo recuperable. */ }
  }
  const cached = project.agenda.holidays.spain.cached;
  const year = new Date().getFullYear();
  if (cached?.year === year && cached.region === project.agenda.holidays.spain.region) spanishHolidays = [...cached.holidays];
  if (persistBrowserCopy || migratedAssets) queueMicrotask(queueSave);
}

function mirrorProjectInBrowser() {
  try { localStorage.setItem('mieink-project', JSON.stringify(project)); }
  catch { /* LittleFS sigue siendo el almacenamiento principal. */ }
}

async function persistProjectNow({ retries = 2 } = {}) {
  clearTimeout(saveTimer);
  clearTimeout(saveRetryTimer);
  mirrorProjectInBrowser();
  if (savePromise) {
    await savePromise;
    if (savedRevision >= saveRevision) return true;
  }
  const targetRevision = saveRevision;
  savePromise = (async () => {
    let lastError;
    for (let attempt = 0; attempt <= retries; attempt += 1) {
      try {
        await syncPageFrames();
        const payload = JSON.stringify(project);
        await jsonResponse(await fetch('/api/project', {
          method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: payload,
        }));
        savedRevision = Math.max(savedRevision, targetRevision);
        $('#save-state').textContent = 'Guardado en el dispositivo';
        return true;
      } catch (error) {
        lastError = error;
        if (attempt < retries) await new Promise((resolve) => setTimeout(resolve, 500 * (attempt + 1)));
      }
    }
    $('#save-state').textContent = 'Pendiente de guardar en el dispositivo';
    throw lastError;
  })();
  try {
    return await savePromise;
  } finally {
    savePromise = null;
    if (savedRevision < saveRevision) {
      saveRetryTimer = setTimeout(() => persistProjectNow({ retries: 1 }).catch(() => {}), 3000);
    }
  }
}

function queueSave() {
  saveRevision += 1;
  mirrorProjectInBrowser();
  $('#save-state').textContent = 'Cambios pendientes';
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => persistProjectNow().catch(() => {}), 250);
}

function commit(mutator, options = {}) {
  if (options.history !== false) {
    undoStack.push(deepClone(project));
    if (undoStack.length > 50) undoStack.shift();
    redoStack = [];
  }
  mutator(project);
  project = sanitizeProject(project);
  renderAll();
  scheduleAutoRefresh();
  queueSave();
}

function historySwap(source, target) {
  if (!source.length) return;
  target.push(deepClone(project));
  project = source.pop();
  selectedWidgetId = null;
  runtime = { blank: false, inverted: false };
  renderAll();
  scheduleAutoRefresh();
  queueSave();
}

function widgetSummary(widget) {
  if (widget.type === 'text') return `<strong>${escapeHtml(widget.config.text || 'Texto')}</strong><small>Texto</small>`;
  if (widget.type === 'clock') return `<strong>${new Intl.DateTimeFormat('es-ES', { hour: '2-digit', minute: '2-digit' }).format(new Date())}</strong><small>Reloj y fecha</small>`;
  if (widget.type === 'weather') return `<strong>${widget.config.weather ? `${Math.round(widget.config.weather.temperature)}°` : '21°'}</strong><small>${escapeHtml(widget.config.location || 'Configura el tiempo')}</small>`;
  if (widget.type === 'calendar') return `<strong>${new Intl.DateTimeFormat('es-ES', { month: 'short' }).format(new Date())}</strong><small>Calendario</small>`;
  if (widget.type === 'events') return `<strong>${project.agenda.events.length || '0'}</strong><small>Eventos globales</small>`;
  if (widget.type === 'device') return `<strong>${deviceTelemetry?.network?.ssid || 'MiEInk'}</strong><small>${physicalDevice ? 'Controlador conectado' : 'Estado simulado'}</small>`;
  if (widget.type === 'image') return widget.config.assetId ? `<img src="${escapeHtml(assetUrl(widget.config.assetId))}" alt="" style="width:100%;height:100%;object-fit:${widget.config.fit || 'cover'}">` : `<strong>${iconMarkup('image')}</strong><small>Añade una imagen</small>`;
  return `<strong>${iconMarkup('qr')}</strong><small>${escapeHtml(widget.config.label || 'Código QR')}</small>`;
}

function renderPages() {
  const list = $('#page-list');
  list.innerHTML = project.pages.map((page, index) => `
    <div class="page-row ${page.id === project.activePageId ? 'active' : ''}" data-page-id="${escapeHtml(page.id)}">
      <span class="thumb">${index + 1}</span>
      <span class="page-label">${escapeHtml(page.name)}</span>
      <span class="page-move">
        <button type="button" data-move="-1" aria-label="Subir página" ${index === 0 ? 'disabled' : ''}>${iconMarkup('move_up')}</button>
        <button type="button" data-move="1" aria-label="Bajar página" ${index === project.pages.length - 1 ? 'disabled' : ''}>${iconMarkup('move_down')}</button>
      </span>
    </div>`).join('');
  $('#delete-page').disabled = project.pages.length === 1;
}

function renderCanvas() {
  const page = activePage();
  $('#project-name').value = project.name;
  $('#page-name').value = page.name;
  const margin = Number(project.device.display.safeMargin || 0);
  const safe = $('#safe-area');
  safe.style.inset = `${margin / DISPLAY_HEIGHT * 100}% ${margin / DISPLAY_WIDTH * 100}%`;
  $('#widget-layer').innerHTML = page.widgets.map((widget) => `
    <div class="canvas-widget ${widget.id === selectedWidgetId ? 'selected' : ''} ${widget.locked ? 'locked' : ''} ${widget.config.invert ? 'invert' : ''}"
      data-widget-id="${escapeHtml(widget.id)}"
      style="left:${widget.x / GRID_COLUMNS * 100}%;top:${widget.y / GRID_ROWS * 100}%;width:${widget.w / GRID_COLUMNS * 100}%;height:${widget.h / GRID_ROWS * 100}%">
      <div class="widget-content">${widgetSummary(widget)}</div>
      ${widget.locked ? '' : '<button class="resize-handle" type="button" aria-label="Redimensionar"></button>'}
    </div>`).join('');
  const design = $('#design-surface');
  const preview = $('#preview');
  const visibility = viewVisibility(currentView);
  design.hidden = !visibility.design;
  preview.hidden = !visibility.preview;
  $$('.view-switch button').forEach((button) => button.classList.toggle('active', button.dataset.view === currentView));
  if (currentView === 'preview') updatePreview();
}

function field(label, control) { return `<label class="field"><span>${label}</span>${control}</label>`; }
function select(value, options, attribute) {
  return `<select ${attribute}>${options.map(([key, label]) => `<option value="${escapeHtml(key)}" ${String(value) === String(key) ? 'selected' : ''}>${escapeHtml(label)}</option>`).join('')}</select>`;
}

function commonInspector(widget) {
  return `<h3>${TYPE_LABELS[widget.type]}</h3>
    <div class="field-row">
      ${field('Ancho', `<input type="number" min="1" max="${GRID_COLUMNS}" value="${widget.w}" data-geometry="w">`)}
      ${field('Alto', `<input type="number" min="1" max="${GRID_ROWS}" value="${widget.h}" data-geometry="h">`)}
    </div>
    ${field('Relleno', `<input type="number" min="0" max="80" value="${widget.config.padding ?? 12}" data-config="padding">`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="border" ${widget.config.border ? 'checked' : ''}> Mostrar borde</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="invert" ${widget.config.invert ? 'checked' : ''}> Invertir widget</label>`)}`;
}

function renderInspector() {
  const content = $('#inspector-content');
  const widget = selectedWidget();
  $('#duplicate-widget').disabled = !widget;
  $('#lock-widget').disabled = !widget;
  $('#delete-widget').disabled = !widget;
  $('#lock-widget').innerHTML = iconMarkup(widget?.locked ? 'unlock' : 'lock');
  $('#lock-widget').title = widget?.locked ? 'Desbloquear widget' : 'Bloquear widget';
  $('#lock-widget').setAttribute('aria-label', $('#lock-widget').title);
  if (!widget) {
    content.innerHTML = `<div class="inspector-empty"><div><b>Selecciona un widget</b><span>Sus opciones aparecerán aquí.<br>También puedes arrastrar una herramienta al lienzo.</span></div></div>`;
    return;
  }
  let specific = '';
  if (widget.type === 'text') specific = `
    ${field('Contenido', `<textarea data-config="text">${escapeHtml(widget.config.text)}</textarea>`)}
    <div class="field-row">
      ${field('Tamaño', `<input data-config="fontSize" value="${escapeHtml(widget.config.fontSize)}" placeholder="auto">`)}
      ${field('Peso', select(widget.config.weight, [['400','Normal'],['700','Negrita'],['900','Extra']], 'data-config="weight"'))}
    </div>
    ${field('Interlineado', `<input type="number" min="0.8" max="2" step="0.05" data-config="lineHeight" value="${widget.config.lineHeight || 1.08}">`)}
    ${field('Alineación', select(widget.config.align, [['left','Izquierda'],['center','Centro'],['right','Derecha']], 'data-config="align"'))}
    ${field('Alineación vertical', select(widget.config.verticalAlign, [['top','Arriba'],['middle','Centro'],['bottom','Abajo']], 'data-config="verticalAlign"'))}`;
  if (widget.type === 'clock') specific = `
    ${field('Formato', select(widget.config.format, [['24','24 horas'],['12','12 horas']], 'data-config="format"'))}
    ${field('Zona horaria', `<input data-config="timezone" value="${escapeHtml(widget.config.timezone || '')}" placeholder="Local · Europe/Madrid">`)}
    ${field('Fecha', select(widget.config.dateStyle || 'short', [['short','Corta'],['long','Larga']], 'data-config="dateStyle"'))}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showTime" ${widget.config.showTime !== false ? 'checked' : ''}> Mostrar hora</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showWeekday" ${widget.config.showWeekday !== false ? 'checked' : ''}> Mostrar día de la semana</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showDate" ${widget.config.showDate !== false ? 'checked' : ''}> Mostrar fecha</label>`)}`;
  if (widget.type === 'weather') specific = `
    ${field('Ubicación', `<input data-config="location" value="${escapeHtml(widget.config.location)}" placeholder="Madrid, Barcelona…">`)}
    ${field('Título', `<input data-config="title" value="${escapeHtml(widget.config.title || '')}" placeholder="Tiempo">`)}
    <div class="field-row">
      ${field('Unidades', select(widget.config.units, [['celsius','Celsius'],['fahrenheit','Fahrenheit']], 'data-config="units"'))}
      ${field('Vista', select(widget.config.view, [['current','Actual'],['forecast','Previsión']], 'data-config="view"'))}
    </div>
    ${field('Días de previsión', select(widget.config.forecastDays || 3, [['1','1 día'],['3','3 días'],['5','5 días']], 'data-config="forecastDays"'))}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showTitle" ${widget.config.showTitle ? 'checked' : ''}> Mostrar título</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showLocation" ${widget.config.showLocation === true ? 'checked' : ''}> Mostrar ubicación</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showWind" ${widget.config.showWind ? 'checked' : ''}> Mostrar viento</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showFeelsLike" ${widget.config.showFeelsLike ? 'checked' : ''}> Sensación térmica</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showHumidity" ${widget.config.showHumidity ? 'checked' : ''}> Humedad</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showPrecipitation" ${widget.config.showPrecipitation ? 'checked' : ''}> Precipitación</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showCloudCover" ${widget.config.showCloudCover ? 'checked' : ''}> Nubosidad</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showSun" ${widget.config.showSun ? 'checked' : ''}> Amanecer y atardecer</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showIcon" ${widget.config.showIcon !== false ? 'checked' : ''}> Icono del tiempo</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showLabels" ${widget.config.showLabels ? 'checked' : ''}> Etiquetas junto a los iconos</label>`)}
    <p class="field-note">OpenWeather se consulta como máximo una vez cada hora. Los redibujados intermedios reutilizan la última lectura.</p>
    <button class="inspector-button" type="button" data-inspector-action="weather">${iconMarkup('refresh')}<span>Buscar y actualizar</span></button>`;
  if (widget.type === 'calendar') specific = `
    ${field('Vista', select(widget.config.view, [['month','Mes físico'],['week','Semana']], 'data-config="view"'))}
    ${field('Fecha de referencia', `<input type="date" data-config="referenceDate" value="${escapeHtml(widget.config.referenceDate || '')}">`)}
    ${field('Semana', select(widget.config.weekMode || 'full', [['full','Lunes a domingo'],['work','Lunes a viernes']], 'data-config="weekMode"'))}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showHeader" ${widget.config.showHeader !== false ? 'checked' : ''}> Cabecera de mes/semana</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showWeekNumbers" ${widget.config.showWeekNumbers ? 'checked' : ''}> Números de semana</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showEventMarkers" ${widget.config.showEventMarkers !== false ? 'checked' : ''}> Marcas de eventos</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showEventLabels" ${widget.config.showEventLabels ? 'checked' : ''}> Nombre del primer evento</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showHolidayLabels" ${widget.config.showHolidayLabels !== false ? 'checked' : ''}> Etiquetas de festivos</label>`)}
    <button class="inspector-button" type="button" data-inspector-action="agenda">Editar agenda y festivos</button>`;
  if (widget.type === 'events') specific = `
    ${field('Vista', select(widget.config.view, [['agenda','Agenda'],['day','Día por horas']], 'data-config="view"'))}
    ${field('Fecha de referencia', `<input type="date" data-config="referenceDate" value="${escapeHtml(widget.config.referenceDate || '')}">`)}
    <div class="field-row">${field('Rango agenda', `<input type="number" min="1" max="90" data-config="rangeDays" value="${widget.config.rangeDays || 14}">`)}${field('Máximo', `<input type="number" min="1" max="20" data-config="maxEvents" value="${widget.config.maxEvents || 6}">`)}</div>
    <div class="field-row">${field('Desde', `<input type="number" min="0" max="23" data-config="dayStart" value="${widget.config.dayStart ?? 8}">`)}${field('Hasta', `<input type="number" min="1" max="24" data-config="dayEnd" value="${widget.config.dayEnd ?? 20}">`)}</div>
    ${field('Intervalo', select(widget.config.interval || 60, [['30','30 min'],['60','60 min'],['120','120 min']], 'data-config="interval"'))}
    <button class="inspector-button" type="button" data-inspector-action="agenda">Editar agenda y festivos</button>`;
  if (widget.type === 'device') specific = `
    ${field('Modo', select(widget.config.mode || 'compact', [['compact','Compacto'],['detailed','Detallado']], 'data-config="mode"'))}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showBattery" ${widget.config.showBattery !== false ? 'checked' : ''}> Batería y carga</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showNetwork" ${widget.config.showNetwork !== false ? 'checked' : ''}> Wi‑Fi, señal e IP</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showRefresh" ${widget.config.showRefresh !== false ? 'checked' : ''}> Último refresco</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showUptime" ${widget.config.showUptime ? 'checked' : ''}> Uptime</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showFirmware" ${widget.config.showFirmware ? 'checked' : ''}> Firmware</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showTemperature" ${widget.config.showTemperature ? 'checked' : ''}> Temperatura</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showErrors" ${widget.config.showErrors !== false ? 'checked' : ''}> Avisos</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showNetworkName" ${widget.config.showNetworkName ? 'checked' : ''}> Nombre de red</label>`)}
    ${field('', `<label class="check-row"><input type="checkbox" data-config="showLabels" ${widget.config.showLabels ? 'checked' : ''}> Etiquetas junto a los iconos</label>`)}
    <button class="inspector-button" type="button" data-inspector-action="device-status">Ver estado en directo</button>`;
  if (widget.type === 'image') specific = `
    ${field('Archivo', '<input type="file" accept="image/png,image/jpeg,image/webp" data-image-upload>')}
    <div class="field-row">
      ${field('Ajuste', select(widget.config.fit, [['cover','Rellenar'],['contain','Encajar']], 'data-config="fit"'))}
      ${field('Dithering', select(widget.config.dither || 'floyd', [['floyd','Floyd–Steinberg'],['bayer','Bayer 4×4'],['threshold','Umbral puro']], 'data-config="dither"'))}
    </div>
    <div class="field-row">
      ${field('Umbral', `<input type="number" min="0" max="255" value="${widget.config.threshold || 160}" data-config="threshold">`)}
      ${field('Rotación', select(widget.config.rotation || 0, [['0','0°'],['90','90°'],['180','180°'],['270','270°']], 'data-config="rotation"'))}
    </div>
    <div class="field-row">
      ${field('Brillo', `<input type="number" min="-100" max="100" value="${widget.config.brightness || 0}" data-config="brightness">`)}
      ${field('Contraste', `<input type="number" min="-100" max="100" value="${widget.config.contrast || 0}" data-config="contrast">`)}
    </div>
    <div class="field-row">
      ${field('Gamma', `<input type="number" min="0.1" max="3" step="0.1" value="${widget.config.gamma || 1}" data-config="gamma">`)}
      ${field('Foco X', `<input type="number" min="0" max="100" value="${widget.config.focusX ?? 50}" data-config="focusX">`)}
    </div>
    ${field('Foco Y', `<input type="number" min="0" max="100" value="${widget.config.focusY ?? 50}" data-config="focusY">`)}`;
  if (widget.type === 'qr') specific = `
    ${field('Contenido', `<textarea data-config="value">${escapeHtml(widget.config.value)}</textarea>`)}
    ${field('Etiqueta', `<input maxlength="40" data-config="label" value="${escapeHtml(widget.config.label)}">`)}
    <div class="field-row">
      ${field('Corrección', select(widget.config.correction, [['L','Baja'],['M','Media'],['Q','Alta'],['H','Máxima']], 'data-config="correction"'))}
      ${field('Margen', `<input type="number" min="0" max="8" data-config="margin" value="${widget.config.margin}">`)}
    </div>`;
  content.innerHTML = commonInspector(widget) + specific;
}

function buttonText(button) {
  if (button.action === 'show_page') return project.pages.find((page) => page.id === button.pageId)?.name || ACTION_LABELS.show_page;
  return ACTION_LABELS[button.action] || button.action;
}

function renderVirtualKeys() {
  $$('.virtual-keys button').forEach((element) => {
    const button = project.device.buttons[element.dataset.key];
    $('small', element).textContent = buttonText(button);
  });
}

function renderAll() {
  renderPages();
  renderCanvas();
  renderInspector();
  renderVirtualKeys();
  $('#undo').disabled = !undoStack.length;
  $('#redo').disabled = !redoStack.length;
  $('#runtime-status').textContent = runtime.blank ? 'Pantalla en blanco' : runtime.inverted ? 'Vista invertida' : activePage().name;
  updateRefreshIndicator();
}

async function updatePreview() {
  const token = ++renderToken;
  const canvas = $('#preview');
  const agenda = { ...project.agenda, holidays: [...project.agenda.holidays.manual, ...spanishHolidays] };
  await renderPage(canvas, project, activePage(), { runtimeInvert: runtime.inverted, blank: runtime.blank, agenda, telemetry: deviceTelemetry, assetResolver: assetUrl });
  if (token !== renderToken) return;
}

function statusCard(icon, title, value, detail = '') {
  return `<div class="status-card">${iconMarkup(icon)}<div><small>${escapeHtml(title)}</small><strong>${escapeHtml(value)}</strong>${detail ? `<small>${escapeHtml(detail)}</small>` : ''}</div></div>`;
}

function renderDeviceStatus() {
  const target = $('#device-status-cards');
  if (!target) return;
  if (!deviceTelemetry) { target.innerHTML = '<p class="privacy-note">Cargando estado del dispositivo…</p>'; return; }
  const power = deviceTelemetry.power || {};
  const network = deviceTelemetry.network || {};
  const display = deviceTelemetry.display || {};
  const system = deviceTelemetry.system || {};
  target.innerHTML = [
    statusCard('battery', 'Energía', power.percentage == null ? 'No disponible' : `${power.percentage}%${power.charging ? ' · cargando' : ''}`, power.voltage == null ? '' : `${power.voltage.toFixed(2)} V`),
    statusCard('wifi', 'Red', network.ssid || 'Sin red', `${network.ip || '—'} · ${network.rssi ?? '—'} dBm`),
    statusCard('screen', 'Pantalla', display.state || '—', `${display.refreshCount || 0} refrescos · ${display.nextRefreshReason || 'sin vencimiento'}`),
    statusCard('chip', 'Sistema', system.firmware || '—', `${system.uptimeLabel || '—'}${system.temperatureC == null ? '' : ` · ${Math.round(system.temperatureC)} °C`}`),
  ].join('');
}

async function loadDeviceStatus() {
  try {
    deviceTelemetry = await jsonResponse(await fetch('/api/device/status', { cache: 'no-store' }));
    renderDeviceStatus();
    renderAll();
  } catch (error) {
    deviceTelemetry = null;
    renderDeviceStatus();
  }
}

async function detectRuntime() {
  const badge = $('#runtime-mode');
  const label = $('#send-label');
  try {
    const status = await jsonResponse(await fetch('/api/status', { cache: 'no-store' }));
    physicalDevice = status.simulation !== true;
    badge.innerHTML = physicalDevice ? '<i></i> Controlador MiEInk conectado' : '<i></i> Simulación local';
    badge.classList.toggle('is-hardware', physicalDevice);
    label.textContent = physicalDevice ? 'Actualizar pantalla física' : 'Simular pantalla';
  } catch {
    physicalDevice = false;
    badge.innerHTML = '<i></i> Modo local sin conexión';
    label.textContent = 'Simular pantalla';
  }
}

function startDeviceStatusPolling() {
  clearInterval(deviceStatusTimer);
  loadDeviceStatus();
  deviceStatusTimer = setInterval(loadDeviceStatus, 5000);
}

function stopDeviceStatusPolling() {
  clearInterval(deviceStatusTimer);
  deviceStatusTimer = null;
}

function renderAgendaDialog() {
  const events = project.agenda.events;
  $('#agenda-event-list').innerHTML = events.length ? events.map((event) => `<div class="agenda-row" data-agenda-event="${escapeHtml(event.id)}"><input type="date" value="${escapeHtml(event.date)}" data-agenda-field="date"><input type="time" value="${escapeHtml(event.start || '')}" data-agenda-field="start" title="Inicio"><input type="time" value="${escapeHtml(event.end || '')}" data-agenda-field="end" title="Fin"><label title="Todo el día"><input type="checkbox" data-agenda-field="allDay" ${event.allDay ? 'checked' : ''}> Día</label><input value="${escapeHtml(event.title)}" data-agenda-field="title" placeholder="Evento"><button type="button" data-remove-agenda-event="${escapeHtml(event.id)}" aria-label="Eliminar evento">${iconMarkup('delete')}</button></div>`).join('') : '<p class="section-help">Todavía no hay eventos.</p>';
  const holidays = project.agenda.holidays.manual;
  $('#agenda-holiday-list').innerHTML = holidays.length ? holidays.map((holiday) => `<div class="holiday-row" data-holiday="${escapeHtml(holiday.id)}"><input type="date" value="${escapeHtml(holiday.date)}" data-holiday-field="date"><input value="${escapeHtml(holiday.title)}" data-holiday-field="title" placeholder="Festivo"><input value="${escapeHtml(holiday.scope)}" data-holiday-field="scope" placeholder="Ámbito"><button type="button" data-remove-holiday="${escapeHtml(holiday.id)}" aria-label="Eliminar festivo">${iconMarkup('delete')}</button></div>`).join('') : '<p class="section-help">Añade festivos locales o personalizados.</p>';
  $('#spain-holidays-enabled').checked = project.agenda.holidays.spain.enabled;
  $('#spain-holiday-region').value = project.agenda.holidays.spain.region;
}

function openAgendaDialog() {
  renderAgendaDialog();
  $('#agenda-dialog').showModal();
  if (project.agenda.holidays.spain.enabled && !spanishHolidays.length) refreshSpainHolidays();
}

async function refreshSpainHolidays({ force = false } = {}) {
  const status = $('#holiday-status');
  if (!project.agenda.holidays.spain.enabled) { spanishHolidays = []; status.textContent = 'Activa la consulta para incluir festivos nacionales o autonómicos.'; renderAll(); return; }
  const year = new Date().getFullYear();
  const region = project.agenda.holidays.spain.region;
  const cached = project.agenda.holidays.spain.cached;
  if (!force && cached?.year === year && cached.region === region && cached.holidays?.length) {
    spanishHolidays = [...cached.holidays];
    status.textContent = `${spanishHolidays.length} festivos cargados desde la memoria del dispositivo.`;
    renderAll();
    return;
  }
  try {
    status.textContent = 'Consultando festivos…';
    const query = new URLSearchParams({ year: String(year), ...(region ? { region } : {}), ...(force ? { force: '1' } : {}) });
    const data = await jsonResponse(await fetch(`/api/integrations/holidays?${query}`, { cache: 'no-store' }));
    spanishHolidays = data.holidays || [];
    commit((draft) => {
      draft.agenda.holidays.spain.cached = {
        year, region, fetchedAt: new Date().toISOString(), holidays: spanishHolidays,
      };
    }, { history: false });
    status.textContent = `${spanishHolidays.length} festivos guardados en el dispositivo para ${year}.`;
  } catch (error) {
    spanishHolidays = cached?.year === year && cached.region === region ? [...cached.holidays] : [];
    status.textContent = spanishHolidays.length
      ? `Sin conexión con festivos; se mantienen ${spanishHolidays.length} guardados en el dispositivo.`
      : `No se pudieron cargar: ${error.message}. Los manuales siguen disponibles.`;
    if (currentView === 'preview') updatePreview();
  }
}

function activePageNeedsLiveRefresh() {
  return activePage().widgets.some((widget) => LIVE_WIDGET_TYPES.has(widget.type));
}

function refreshVisibleLiveWidgets() {
  if (!activePageNeedsLiveRefresh()) return;
  if (currentView === 'preview') {
    updatePreview();
    return;
  }
  // En Diseño no se reconstruye el editor: solo cambian los resúmenes de los
  // widgets que dependen de la hora o la fecha.
  activePage().widgets.filter((widget) => LIVE_WIDGET_TYPES.has(widget.type)).forEach((widget) => {
    const content = $(`.canvas-widget[data-widget-id="${widget.id}"] .widget-content`);
    if (content) content.innerHTML = widgetSummary(widget);
  });
}

function updateRefreshIndicator() {
  const element = $('#refresh-state');
  const schedule = dynamicScheduleForPage(activePage());
  if (!schedule) {
    element.textContent = 'Sin refrescos dinámicos';
    return;
  }
  const controllerAt = deviceTelemetry?.display?.nextRefreshAt;
  const when = controllerAt ? new Date(controllerAt) : schedule.at;
  const reasons = deviceTelemetry?.display?.nextRefreshReason || schedule.reasons.join(', ');
  element.textContent = `Controlador · ${reasons} · ${new Intl.DateTimeFormat('es-ES', { hour: '2-digit', minute: '2-digit' }).format(when)}`;
}

function scheduleAutoRefresh() {
  clearTimeout(previewRefreshTimer);
  const schedule = dynamicScheduleForPage(activePage());
  if (!schedule) {
    updateRefreshIndicator();
    return;
  }
  const tick = () => {
    refreshVisibleLiveWidgets();
    updateRefreshIndicator();
    scheduleAutoRefresh();
  };
  updateRefreshIndicator();
  previewRefreshTimer = setTimeout(tick, Math.max(250, schedule.at.getTime() - Date.now() + 100));
}

function addWidget(type, position = null) {
  const page = activePage();
  const widget = createWidget(type, page, position || {});
  if (!widget) {
    toast('No queda espacio libre para esa herramienta.');
    return;
  }
  commit(() => page.widgets.push(widget));
  selectedWidgetId = widget.id;
  renderAll();
}

function gridPosition(event) {
  const rect = $('#design-surface').getBoundingClientRect();
  return {
    x: Math.max(0, Math.min(GRID_COLUMNS - 1, Math.floor((event.clientX - rect.left) / rect.width * GRID_COLUMNS))),
    y: Math.max(0, Math.min(GRID_ROWS - 1, Math.floor((event.clientY - rect.top) / rect.height * GRID_ROWS))),
  };
}

function showGuide(rect, valid) {
  const guide = $('#drop-guide');
  guide.hidden = false;
  guide.classList.toggle('invalid', !valid);
  guide.style.left = `${rect.x / GRID_COLUMNS * 100}%`;
  guide.style.top = `${rect.y / GRID_ROWS * 100}%`;
  guide.style.width = `${rect.w / GRID_COLUMNS * 100}%`;
  guide.style.height = `${rect.h / GRID_ROWS * 100}%`;
}

function hideGuide() { $('#drop-guide').hidden = true; }

function beginPointerEdit(event, widget, resize) {
  if (widget.locked) return;
  event.preventDefault();
  selectedWidgetId = widget.id;
  renderAll();
  const surface = $('#design-surface').getBoundingClientRect();
  const start = { clientX: event.clientX, clientY: event.clientY, x: widget.x, y: widget.y, w: widget.w, h: widget.h };
  let candidate = { x: widget.x, y: widget.y, w: widget.w, h: widget.h };
  let valid = true;
  const move = (moveEvent) => {
    const dx = Math.round((moveEvent.clientX - start.clientX) / surface.width * GRID_COLUMNS);
    const dy = Math.round((moveEvent.clientY - start.clientY) / surface.height * GRID_ROWS);
    candidate = resize
      ? { x: start.x, y: start.y, w: Math.max(1, start.w + dx), h: Math.max(1, start.h + dy) }
      : { x: start.x + dx, y: start.y + dy, w: start.w, h: start.h };
    valid = canPlace(activePage(), candidate, widget.id);
    showGuide(candidate, valid);
  };
  const end = () => {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', end);
    hideGuide();
    if (valid && (candidate.x !== start.x || candidate.y !== start.y || candidate.w !== start.w || candidate.h !== start.h)) {
      commit(() => Object.assign(widget, candidate));
    }
  };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', end, { once: true });
}

function configValue(input) {
  if (input.type === 'checkbox') return input.checked;
  if (input.type === 'number' || input.type === 'range') return Number(input.value);
  return input.value;
}

function weatherIsStale(widget, now = Date.now()) {
  const fetchedAt = Date.parse(widget.config.weather?.fetchedAt || '');
  return !Number.isFinite(fetchedAt) || now - fetchedAt >= WEATHER_REFRESH_MS;
}

async function requestWeather(latitude, longitude, units, days) {
  const key = [Number(latitude).toFixed(4), Number(longitude).toFixed(4), units, days].join(':');
  if (weatherRequests.has(key)) return weatherRequests.get(key);
  const query = new URLSearchParams({ latitude: String(latitude), longitude: String(longitude), units, days: String(days) });
  const request = jsonResponse(await fetch(`/api/integrations/weather?${query}`, { cache: 'no-store' }))
    .finally(() => weatherRequests.delete(key));
  weatherRequests.set(key, request);
  return request;
}

async function refreshWeather(widget, { force = true, silent = false } = {}) {
  const location = String(widget.config.location || '').trim();
  if (!location) { if (!silent) toast('Escribe una ubicación.'); return false; }
  if (!force && !weatherIsStale(widget)) return false;
  try {
    if (!silent) toast('Consultando OpenWeather…');
    let latitude = Number(widget.config.latitude);
    let longitude = Number(widget.config.longitude);
    let resolvedLocation = String(widget.config.resolvedLocation || '');
    let canonicalLocation = location;
    if (!Number.isFinite(latitude) || !Number.isFinite(longitude) || resolvedLocation !== location) {
      const places = await jsonResponse(await fetch(`/api/integrations/geocode?q=${encodeURIComponent(location)}`, { cache: 'no-store' }));
      if (!places.results?.length) throw new Error('No se encontró esa ubicación.');
      const place = places.results[0];
      latitude = place.latitude;
      longitude = place.longitude;
      canonicalLocation = [place.name, place.state, place.country].filter(Boolean).join(', ');
      resolvedLocation = canonicalLocation;
    }
    const days = Math.max(1, Math.min(5, Number(widget.config.forecastDays || 3)));
    const weather = await requestWeather(latitude, longitude, widget.config.units, days);
    commit(() => {
      widget.config.location = canonicalLocation;
      widget.config.resolvedLocation = resolvedLocation;
      widget.config.latitude = latitude;
      widget.config.longitude = longitude;
      widget.config.forecastDays = days;
      widget.config.weather = weather;
    }, { history: false });
    if (!silent) toast(weather.cached ? 'Tiempo actualizado desde la caché horaria.' : 'Tiempo actualizado con OpenWeather.');
    return true;
  } catch (error) {
    if (!silent) toast(error.message || 'No se pudo consultar el tiempo.');
    return false;
  }
}

async function refreshStaleWeather() {
  const targets = project.pages.flatMap((page) => page.widgets.filter((widget) => widget.type === 'weather' && widget.config.location).map((widget) => ({ pageId: page.id, widgetId: widget.id })));
  for (const target of targets) {
    const widget = project.pages.find((page) => page.id === target.pageId)?.widgets.find((item) => item.id === target.widgetId);
    if (widget) await refreshWeather(widget, { force: false, silent: true });
  }
}

async function uploadAsset(file) {
  if (!file || !file.type.startsWith('image/')) throw new Error('Selecciona una imagen válida.');
  if (file.size > 12 * 1024 * 1024) throw new Error('La imagen no puede superar 12 MB.');
  const objectUrl = URL.createObjectURL(file);
  try {
    const image = await new Promise((resolve, reject) => {
      const element = new Image();
      element.onload = () => resolve(element);
      element.onerror = () => reject(new Error('No se pudo leer esa imagen.'));
      element.src = objectUrl;
    });
    const sourceWidth = image.naturalWidth || image.width;
    const sourceHeight = image.naturalHeight || image.height;
    const scale = Math.min(1, DISPLAY_WIDTH / sourceWidth, DISPLAY_HEIGHT / sourceHeight);
    const width = Math.max(1, Math.round(sourceWidth * scale));
    const height = Math.max(1, Math.round(sourceHeight * scale));
    const canvas = document.createElement('canvas');
    canvas.width = width; canvas.height = height;
    const context = canvas.getContext('2d', { willReadFrequently: true });
    context.fillStyle = '#fff'; context.fillRect(0, 0, width, height);
    context.drawImage(image, 0, 0, width, height);
    const imageData = context.getImageData(0, 0, width, height);
    ditherImageData(imageData.data, width, height, { dither: 'floyd', threshold: project.device.display.threshold });
    context.putImageData(imageData, 0, 0);
    const bmp = canvasToOneBitBmp(canvas, project.device.display.threshold);
    const id = await sha256Hex(bmp);
    const asset = await jsonResponse(await fetch(`/api/assets/${id}`, {
      method: 'POST',
      headers: { 'Content-Type': 'image/bmp', 'X-File-Name': encodeURIComponent(file.name) },
      body: bmp,
    }));
    return {
      ...asset,
      id,
      name: String(file.name || 'imagen').slice(0, 120),
      mime: 'image/bmp',
      format: 'mono1',
      width,
      height,
      size: bmp.byteLength,
      crc32: crc32Hex(bmp),
      url: `/api/assets/${id}`,
    };
  } finally {
    URL.revokeObjectURL(objectUrl);
  }
}

function bytesToDataUrl(buffer, mime) {
  let binary = '';
  const bytes = new Uint8Array(buffer);
  for (let index = 0; index < bytes.length; index += 0x8000) binary += String.fromCharCode(...bytes.subarray(index, index + 0x8000));
  return `data:${mime};base64,${btoa(binary)}`;
}

async function exportProject() {
  const embeddedAssets = {};
  for (const [assetId, metadata] of Object.entries(project.assets)) {
    if (metadata.data?.startsWith('data:image/')) {
      embeddedAssets[assetId] = { ...metadata, data: metadata.data };
      continue;
    }
    try {
      const response = await fetch(`/api/assets/${encodeURIComponent(assetId)}`);
      if (response.ok) embeddedAssets[assetId] = { ...metadata, data: bytesToDataUrl(await response.arrayBuffer(), response.headers.get('content-type') || metadata.mime) };
    } catch { /* El proyecto sigue siendo exportable aunque falte un recurso. */ }
  }
  const payload = { exportVersion: 1, project, embeddedAssets };
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = `${project.name.toLowerCase().replace(/[^a-z0-9áéíóúñ]+/gi, '-') || 'mieink'}.json`;
  link.click();
  URL.revokeObjectURL(link.href);
  toast('Proyecto exportado.');
}

async function importProjectFile(file) {
  const parsed = JSON.parse(await file.text());
  const incoming = sanitizeProject(parsed.project || parsed);
  const remap = {};
  for (const [oldId, asset] of Object.entries(parsed.embeddedAssets || {})) {
    if (!asset.data?.startsWith('data:image/')) continue;
    const response = await fetch(asset.data);
    const uploaded = await uploadAsset(new File([await response.blob()], asset.name || 'imagen', { type: asset.mime || response.headers.get('content-type') }));
    remap[oldId] = uploaded.id;
    incoming.assets[uploaded.id] = uploaded;
  }
  for (const page of incoming.pages) for (const widget of page.widgets) if (widget.type === 'image' && remap[widget.config.assetId]) widget.config.assetId = remap[widget.config.assetId];
  for (const oldId of Object.keys(remap)) delete incoming.assets[oldId];
  undoStack.push(deepClone(project));
  project = incoming;
  redoStack = [];
  selectedWidgetId = null;
  runtime = { blank: false, inverted: false };
  renderAll();
  scheduleAutoRefresh();
  queueSave();
  toast('Proyecto importado.');
}

function deviceSelectOptions(selected) {
  return BUTTON_ACTIONS.map((action) => `<option value="${action}" ${selected === action ? 'selected' : ''}>${ACTION_LABELS[action]}</option>`).join('');
}

function homeNetworkValue() {
  const form = $('#device-form');
  return form.elements.homeSsid.value === '__custom__'
    ? String(form.elements.homeSsidCustom.value || '').trim()
    : String(form.elements.homeSsid.value || '').trim();
}

function renderNetworkOptions(networks = [], selected = '') {
  const select = $('#home-network-select');
  const unique = new Map(networks.filter((network) => network?.ssid).map((network) => [network.ssid, network]));
  if (selected && !unique.has(selected)) unique.set(selected, { ssid: selected, rssi: null, security: 'guardada' });
  const options = [...unique.values()].sort((left, right) => Number(right.rssi ?? -999) - Number(left.rssi ?? -999));
  select.innerHTML = [
    '<option value="">Selecciona una red</option>',
    ...options.map((network) => {
      const signal = network.rssi == null ? '' : network.rssi >= -55 ? ' · excelente' : network.rssi >= -70 ? ' · buena' : ' · débil';
      const lock = network.open ? ' · abierta' : ' · segura';
      return `<option value="${escapeHtml(network.ssid)}" ${network.ssid === selected ? 'selected' : ''}>${escapeHtml(network.ssid + signal + lock)}</option>`;
    }),
    '<option value="__custom__">Otra red / SSID oculto…</option>',
  ].join('');
}

function updateCustomNetworkVisibility() {
  const custom = $('#home-network-select').value === '__custom__';
  $('#custom-network-field').hidden = !custom;
  if (custom) $('#device-form').elements.homeSsidCustom.focus();
}

function networkStatusText(network) {
  if (network.connected) return `Conectado a ${network.homeSsid} · ${network.ip} · ${network.rssi} dBm${network.weatherConfigured ? '' : ' · falta clave OpenWeather'}`;
  if (network.state === 'connecting') return `Conectando a ${network.homeSsid}… El AP ${network.apSsid || 'MiEInk'} continúa disponible.`;
  if (network.error) return `${network.error} El AP ${network.apSsid || 'MiEInk'} sigue disponible en ${network.apIp || '192.168.4.1'}.`;
  return network.configured ? `Red ${network.homeSsid} guardada, pero sin conexión.` : 'Aún no hay una red doméstica configurada.';
}

async function waitForHomeConnection(expectedSsid, timeoutMs = 25000) {
  const status = $('#network-scan-status');
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    try {
      const network = await jsonResponse(await fetch('/api/device/network', { cache: 'no-store' }));
      status.textContent = networkStatusText(network);
      if (network.connected && network.homeSsid === expectedSsid) return network;
      if (network.state === 'error' && network.error) throw new Error(network.error);
      lastError = null;
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw lastError || new Error('El router no asignó una dirección IP. Revisa que sea una red de 2,4 GHz.');
}

async function scanHomeNetworks({ quiet = false } = {}) {
  const status = $('#network-scan-status');
  const button = $('#scan-networks');
  const selected = homeNetworkValue() || project.device.network.homeSsid;
  button.disabled = true;
  status.textContent = 'Buscando redes Wi‑Fi cercanas…';
  try {
    const data = await jsonResponse(await fetch('/api/device/networks', { cache: 'no-store' }));
    renderNetworkOptions(data.networks || [], selected);
    updateCustomNetworkVisibility();
    status.textContent = `${data.networks?.length || 0} redes encontradas. Elige la de casa e introduce su contraseña.`;
  } catch (error) {
    renderNetworkOptions([], selected);
    status.textContent = error.message || 'No se pudieron buscar redes.';
    if (!quiet) toast(status.textContent);
  } finally {
    button.disabled = false;
  }
}

async function openDeviceDialog() {
  const form = $('#device-form');
  const display = project.device.display;
  form.elements.rotation.value = display.rotation;
  form.elements.safeMargin.value = display.safeMargin;
  form.elements.threshold.value = display.threshold;
  form.elements.invert.checked = display.invert;
  form.elements.apSsid.value = project.device.network.apSsid;
  renderNetworkOptions([], project.device.network.homeSsid);
  form.elements.homeSsid.value = project.device.network.homeSsid;
  form.elements.homeSsidCustom.value = '';
  form.elements.homePassword.value = '';
  $('#margin-output').textContent = `${display.safeMargin} px`;
  $('#threshold-output').textContent = display.threshold;
  $('#button-settings').innerHTML = ['key1', 'key2', 'key3'].map((key) => {
    const setting = project.device.buttons[key];
    return `<div class="button-setting"><strong>${key.toUpperCase()}</strong><select name="${key}Action">${deviceSelectOptions(setting.action)}</select><select class="button-target" name="${key}Page" ${setting.action === 'show_page' ? '' : 'hidden'}>${project.pages.map((page) => `<option value="${escapeHtml(page.id)}" ${setting.pageId === page.id ? 'selected' : ''}>${escapeHtml(page.name)}</option>`).join('')}</select></div>`;
  }).join('');
  $('#device-dialog').showModal();
  startDeviceStatusPolling();
  try {
    const network = await jsonResponse(await fetch('/api/device/network', { cache: 'no-store' }));
    form.elements.apSsid.value = network.apSsid || project.device.network.apSsid;
    project.device.network.apSsid = form.elements.apSsid.value;
    project.device.network.homeSsid = network.homeSsid || project.device.network.homeSsid;
    renderNetworkOptions([], project.device.network.homeSsid);
    $('#network-scan-status').textContent = networkStatusText(network);
  } catch { /* El simulador antiguo o un firmware previo pueden no exponer esta ruta. */ }
  await scanHomeNetworks({ quiet: true });
}

async function saveDevice(event) {
  event.preventDefault();
  const form = $('#device-form');
  const data = new FormData(form);
  const saveButton = $('#save-device');
  const homeSsid = homeNetworkValue();
  if (!homeSsid) {
    toast('Selecciona o escribe la red Wi‑Fi de casa.');
    return;
  }
  const nextButtons = {};
  for (const key of ['key1', 'key2', 'key3']) nextButtons[key] = { action: data.get(`${key}Action`), pageId: data.get(`${key}Page`) || null };
  commit((draft) => {
    Object.assign(draft.device.display, {
      rotation: Number(data.get('rotation')),
      safeMargin: Number(data.get('safeMargin')),
      threshold: Number(data.get('threshold')),
      refreshMode: 'full',
      invert: form.elements.invert.checked,
    });
    Object.assign(draft.device.network, { mode: 'ap_sta', apSsid: form.elements.apSsid.value, homeSsid });
    draft.device.buttons = nextButtons;
  });
  saveButton.disabled = true;
  try {
    // El adaptador Wi-Fi puede reiniciarse al guardar credenciales. Confirmar
    // primero el proyecto evita perder el cambio por esa breve desconexión.
    await persistProjectNow({ retries: 3 });
    await jsonResponse(await fetch('/api/device/network', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ homeSsid, homePassword: data.get('homePassword') }) }));
    $('#network-scan-status').textContent = `Credenciales guardadas. Conectando a ${homeSsid}…`;
    await waitForHomeConnection(homeSsid);
  } catch (error) {
    const message = error.message || 'No se pudo conectar a la red doméstica.';
    $('#network-scan-status').textContent = `${message} Corrige los datos y vuelve a intentarlo; el AP del dispositivo sigue activo.`;
    toast(message);
    return;
  } finally {
    saveButton.disabled = false;
  }
  form.elements.homePassword.value = '';
  $('#device-dialog').close();
  scheduleAutoRefresh();
  toast(physicalDevice
    ? 'Internet conectado. El AP del dispositivo continúa disponible.'
    : 'Configuración simulada guardada.');
}

async function waitForDisplayJob(job) {
  const limit = Date.now() + 30_000;
  while (Date.now() < limit) {
    const status = await jsonResponse(await fetch('/api/status', { cache: 'no-store' }));
    if (status.job === job && status.state === 'ready') return;
    if (status.job === job && status.state === 'error') throw new Error(status.message || 'La pantalla informó de un error.');
    await new Promise((resolve) => setTimeout(resolve, 160));
  }
  throw new Error('La simulación tardó demasiado en terminar.');
}

async function sendDisplay() {
  if (displayJobActive) return false;
  const button = $('#send-display');
  displayJobActive = true;
  button.disabled = true;
  try {
    await persistProjectNow({ retries: 3 });
    await updatePreview();
    const canvas = document.createElement('canvas');
    canvas.width = DISPLAY_WIDTH; canvas.height = DISPLAY_HEIGHT;
    const agenda = { ...project.agenda, holidays: [...project.agenda.holidays.manual, ...spanishHolidays] };
    await renderPage(canvas, project, activePage(), {
      runtimeInvert: runtime.inverted,
      blank: runtime.blank,
      agenda,
      telemetry: deviceTelemetry,
      assetResolver: assetUrl,
      deferDisplayTransform: true,
    });
    const bmp = canvasToMonoBmp(canvas, project.device.display.threshold);
    const accepted = await jsonResponse(await fetch('/api/display', {
      method: 'POST',
      headers: {
        'Content-Type': 'image/bmp',
        'X-Page-Id': project.activePageId,
        'X-Runtime-Invert': runtime.inverted ? '1' : '0',
        'X-Runtime-Blank': runtime.blank ? '1' : '0',
      },
      body: bmp,
    }));
    await waitForDisplayJob(accepted.job);
    toast(`${physicalDevice ? 'Pantalla actualizada' : 'Pantalla simulada'} · trabajo ${accepted.job}`);
    return true;
  } catch (error) {
    toast(error.message || 'No se pudo actualizar la pantalla.');
    return false;
  } finally {
    displayJobActive = false;
    button.disabled = false;
  }
}

function attachEvents() {
  $('#project-name').addEventListener('change', (event) => commit((draft) => { draft.name = event.target.value; }));
  $('#page-name').addEventListener('change', (event) => commit(() => { activePage().name = event.target.value || 'Sin título'; }));
  $('#undo').addEventListener('click', () => historySwap(undoStack, redoStack));
  $('#redo').addEventListener('click', () => historySwap(redoStack, undoStack));
  $('#add-page').addEventListener('click', () => commit((draft) => addPage(draft)));
  $('#duplicate-page').addEventListener('click', () => commit((draft) => addPage(draft, draft.activePageId)));
  $('#delete-page').addEventListener('click', () => commit((draft) => deletePage(draft, draft.activePageId)));
  $('#page-list').addEventListener('click', (event) => {
    const row = event.target.closest('.page-row');
    if (!row) return;
    const movement = event.target.closest('[data-move]');
    if (movement) {
      event.stopPropagation();
      commit((draft) => movePage(draft, row.dataset.pageId, Number(movement.dataset.move)));
      return;
    }
    project.activePageId = row.dataset.pageId;
    selectedWidgetId = null;
    runtime.blank = false;
    renderAll();
    scheduleAutoRefresh();
    queueSave();
  });
  $('#tool-grid').addEventListener('click', (event) => { const tool = event.target.closest('[data-widget-type]'); if (tool) addWidget(tool.dataset.widgetType); });
  $('#tool-grid').addEventListener('dragstart', (event) => { const tool = event.target.closest('[data-widget-type]'); if (tool) event.dataTransfer.setData('application/x-mieink-widget', tool.dataset.widgetType); });
  const surface = $('#design-surface');
  surface.addEventListener('dragover', (event) => {
    const type = event.dataTransfer.types.includes('application/x-mieink-widget') ? 'pending' : null;
    if (!type) return;
    event.preventDefault();
    const position = gridPosition(event);
    showGuide({ ...position, w: 2, h: 2 }, true);
  });
  surface.addEventListener('dragleave', hideGuide);
  surface.addEventListener('drop', (event) => {
    event.preventDefault();
    hideGuide();
    const type = event.dataTransfer.getData('application/x-mieink-widget');
    if (type) addWidget(type, gridPosition(event));
  });
  $('#widget-layer').addEventListener('pointerdown', (event) => {
    const element = event.target.closest('.canvas-widget');
    if (!element) return;
    const widget = activePage().widgets.find((item) => item.id === element.dataset.widgetId);
    if (widget) beginPointerEdit(event, widget, Boolean(event.target.closest('.resize-handle')));
  });
  surface.addEventListener('pointerdown', (event) => {
    if (event.target === surface || event.target.id === 'widget-layer') { selectedWidgetId = null; renderAll(); }
  });
  $$('.view-switch button').forEach((button) => button.addEventListener('click', () => { currentView = button.dataset.view; renderAll(); }));
  $('#send-display').addEventListener('click', sendDisplay);
  $('#duplicate-widget').addEventListener('click', () => {
    const source = selectedWidget();
    if (!source) return;
    const position = findFreePosition(activePage(), source.w, source.h);
    if (!position) return toast('No hay espacio para duplicar este widget.');
    const copy = createWidget(source.type, activePage(), { ...position, w: source.w, h: source.h, config: deepClone(source.config) });
    commit(() => activePage().widgets.push(copy));
    selectedWidgetId = copy.id;
    renderAll();
  });
  $('#lock-widget').addEventListener('click', () => { const widget = selectedWidget(); if (widget) commit(() => { widget.locked = !widget.locked; }); });
  $('#delete-widget').addEventListener('click', () => { if (!selectedWidget()) return; commit(() => { activePage().widgets = activePage().widgets.filter((widget) => widget.id !== selectedWidgetId); }); selectedWidgetId = null; renderAll(); });
  $('#inspector-content').addEventListener('change', async (event) => {
    const widget = selectedWidget();
    if (!widget) return;
    if (event.target.matches('[data-config]')) commit(() => {
      const key = event.target.dataset.config;
      widget.config[key] = configValue(event.target);
      if (widget.type === 'weather' && key === 'location') {
        widget.config.latitude = null;
        widget.config.longitude = null;
        widget.config.resolvedLocation = '';
      }
    });
    if (event.target.matches('[data-geometry]')) {
      const candidate = { ...widget, [event.target.dataset.geometry]: Number(event.target.value) };
      if (canPlace(activePage(), candidate, widget.id)) commit(() => { widget[event.target.dataset.geometry] = Number(event.target.value); });
      else { toast('Ese tamaño choca con otro widget o sale del lienzo.'); renderInspector(); }
    }
    if (event.target.matches('[data-image-upload]') && event.target.files[0]) {
      try {
        const asset = await uploadAsset(event.target.files[0]);
        commit((draft) => { draft.assets[asset.id] = asset; widget.config.assetId = asset.id; });
        toast('Imagen añadida.');
      } catch (error) { toast(error.message); }
    }
  });
  $('#inspector-content').addEventListener('click', (event) => {
    const widget = selectedWidget();
    if (!widget) return;
    if (event.target.dataset.inspectorAction === 'weather') refreshWeather(widget, { force: true });
    if (event.target.dataset.inspectorAction === 'agenda') openAgendaDialog();
    if (event.target.dataset.inspectorAction === 'device-status') openDeviceDialog();
  });
  $$('.virtual-keys button').forEach((button) => button.addEventListener('click', () => {
    runtime = applyButtonAction(project, button.dataset.key, runtime);
    selectedWidgetId = null;
    renderAll();
    scheduleAutoRefresh();
    queueSave();
    toast(`${button.dataset.key.toUpperCase()}: ${buttonText(project.device.buttons[button.dataset.key])}`);
  }));
  $('#open-device').addEventListener('click', openDeviceDialog);
  $('#open-agenda').addEventListener('click', openAgendaDialog);
  $('.dialog-close').addEventListener('click', () => $('#device-dialog').close());
  $('#device-dialog').addEventListener('close', stopDeviceStatusPolling);
  $('#device-form').addEventListener('submit', saveDevice);
  $('#scan-networks').addEventListener('click', () => scanHomeNetworks());
  $('#home-network-select').addEventListener('change', updateCustomNetworkVisibility);
  $('#device-form').addEventListener('input', (event) => {
    if (event.target.name === 'safeMargin') $('#margin-output').textContent = `${event.target.value} px`;
    if (event.target.name === 'threshold') $('#threshold-output').textContent = event.target.value;
  });
  $('#button-settings').addEventListener('change', (event) => {
    if (!event.target.name?.endsWith('Action')) return;
    event.target.closest('.button-setting').querySelector('.button-target').hidden = event.target.value !== 'show_page';
  });
  $('.agenda-close').addEventListener('click', () => $('#agenda-dialog').close());
  $('#add-agenda-event').addEventListener('click', () => {
    commit((draft) => draft.agenda.events.push({ id: `event-${Date.now()}-${Math.random().toString(36).slice(2)}`, date: new Date().toISOString().slice(0, 10), start: '', end: '', title: 'Nuevo evento', allDay: true }));
    renderAgendaDialog();
  });
  $('#add-holiday').addEventListener('click', () => {
    commit((draft) => draft.agenda.holidays.manual.push({ id: `holiday-${Date.now()}-${Math.random().toString(36).slice(2)}`, date: new Date().toISOString().slice(0, 10), title: 'Festivo local', scope: 'Local' }));
    renderAgendaDialog();
  });
  $('#agenda-dialog').addEventListener('change', (event) => {
    const eventRow = event.target.closest('[data-agenda-event]');
    if (eventRow && event.target.matches('[data-agenda-field]')) {
      const eventId = eventRow.dataset.agendaEvent;
      commit((draft) => { const item = draft.agenda.events.find((entry) => entry.id === eventId); if (item) item[event.target.dataset.agendaField] = event.target.type === 'checkbox' ? event.target.checked : event.target.value; });
      renderAgendaDialog();
      return;
    }
    const holidayRow = event.target.closest('[data-holiday]');
    if (holidayRow && event.target.matches('[data-holiday-field]')) {
      const holidayId = holidayRow.dataset.holiday;
      commit((draft) => { const item = draft.agenda.holidays.manual.find((entry) => entry.id === holidayId); if (item) item[event.target.dataset.holidayField] = event.target.value; });
      renderAgendaDialog();
      return;
    }
    if (event.target.id === 'spain-holidays-enabled' || event.target.id === 'spain-holiday-region') {
      commit((draft) => { draft.agenda.holidays.spain.enabled = $('#spain-holidays-enabled').checked; draft.agenda.holidays.spain.region = $('#spain-holiday-region').value; });
      refreshSpainHolidays({ force: true });
    }
  });
  $('#agenda-dialog').addEventListener('click', (event) => {
    const eventId = event.target.dataset.removeAgendaEvent;
    if (eventId) { commit((draft) => { draft.agenda.events = draft.agenda.events.filter((item) => item.id !== eventId); }); renderAgendaDialog(); }
    const holidayId = event.target.dataset.removeHoliday;
    if (holidayId) { commit((draft) => { draft.agenda.holidays.manual = draft.agenda.holidays.manual.filter((item) => item.id !== holidayId); }); renderAgendaDialog(); }
    if (event.target.id === 'refresh-holidays') refreshSpainHolidays({ force: true });
  });
  $('#export-project').addEventListener('click', exportProject);
  $('#import-project').addEventListener('click', () => $('#project-file').click());
  $('#project-file').addEventListener('change', async (event) => {
    if (!event.target.files[0]) return;
    try { await importProjectFile(event.target.files[0]); }
    catch (error) { toast(`No se pudo importar: ${error.message}`); }
    event.target.value = '';
  });
  surface.addEventListener('keydown', (event) => {
    const widget = selectedWidget();
    if (!widget || widget.locked || !['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Delete'].includes(event.key)) return;
    event.preventDefault();
    if (event.key === 'Delete') { $('#delete-widget').click(); return; }
    const delta = { ArrowLeft: [-1,0], ArrowRight: [1,0], ArrowUp: [0,-1], ArrowDown: [0,1] }[event.key];
    const candidate = event.shiftKey ? { ...widget, w: widget.w + delta[0], h: widget.h + delta[1] } : { ...widget, x: widget.x + delta[0], y: widget.y + delta[1] };
    if (canPlace(activePage(), candidate, widget.id)) commit(() => Object.assign(widget, candidate));
  });
}

async function initializeApp() {
  hydrateMaterialIcons(document);
  await loadProject();
  await detectRuntime();
  attachEvents();
  renderAll();
  scheduleAutoRefresh();
  loadDeviceStatus();
  if (project.agenda.holidays.spain.enabled) refreshSpainHolidays();
  // Hora, fecha y calendario se redibujan siempre. El temporizador de arriba
  // convierte esa nueva composición en BMP y la manda al panel cuando está
  // configurado un intervalo automático.
  setInterval(refreshVisibleLiveWidgets, 60_000);
  clearInterval(weatherRefreshTimer);
  weatherRefreshTimer = setInterval(refreshStaleWeather, 60_000);
  refreshStaleWeather();
  document.addEventListener('visibilitychange', async () => {
    if (!document.hidden) {
      await refreshStaleWeather();
      scheduleAutoRefresh();
    }
  });
  window.addEventListener('pagehide', () => {
    mirrorProjectInBrowser();
    const payload = JSON.stringify(project);
    if (savedRevision < saveRevision && payload.length < 60_000) {
      fetch('/api/project', {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: payload, keepalive: true,
      }).catch(() => {});
    }
  });
}

if (typeof document !== 'undefined') {
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initializeApp, { once: true });
  else initializeApp();
}
